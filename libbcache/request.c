/*
 * Handle a read or a write request and decide what to do with it.
 *
 * Copyright 2010, 2011 Kent Overstreet <kent.overstreet@gmail.com>
 * Copyright 2012 Google, Inc.
 *
 * Main pieces here:
 *
 * 1) Data insert path, via bch_data_insert() -- writes data to cache and
 *    updates extents btree
 * 2) Read path, via bch_read() -- for now only used by bcachefs and ioctl
 *    interface
 * 3) Read path, via cache_lookup() and struct search -- used by block device
 *    make_request functions
 * 4) Cache promotion -- used by bch_read() and cache_lookup() to copy data to
 *    the cache, either from a backing device or a cache device in a higher tier
 *
 * One tricky thing that comes up is a race condition where a bucket may be
 * re-used while reads from it are still in flight. To guard against this, we
 * save the ptr that is being read and check if it is stale once the read
 * completes. If the ptr is stale, the read is retried.
 *
 * #2 and #3 will be unified further in the future.
 */

#include "bcache.h"
#include "blockdev.h"
#include "btree_update.h"
#include "btree_iter.h"
#include "clock.h"
#include "debug.h"
#include "error.h"
#include "extents.h"
#include "io.h"
#include "journal.h"
#include "keybuf.h"
#include "request.h"
#include "writeback.h"
#include "stats.h"

#include <linux/module.h>
#include <linux/hash.h>
#include <linux/random.h>
#include <linux/backing-dev.h>

#include <trace/events/bcache.h>

#define CUTOFF_CACHE_ADD	10
#define CUTOFF_CACHE_READA	15

/* Congested? */

unsigned bch_get_congested(struct cache_set *c)
{
	int i;
	long rand;

	if (!c->congested_read_threshold_us &&
	    !c->congested_write_threshold_us)
		return 0;

	i = (local_clock_us() - c->congested_last_us) / 1024;
	if (i < 0)
		return 0;

	i += atomic_read(&c->congested);
	if (i >= 0)
		return 0;

	i += CONGESTED_MAX;

	if (i > 0)
		i = fract_exp_two(i, 6);

	rand = get_random_int();
	i -= bitmap_weight(&rand, BITS_PER_LONG);

	return i > 0 ? i : 1;
}

static void add_sequential(struct task_struct *t)
{
	t->sequential_io_avg = ewma_add(t->sequential_io_avg,
					t->sequential_io, 3);
	t->sequential_io = 0;
}

static struct hlist_head *iohash(struct cached_dev *dc, uint64_t k)
{
	return &dc->io_hash[hash_64(k, RECENT_IO_BITS)];
}

static bool check_should_bypass(struct cached_dev *dc, struct bio *bio, int rw)
{
	struct cache_set *c = dc->disk.c;
	unsigned mode = BDEV_CACHE_MODE(dc->disk_sb.sb);
	unsigned sectors, congested = bch_get_congested(c);
	struct task_struct *task = current;
	struct io *i;

	if (test_bit(BCACHE_DEV_DETACHING, &dc->disk.flags) ||
	    sectors_available(c) * 100 < c->capacity * CUTOFF_CACHE_ADD ||
	    (bio_op(bio) == REQ_OP_DISCARD))
		goto skip;

	if (mode == CACHE_MODE_NONE ||
	    (mode == CACHE_MODE_WRITEAROUND &&
	     op_is_write(bio_op(bio))))
		goto skip;

	if (bio->bi_iter.bi_sector & (c->sb.block_size - 1) ||
	    bio_sectors(bio) & (c->sb.block_size - 1)) {
		pr_debug("skipping unaligned io");
		goto skip;
	}

	if (bypass_torture_test(dc)) {
		if ((get_random_int() & 3) == 3)
			goto skip;
		else
			goto rescale;
	}

	if (!congested && !dc->sequential_cutoff)
		goto rescale;

	if (!congested &&
	    mode == CACHE_MODE_WRITEBACK &&
	    op_is_write(bio_op(bio)) &&
	    (bio->bi_opf & REQ_SYNC))
		goto rescale;

	spin_lock(&dc->io_lock);

	hlist_for_each_entry(i, iohash(dc, bio->bi_iter.bi_sector), hash)
		if (i->last == bio->bi_iter.bi_sector &&
		    time_before(jiffies, i->last_io))
			goto found;

	i = list_first_entry(&dc->io_lru, struct io, lru);

	add_sequential(task);
	i->sequential = 0;
found:
	if (i->sequential + bio->bi_iter.bi_size > i->sequential)
		i->sequential	+= bio->bi_iter.bi_size;

	i->last			 = bio_end_sector(bio);
	i->last_io		 = jiffies + msecs_to_jiffies(5000);
	task->sequential_io	 = i->sequential;

	hlist_del(&i->hash);
	hlist_add_head(&i->hash, iohash(dc, i->last));
	list_move_tail(&i->lru, &dc->io_lru);

	spin_unlock(&dc->io_lock);

	sectors = max(task->sequential_io,
		      task->sequential_io_avg) >> 9;

	if (dc->sequential_cutoff &&
	    sectors >= dc->sequential_cutoff >> 9) {
		trace_bcache_bypass_sequential(bio);
		goto skip;
	}

	if (congested && sectors >= congested) {
		trace_bcache_bypass_congested(bio);
		goto skip;
	}

rescale:
	return false;
skip:
	bch_mark_sectors_bypassed(c, dc, bio_sectors(bio));
	return true;
}

/* Common code for the make_request functions */

/**
 * request_endio - endio function for backing device bios
 */
static void request_endio(struct bio *bio)
{
	struct closure *cl = bio->bi_private;

	if (bio->bi_error) {
		struct search *s = container_of(cl, struct search, cl);
		s->iop.error = bio->bi_error;
		/* Only cache read errors are recoverable */
		s->recoverable = false;
	}

	bio_put(bio);
	closure_put(cl);
}

static void bio_complete(struct search *s)
{
	if (s->orig_bio) {
		generic_end_io_acct(bio_data_dir(s->orig_bio),
				    &s->d->disk->part0, s->start_time);

		trace_bcache_request_end(s->d, s->orig_bio);
		s->orig_bio->bi_error = s->iop.error;
		bio_endio(s->orig_bio);
		s->orig_bio = NULL;
	}
}

static void do_bio_hook(struct search *s, struct bio *orig_bio)
{
	int rw = bio_data_dir(orig_bio);
	struct bio *bio = rw ? &s->wbio.bio : &s->rbio.bio;

	bio_init(bio);
	__bio_clone_fast(bio, orig_bio);
	bio->bi_end_io		= request_endio;
	bio->bi_private		= &s->cl;

	bio_cnt_set(bio, 3);
}

static void search_free(struct closure *cl)
{
	struct search *s = container_of(cl, struct search, cl);

	bio_complete(s);

	if (s->iop.bio)
		bio_put(&s->iop.bio->bio);

	closure_debug_destroy(cl);
	mempool_free(s, &s->d->c->search);
}

static inline struct search *search_alloc(struct bio *bio,
					  struct bcache_device *d)
{
	struct search *s;

	s = mempool_alloc(&d->c->search, GFP_NOIO);

	closure_init(&s->cl, NULL);
	do_bio_hook(s, bio);

	s->orig_bio		= bio;
	s->d			= d;
	s->recoverable		= 1;
	s->bypass		= 0;
	s->write		= op_is_write(bio_op(bio));
	s->read_dirty_data	= 0;
	s->cache_miss		= 0;
	s->start_time		= jiffies;
	s->inode		= bcache_dev_inum(d);

	s->iop.c		= d->c;
	s->iop.bio		= NULL;
	s->iop.error		= 0;

	return s;
}

/* Cached devices */

static void cached_dev_bio_complete(struct closure *cl)
{
	struct search *s = container_of(cl, struct search, cl);
	struct cached_dev *dc = container_of(s->d, struct cached_dev, disk);

	search_free(cl);
	cached_dev_put(dc);
}

/* Process reads */

static void cached_dev_read_error(struct closure *cl)
{
	struct search *s = container_of(cl, struct search, cl);
	struct bio *bio = &s->rbio.bio;

	if (s->recoverable) {
		/* Read bucket invalidate races are handled here, also plain
		 * old IO errors from the cache that can be retried from the
		 * backing device (reads of clean data) */
		trace_bcache_read_retry(s->orig_bio);

		s->iop.error = 0;
		do_bio_hook(s, s->orig_bio);

		/* XXX: invalidate cache, don't count twice */

		closure_bio_submit(bio, cl);
	}

	continue_at(cl, cached_dev_bio_complete, NULL);
}

static void cached_dev_read_done(struct closure *cl)
{
	struct search *s = container_of(cl, struct search, cl);
	struct cached_dev *dc = container_of(s->d, struct cached_dev, disk);

	if (dc->verify && s->recoverable && !s->read_dirty_data)
		bch_data_verify(dc, s->orig_bio);

	continue_at_nobarrier(cl, cached_dev_bio_complete, NULL);
}

static void cached_dev_read_done_bh(struct closure *cl)
{
	struct search *s = container_of(cl, struct search, cl);
	struct cached_dev *dc = container_of(s->d, struct cached_dev, disk);

	bch_mark_cache_accounting(s->iop.c, dc, !s->cache_miss, s->bypass);
	trace_bcache_read(s->orig_bio, !s->cache_miss, s->bypass);

	if (s->iop.error)
		continue_at_nobarrier(cl, cached_dev_read_error, s->iop.c->wq);
	else if (dc->verify)
		continue_at_nobarrier(cl, cached_dev_read_done, s->iop.c->wq);
	else
		continue_at_nobarrier(cl, cached_dev_bio_complete, NULL);
}

/**
 * __cache_promote -- insert result of read bio into cache
 *
 * Used for backing devices and flash-only volumes.
 *
 * @orig_bio must actually be a bbio with a valid key.
 */
void __cache_promote(struct cache_set *c, struct bch_read_bio *orig_bio,
		     struct bkey_s_c old,
		     struct bkey_s_c new,
		     unsigned write_flags)
{
#if 0
	struct cache_promote_op *op;
	struct bio *bio;
	unsigned pages = DIV_ROUND_UP(orig_bio->bio.bi_iter.bi_size, PAGE_SIZE);

	/* XXX: readahead? */

	op = kmalloc(sizeof(*op) + sizeof(struct bio_vec) * pages, GFP_NOIO);
	if (!op)
		goto out_submit;

	/* clone the bbio */
	memcpy(&op->bio, orig_bio, offsetof(struct bbio, bio));

	bio = &op->bio.bio.bio;
	bio_init(bio);
	bio_get(bio);
	bio->bi_bdev		= orig_bio->bio.bi_bdev;
	bio->bi_iter.bi_sector	= orig_bio->bio.bi_iter.bi_sector;
	bio->bi_iter.bi_size	= orig_bio->bio.bi_iter.bi_size;
	bio->bi_end_io		= cache_promote_endio;
	bio->bi_private		= &op->cl;
	bio->bi_io_vec		= bio->bi_inline_vecs;
	bch_bio_map(bio, NULL);

	if (bio_alloc_pages(bio, __GFP_NOWARN|GFP_NOIO))
		goto out_free;

	orig_bio->ca = NULL;

	closure_init(&op->cl, &c->cl);
	op->orig_bio		= &orig_bio->bio;
	op->stale		= 0;

	bch_write_op_init(&op->iop, c, &op->bio, &c->promote_write_point,
			  new, old,
			  BCH_WRITE_ALLOC_NOWAIT|write_flags);
	op->iop.nr_replicas = 1;

	//bch_cut_front(bkey_start_pos(&orig_bio->key.k), &op->iop.insert_key);
	//bch_cut_back(orig_bio->key.k.p, &op->iop.insert_key.k);

	trace_bcache_promote(&orig_bio->bio);

	op->bio.bio.submit_time_us = local_clock_us();
	closure_bio_submit(bio, &op->cl);

	continue_at(&op->cl, cache_promote_write, c->wq);
out_free:
	kfree(op);
out_submit:
	generic_make_request(&orig_bio->bio);
#endif
}

/**
 * cached_dev_cache_miss - populate cache with data from backing device
 *
 * We don't write to the cache if s->bypass is set.
 */
static int cached_dev_cache_miss(struct btree_iter *iter, struct search *s,
				 struct bio *bio, unsigned sectors)
{
	int ret;
	unsigned reada = 0;
	struct bio *miss;
	BKEY_PADDED(key) replace;

	s->cache_miss = 1;

	if (s->bypass)
		goto nopromote;
#if 0
	struct cached_dev *dc = container_of(s->d, struct cached_dev, disk);

	/* XXX: broken */
	if (!(bio->bi_opf & REQ_RAHEAD) &&
	    !(bio->bi_opf & REQ_META) &&
	    ((u64) sectors_available(dc->disk.c) * 100 <
	     (u64) iter->c->capacity * CUTOFF_CACHE_READA))
		reada = min_t(sector_t, dc->readahead >> 9,
			      bdev_sectors(bio->bi_bdev) - bio_end_sector(bio));
#endif
	sectors = min(sectors, bio_sectors(bio) + reada);

	replace.key.k = KEY(s->inode,
			    bio->bi_iter.bi_sector + sectors,
			    sectors);

	ret = bch_btree_insert_check_key(iter, &replace.key);
	if (ret == -EINTR)
		return ret;

	miss = bio_next_split(bio, sectors, GFP_NOIO, &s->d->bio_split);

	miss->bi_end_io		= request_endio;
	miss->bi_private	= &s->cl;

	//to_bbio(miss)->key.k = KEY(s->inode,
	//			   bio_end_sector(miss),
	//			   bio_sectors(miss));
	to_rbio(miss)->ca = NULL;

	closure_get(&s->cl);
	__cache_promote(s->iop.c, to_rbio(miss),
			bkey_i_to_s_c(&replace.key),
			bkey_to_s_c(&KEY(replace.key.k.p.inode,
					 replace.key.k.p.offset,
					 replace.key.k.size)),
			BCH_WRITE_CACHED);

	return 0;
nopromote:
	miss = bio_next_split(bio, sectors, GFP_NOIO, &s->d->bio_split);

	miss->bi_end_io		= request_endio;
	miss->bi_private	= &s->cl;
	closure_bio_submit(miss, &s->cl);

	return 0;
}

static void cached_dev_read(struct cached_dev *dc, struct search *s)
{
	struct cache_set *c = s->iop.c;
	struct closure *cl = &s->cl;
	struct bio *bio = &s->rbio.bio;
	struct btree_iter iter;
	struct bkey_s_c k;
	int ret;

	bch_increment_clock(c, bio_sectors(bio), READ);

	for_each_btree_key_with_holes(&iter, c, BTREE_ID_EXTENTS,
				POS(s->inode, bio->bi_iter.bi_sector), k) {
		BKEY_PADDED(k) tmp;
		struct extent_pick_ptr pick;
		unsigned sectors, bytes;
		bool is_last;
retry:
		bkey_reassemble(&tmp.k, k);
		bch_btree_iter_unlock(&iter);
		k = bkey_i_to_s_c(&tmp.k);

		bch_extent_pick_ptr(c, k, &pick);
		if (IS_ERR(pick.ca)) {
			bcache_io_error(c, bio, "no device to read from");
			goto out;
		}

		sectors = min_t(u64, k.k->p.offset, bio_end_sector(bio)) -
			bio->bi_iter.bi_sector;
		bytes = sectors << 9;
		is_last = bytes == bio->bi_iter.bi_size;
		swap(bio->bi_iter.bi_size, bytes);

		if (pick.ca) {
			PTR_BUCKET(pick.ca, &pick.ptr)->read_prio =
				c->prio_clock[READ].hand;

			if (!bkey_extent_is_cached(k.k))
				s->read_dirty_data = true;

			bch_read_extent(c, &s->rbio, k, &pick,
					BCH_READ_FORCE_BOUNCE|
					BCH_READ_RETRY_IF_STALE|
					(!s->bypass ? BCH_READ_PROMOTE : 0)|
					(is_last ? BCH_READ_IS_LAST : 0));
		} else {
			/* not present (hole), or stale cached data */
			if (cached_dev_cache_miss(&iter, s, bio, sectors)) {
				k = bch_btree_iter_peek_with_holes(&iter);
				if (btree_iter_err(k))
					break;
				goto retry;
			}
		}

		swap(bio->bi_iter.bi_size, bytes);
		bio_advance(bio, bytes);

		if (is_last) {
			bch_btree_iter_unlock(&iter);
			goto out;
		}
	}

	/*
	 * If we get here, it better have been because there was an error
	 * reading a btree node
	 */
	ret = bch_btree_iter_unlock(&iter);
	BUG_ON(!ret);
	bcache_io_error(c, bio, "btree IO error %i", ret);
out:
	continue_at(cl, cached_dev_read_done_bh, NULL);
}

/* Process writes */

static void cached_dev_write_complete(struct closure *cl)
{
	struct search *s = container_of(cl, struct search, cl);
	struct cached_dev *dc = container_of(s->d, struct cached_dev, disk);

	up_read_non_owner(&dc->writeback_lock);
	cached_dev_bio_complete(cl);
}

static void cached_dev_write(struct cached_dev *dc, struct search *s)
{
	struct closure *cl = &s->cl;
	struct bio *bio = &s->wbio.bio;
	bool writeback = false;
	bool bypass = s->bypass;
	struct bkey insert_key = KEY(s->inode,
				     bio_end_sector(bio),
				     bio_sectors(bio));
	unsigned flags = BCH_WRITE_DISCARD_ON_ERROR;
	struct disk_reservation zero_reservation;

	down_read_non_owner(&dc->writeback_lock);
	if (bch_keybuf_check_overlapping(&dc->writeback_keys,
					 bkey_start_pos(&insert_key),
					 insert_key.p)) {
		/*
		 * We overlap with some dirty data undergoing background
		 * writeback, force this write to writeback
		 */
		bypass = false;
		writeback = true;
	}

	/*
	 * Discards aren't _required_ to do anything, so skipping if
	 * check_overlapping returned true is ok
	 *
	 * But check_overlapping drops dirty keys for which io hasn't started,
	 * so we still want to call it.
	 */
	if (bio_op(bio) == REQ_OP_DISCARD)
		bypass = true;

	if (should_writeback(dc, bio, BDEV_CACHE_MODE(dc->disk_sb.sb),
			     bypass)) {
		bypass = false;
		writeback = true;
	}

	if (bypass) {
		/*
		 * If this is a bypass-write (as opposed to a discard), send
		 * it down to the backing device. If this is a discard, only
		 * send it to the backing device if the backing device
		 * supports discards. Otherwise, we simply discard the key
		 * range from the cache and don't touch the backing device.
		 */
		if ((bio_op(bio) != REQ_OP_DISCARD) ||
		    blk_queue_discard(bdev_get_queue(dc->disk_sb.bdev)))
			closure_bio_submit(s->orig_bio, cl);
	} else if (writeback) {
		bch_writeback_add(dc);

		if (bio->bi_opf & REQ_PREFLUSH) {
			/* Also need to send a flush to the backing device */
			struct bio *flush = bio_alloc_bioset(GFP_NOIO, 0,
							     &dc->disk.bio_split);

			flush->bi_bdev	= bio->bi_bdev;
			flush->bi_end_io = request_endio;
			flush->bi_private = cl;
			bio_set_op_attrs(flush, REQ_OP_WRITE, WRITE_FLUSH);

			closure_bio_submit(flush, cl);
		}
	} else {
		struct bio *writethrough =
			bio_clone_fast(bio, GFP_NOIO, &dc->disk.bio_split);

		closure_bio_submit(writethrough, cl);

		flags |= BCH_WRITE_CACHED;
		flags |= BCH_WRITE_ALLOC_NOWAIT;
	}

	if (bio->bi_opf & (REQ_PREFLUSH|REQ_FUA))
		flags |= BCH_WRITE_FLUSH;
	if (bypass)
		flags |= BCH_WRITE_DISCARD;

	bch_zero(zero_reservation);
	bch_write_op_init(&s->iop, dc->disk.c, &s->wbio,
			  zero_reservation,
			  foreground_write_point(dc->disk.c,
					(unsigned long) current),
			  bkey_start_pos(&insert_key),
			  NULL, flags);

	closure_call(&s->iop.cl, bch_write, NULL, cl);
	continue_at(cl, cached_dev_write_complete, NULL);
}

/* Cached devices - read & write stuff */

static void __cached_dev_make_request(struct request_queue *q, struct bio *bio)
{
	struct search *s;
	struct bcache_device *d = bio->bi_bdev->bd_disk->private_data;
	struct cached_dev *dc = container_of(d, struct cached_dev, disk);
	int rw = bio_data_dir(bio);

	generic_start_io_acct(rw, bio_sectors(bio), &d->disk->part0);

	bio->bi_bdev = dc->disk_sb.bdev;
	bio->bi_iter.bi_sector += le64_to_cpu(dc->disk_sb.sb->data_offset);

	if (cached_dev_get(dc)) {
		struct bio *clone;

		s = search_alloc(bio, d);
		trace_bcache_request_start(s->d, bio);

		clone = rw ? &s->wbio.bio : &s->rbio.bio;

		if (!bio->bi_iter.bi_size) {
			if (s->orig_bio->bi_opf & (REQ_PREFLUSH|REQ_FUA))
				bch_journal_flush_async(&s->iop.c->journal,
							&s->cl);

			/*
			 * If it's a flush, we send the flush to the backing
			 * device too
			 */
			closure_bio_submit(clone, &s->cl);

			continue_at(&s->cl, cached_dev_bio_complete, NULL);
		} else {
			s->bypass = check_should_bypass(dc, bio, rw);

			if (rw)
				cached_dev_write(dc, s);
			else
				cached_dev_read(dc, s);
		}
	} else {
		if ((bio_op(bio) == REQ_OP_DISCARD) &&
		    !blk_queue_discard(bdev_get_queue(dc->disk_sb.bdev)))
			bio_endio(bio);
		else
			generic_make_request(bio);
	}
}

static blk_qc_t cached_dev_make_request(struct request_queue *q,
					struct bio *bio)
{
	__cached_dev_make_request(q, bio);
	return BLK_QC_T_NONE;
}

static int cached_dev_ioctl(struct bcache_device *d, fmode_t mode,
			    unsigned int cmd, unsigned long arg)
{
	struct cached_dev *dc = container_of(d, struct cached_dev, disk);
	return __blkdev_driver_ioctl(dc->disk_sb.bdev, mode, cmd, arg);
}

static int cached_dev_congested(void *data, int bits)
{
	struct bcache_device *d = data;
	struct cached_dev *dc = container_of(d, struct cached_dev, disk);
	struct request_queue *q = bdev_get_queue(dc->disk_sb.bdev);
	int ret = 0;

	if (bdi_congested(&q->backing_dev_info, bits))
		return 1;

	if (cached_dev_get(dc)) {
		unsigned i;
		struct cache *ca;

		for_each_cache(ca, d->c, i) {
			q = bdev_get_queue(ca->disk_sb.bdev);
			ret |= bdi_congested(&q->backing_dev_info, bits);
		}

		cached_dev_put(dc);
	}

	return ret;
}

void bch_cached_dev_request_init(struct cached_dev *dc)
{
	struct gendisk *g = dc->disk.disk;

	g->queue->make_request_fn		= cached_dev_make_request;
	g->queue->backing_dev_info.congested_fn = cached_dev_congested;
	dc->disk.ioctl				= cached_dev_ioctl;
}

/* Blockdev volumes */

static void __blockdev_volume_make_request(struct request_queue *q,
					   struct bio *bio)
{
	struct search *s;
	struct bcache_device *d = bio->bi_bdev->bd_disk->private_data;
	int rw = bio_data_dir(bio);

	generic_start_io_acct(rw, bio_sectors(bio), &d->disk->part0);

	trace_bcache_request_start(d, bio);

	s = search_alloc(bio, d);

	if (!bio->bi_iter.bi_size) {
		if (s->orig_bio->bi_opf & (REQ_PREFLUSH|REQ_FUA))
			bch_journal_flush_async(&s->iop.c->journal,
						&s->cl);

		continue_at(&s->cl, search_free, NULL);
	} else if (rw) {
		struct disk_reservation res;
		unsigned flags = 0;

		bch_zero(res);

		if (bio_op(bio) != REQ_OP_DISCARD &&
		    bch_disk_reservation_get(d->c, &res, bio_sectors(bio), 0)) {
			s->iop.error = -ENOSPC;
			continue_at(&s->cl, search_free, NULL);
			return;
		}

		if (bio->bi_opf & (REQ_PREFLUSH|REQ_FUA))
			flags |= BCH_WRITE_FLUSH;
		if (bio_op(bio) == REQ_OP_DISCARD)
			flags |= BCH_WRITE_DISCARD;

		bch_write_op_init(&s->iop, d->c, &s->wbio, res,
				  foreground_write_point(d->c,
						(unsigned long) current),
				  POS(s->inode, bio->bi_iter.bi_sector),
				  NULL, flags);

		closure_call(&s->iop.cl, bch_write, NULL, &s->cl);
	} else {
		closure_get(&s->cl);
		bch_read(d->c, &s->rbio, bcache_dev_inum(d));
	}
	continue_at(&s->cl, search_free, NULL);
}

static blk_qc_t blockdev_volume_make_request(struct request_queue *q,
					     struct bio *bio)
{
	__blockdev_volume_make_request(q, bio);
	return BLK_QC_T_NONE;
}

static int blockdev_volume_ioctl(struct bcache_device *d, fmode_t mode,
				 unsigned int cmd, unsigned long arg)
{
	return -ENOTTY;
}

static int blockdev_volume_congested(void *data, int bits)
{
	struct bcache_device *d = data;
	struct request_queue *q;
	struct cache *ca;
	unsigned i;
	int ret = 0;

	for_each_cache(ca, d->c, i) {
		q = bdev_get_queue(ca->disk_sb.bdev);
		ret |= bdi_congested(&q->backing_dev_info, bits);
	}

	return ret;
}

void bch_blockdev_volume_request_init(struct bcache_device *d)
{
	struct gendisk *g = d->disk;

	g->queue->make_request_fn		= blockdev_volume_make_request;
	g->queue->backing_dev_info.congested_fn = blockdev_volume_congested;
	d->ioctl				= blockdev_volume_ioctl;
}
