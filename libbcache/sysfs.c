/*
 * bcache sysfs interfaces
 *
 * Copyright 2010, 2011 Kent Overstreet <kent.overstreet@gmail.com>
 * Copyright 2012 Google, Inc.
 */

#include "bcache.h"
#include "alloc.h"
#include "blockdev.h"
#include "compress.h"
#include "sysfs.h"
#include "btree_cache.h"
#include "btree_iter.h"
#include "btree_update.h"
#include "btree_gc.h"
#include "buckets.h"
#include "inode.h"
#include "journal.h"
#include "keylist.h"
#include "move.h"
#include "opts.h"
#include "request.h"
#include "super-io.h"
#include "tier.h"
#include "writeback.h"

#include <linux/blkdev.h>
#include <linux/sort.h>

write_attribute(attach);
write_attribute(detach);
write_attribute(unregister);
write_attribute(stop);
write_attribute(clear_stats);
write_attribute(trigger_btree_coalesce);
write_attribute(trigger_gc);
write_attribute(prune_cache);
write_attribute(blockdev_volume_create);
write_attribute(add_device);

read_attribute(uuid);
read_attribute(minor);
read_attribute(bucket_size);
read_attribute(bucket_size_bytes);
read_attribute(block_size);
read_attribute(block_size_bytes);
read_attribute(btree_node_size);
read_attribute(btree_node_size_bytes);
read_attribute(first_bucket);
read_attribute(nbuckets);
read_attribute(tree_depth);
read_attribute(root_usage_percent);
read_attribute(read_priority_stats);
read_attribute(write_priority_stats);
read_attribute(fragmentation_stats);
read_attribute(oldest_gen_stats);
read_attribute(reserve_stats);
read_attribute(btree_cache_size);
read_attribute(cache_available_percent);
read_attribute(compression_stats);
read_attribute(written);
read_attribute(btree_written);
read_attribute(metadata_written);
read_attribute(journal_debug);
write_attribute(journal_flush);
read_attribute(internal_uuid);

read_attribute(btree_gc_running);

read_attribute(btree_nodes);
read_attribute(btree_used_percent);
read_attribute(average_key_size);
read_attribute(available_buckets);
read_attribute(free_buckets);
read_attribute(dirty_data);
read_attribute(dirty_bytes);
read_attribute(dirty_buckets);
read_attribute(cached_data);
read_attribute(cached_bytes);
read_attribute(cached_buckets);
read_attribute(meta_buckets);
read_attribute(alloc_buckets);
read_attribute(has_data);
read_attribute(has_metadata);
read_attribute(bset_tree_stats);
read_attribute(alloc_debug);

read_attribute(state);
read_attribute(cache_read_races);
read_attribute(writeback_keys_done);
read_attribute(writeback_keys_failed);
read_attribute(io_errors);
rw_attribute(io_error_limit);
rw_attribute(io_error_halflife);
read_attribute(congested);
rw_attribute(congested_read_threshold_us);
rw_attribute(congested_write_threshold_us);

rw_attribute(sequential_cutoff);
rw_attribute(cache_mode);
rw_attribute(writeback_metadata);
rw_attribute(writeback_running);
rw_attribute(writeback_percent);
sysfs_pd_controller_attribute(writeback);

read_attribute(stripe_size);
read_attribute(partial_stripes_expensive);

rw_attribute(journal_write_delay_ms);
rw_attribute(journal_reclaim_delay_ms);
read_attribute(journal_entry_size_max);

rw_attribute(discard);
rw_attribute(running);
rw_attribute(label);
rw_attribute(readahead);
rw_attribute(verify);
rw_attribute(bypass_torture_test);
rw_attribute(cache_replacement_policy);

rw_attribute(foreground_write_ratelimit_enabled);
rw_attribute(copy_gc_enabled);
sysfs_pd_controller_attribute(copy_gc);

rw_attribute(tier);
rw_attribute(tiering_enabled);
rw_attribute(tiering_percent);
sysfs_pd_controller_attribute(tiering);

sysfs_pd_controller_attribute(foreground_write);

rw_attribute(pd_controllers_update_seconds);

rw_attribute(foreground_target_percent);

rw_attribute(size);
read_attribute(meta_replicas_have);
read_attribute(data_replicas_have);

#define BCH_DEBUG_PARAM(name, description)				\
	rw_attribute(name);

	BCH_DEBUG_PARAMS()
#undef BCH_DEBUG_PARAM

#define BCH_OPT(_name, _mode, ...)					\
	static struct attribute sysfs_opt_##_name = {			\
		.name = #_name,	.mode = _mode,				\
	};

	BCH_VISIBLE_OPTS()
#undef BCH_OPT

#define BCH_TIME_STAT(name, frequency_units, duration_units)		\
	sysfs_time_stats_attribute(name, frequency_units, duration_units);
	BCH_TIME_STATS()
#undef BCH_TIME_STAT

static struct attribute sysfs_state_rw = {
	.name = "state",
	.mode = S_IRUGO
};

SHOW(bch_cached_dev)
{
	struct cached_dev *dc = container_of(kobj, struct cached_dev,
					     disk.kobj);
	const char *states[] = { "no cache", "clean", "dirty", "inconsistent" };

#define var(stat)		(dc->stat)

	if (attr == &sysfs_cache_mode)
		return bch_snprint_string_list(buf, PAGE_SIZE,
					       bch_cache_modes + 1,
					       BDEV_CACHE_MODE(dc->disk_sb.sb));

	var_printf(verify,		"%i");
	var_printf(bypass_torture_test,	"%i");
	var_printf(writeback_metadata,	"%i");
	var_printf(writeback_running,	"%i");
	var_print(writeback_percent);
	sysfs_pd_controller_show(writeback, &dc->writeback_pd);

	sysfs_hprint(dirty_data,
		     bcache_dev_sectors_dirty(&dc->disk) << 9);
	sysfs_print(dirty_bytes,
		    bcache_dev_sectors_dirty(&dc->disk) << 9);

	sysfs_hprint(stripe_size,	dc->disk.stripe_size << 9);
	var_printf(partial_stripes_expensive,	"%u");

	var_hprint(sequential_cutoff);
	var_hprint(readahead);

	sysfs_print(running,		atomic_read(&dc->running));
	sysfs_print(state,		states[BDEV_STATE(dc->disk_sb.sb)]);

	if (attr == &sysfs_label) {
		memcpy(buf, dc->disk_sb.sb->label, BCH_SB_LABEL_SIZE);
		buf[BCH_SB_LABEL_SIZE + 1] = '\0';
		strcat(buf, "\n");
		return strlen(buf);
	}

#undef var
	return 0;
}

STORE(__cached_dev)
{
	struct cached_dev *dc = container_of(kobj, struct cached_dev,
					     disk.kobj);
	unsigned v = size;
	struct cache_set *c;
	struct kobj_uevent_env *env;

#define d_strtoul(var)		sysfs_strtoul(var, dc->var)
#define d_strtoul_nonzero(var)	sysfs_strtoul_clamp(var, dc->var, 1, INT_MAX)
#define d_strtoi_h(var)		sysfs_hatoi(var, dc->var)

	d_strtoul(verify);
	d_strtoul(bypass_torture_test);
	d_strtoul(writeback_metadata);
	d_strtoul(writeback_running);
	sysfs_strtoul_clamp(writeback_percent, dc->writeback_percent, 0, 40);
	sysfs_pd_controller_store(writeback, &dc->writeback_pd);

	d_strtoi_h(sequential_cutoff);
	d_strtoi_h(readahead);

	if (attr == &sysfs_clear_stats)
		bch_cache_accounting_clear(&dc->accounting);

	if (attr == &sysfs_running &&
	    strtoul_or_return(buf))
		bch_cached_dev_run(dc);

	if (attr == &sysfs_cache_mode) {
		ssize_t v = bch_read_string_list(buf, bch_cache_modes + 1);

		if (v < 0)
			return v;

		if ((unsigned) v != BDEV_CACHE_MODE(dc->disk_sb.sb)) {
			SET_BDEV_CACHE_MODE(dc->disk_sb.sb, v);
			bch_write_bdev_super(dc, NULL);
		}
	}

	if (attr == &sysfs_label) {
		u64 journal_seq = 0;
		int ret = 0;

		if (size > BCH_SB_LABEL_SIZE)
			return -EINVAL;

		mutex_lock(&dc->disk.inode_lock);

		memcpy(dc->disk_sb.sb->label, buf, size);
		if (size < BCH_SB_LABEL_SIZE)
			dc->disk_sb.sb->label[size] = '\0';
		if (size && dc->disk_sb.sb->label[size - 1] == '\n')
			dc->disk_sb.sb->label[size - 1] = '\0';

		memcpy(dc->disk.inode.v.i_label,
		       dc->disk_sb.sb->label, BCH_SB_LABEL_SIZE);

		bch_write_bdev_super(dc, NULL);

		if (dc->disk.c)
			ret = bch_btree_update(dc->disk.c, BTREE_ID_INODES,
					       &dc->disk.inode.k_i,
					       &journal_seq);

		mutex_unlock(&dc->disk.inode_lock);

		if (ret)
			return ret;

		if (dc->disk.c)
			ret = bch_journal_flush_seq(&dc->disk.c->journal,
						    journal_seq);
		if (ret)
			return ret;

		env = kzalloc(sizeof(struct kobj_uevent_env), GFP_KERNEL);
		if (!env)
			return -ENOMEM;
		add_uevent_var(env, "DRIVER=bcache");
		add_uevent_var(env, "CACHED_UUID=%pU", dc->disk_sb.sb->disk_uuid.b),
		add_uevent_var(env, "CACHED_LABEL=%s", buf);
		kobject_uevent_env(
			&disk_to_dev(dc->disk.disk)->kobj, KOBJ_CHANGE, env->envp);
		kfree(env);
	}

	if (attr == &sysfs_attach) {
		if (uuid_parse(buf, &dc->disk_sb.sb->user_uuid))
			return -EINVAL;

		list_for_each_entry(c, &bch_fs_list, list) {
			v = bch_cached_dev_attach(dc, c);
			if (!v)
				return size;
		}

		pr_err("Can't attach %s: cache set not found", buf);
		size = v;
	}

	if (attr == &sysfs_detach && dc->disk.c)
		bch_cached_dev_detach(dc);

	if (attr == &sysfs_stop)
		bch_blockdev_stop(&dc->disk);

	return size;
}

STORE(bch_cached_dev)
{
	struct cached_dev *dc = container_of(kobj, struct cached_dev,
					     disk.kobj);

	mutex_lock(&bch_register_lock);
	size = __cached_dev_store(kobj, attr, buf, size);

	if (attr == &sysfs_writeback_running)
		bch_writeback_queue(dc);

	if (attr == &sysfs_writeback_percent)
		schedule_delayed_work(&dc->writeback_pd_update,
				      dc->writeback_pd_update_seconds * HZ);

	mutex_unlock(&bch_register_lock);
	return size;
}

static struct attribute *bch_cached_dev_files[] = {
	&sysfs_attach,
	&sysfs_detach,
	&sysfs_stop,
	&sysfs_cache_mode,
	&sysfs_writeback_metadata,
	&sysfs_writeback_running,
	&sysfs_writeback_percent,
	sysfs_pd_controller_files(writeback),
	&sysfs_dirty_data,
	&sysfs_dirty_bytes,
	&sysfs_stripe_size,
	&sysfs_partial_stripes_expensive,
	&sysfs_sequential_cutoff,
	&sysfs_clear_stats,
	&sysfs_running,
	&sysfs_state,
	&sysfs_label,
	&sysfs_readahead,
#ifdef CONFIG_BCACHE_DEBUG
	&sysfs_verify,
	&sysfs_bypass_torture_test,
#endif
	NULL
};
KTYPE(bch_cached_dev);

SHOW(bch_blockdev_volume)
{
	struct bcache_device *d = container_of(kobj, struct bcache_device,
					       kobj);

	sysfs_hprint(size,	le64_to_cpu(d->inode.v.i_size));

	if (attr == &sysfs_label) {
		memcpy(buf, d->inode.v.i_label, BCH_SB_LABEL_SIZE);
		buf[BCH_SB_LABEL_SIZE + 1] = '\0';
		strcat(buf, "\n");
		return strlen(buf);
	}

	return 0;
}

STORE(__bch_blockdev_volume)
{
	struct bcache_device *d = container_of(kobj, struct bcache_device,
					       kobj);

	if (attr == &sysfs_size) {
		u64 journal_seq = 0;
		u64 v = strtoi_h_or_return(buf);
		int ret;

		mutex_lock(&d->inode_lock);

		if (v < le64_to_cpu(d->inode.v.i_size) ){
			ret = bch_inode_truncate(d->c, d->inode.k.p.inode,
						 v >> 9, NULL, NULL);
			if (ret) {
				mutex_unlock(&d->inode_lock);
				return ret;
			}
		}
		d->inode.v.i_size = cpu_to_le64(v);
		ret = bch_btree_update(d->c, BTREE_ID_INODES,
				       &d->inode.k_i, &journal_seq);

		mutex_unlock(&d->inode_lock);

		if (ret)
			return ret;

		ret = bch_journal_flush_seq(&d->c->journal, journal_seq);
		if (ret)
			return ret;

		set_capacity(d->disk, v >> 9);
	}

	if (attr == &sysfs_label) {
		u64 journal_seq = 0;
		int ret;

		mutex_lock(&d->inode_lock);

		memcpy(d->inode.v.i_label, buf, BCH_SB_LABEL_SIZE);
		ret = bch_btree_update(d->c, BTREE_ID_INODES,
				       &d->inode.k_i, &journal_seq);

		mutex_unlock(&d->inode_lock);

		return ret ?: bch_journal_flush_seq(&d->c->journal, journal_seq);
	}

	if (attr == &sysfs_unregister) {
		set_bit(BCACHE_DEV_DETACHING, &d->flags);
		bch_blockdev_stop(d);
	}

	return size;
}
STORE_LOCKED(bch_blockdev_volume)

static struct attribute *bch_blockdev_volume_files[] = {
	&sysfs_unregister,
	&sysfs_label,
	&sysfs_size,
	NULL
};
KTYPE(bch_blockdev_volume);

static int bch_bset_print_stats(struct cache_set *c, char *buf)
{
	struct bset_stats stats;
	size_t nodes = 0;
	struct btree *b;
	struct bucket_table *tbl;
	struct rhash_head *pos;
	unsigned iter;

	memset(&stats, 0, sizeof(stats));

	rcu_read_lock();
	for_each_cached_btree(b, c, tbl, iter, pos) {
		bch_btree_keys_stats(b, &stats);
		nodes++;
	}
	rcu_read_unlock();

	return snprintf(buf, PAGE_SIZE,
			"btree nodes:		%zu\n"
			"written sets:		%zu\n"
			"written key bytes:	%zu\n"
			"unwritten sets:		%zu\n"
			"unwritten key bytes:	%zu\n"
			"no table sets:		%zu\n"
			"no table key bytes:	%zu\n"
			"floats:			%zu\n"
			"failed unpacked:	%zu\n"
			"failed prev:		%zu\n"
			"failed overflow:	%zu\n",
			nodes,
			stats.sets[BSET_RO_AUX_TREE].nr,
			stats.sets[BSET_RO_AUX_TREE].bytes,
			stats.sets[BSET_RW_AUX_TREE].nr,
			stats.sets[BSET_RW_AUX_TREE].bytes,
			stats.sets[BSET_NO_AUX_TREE].nr,
			stats.sets[BSET_NO_AUX_TREE].bytes,
			stats.floats,
			stats.failed_unpacked,
			stats.failed_prev,
			stats.failed_overflow);
}

static unsigned bch_root_usage(struct cache_set *c)
{
	unsigned bytes = 0;
	struct bkey_packed *k;
	struct btree *b;
	struct btree_node_iter iter;

	goto lock_root;

	do {
		six_unlock_read(&b->lock);
lock_root:
		b = c->btree_roots[BTREE_ID_EXTENTS].b;
		six_lock_read(&b->lock);
	} while (b != c->btree_roots[BTREE_ID_EXTENTS].b);

	for_each_btree_node_key(b, k, &iter, btree_node_is_extents(b))
		bytes += bkey_bytes(k);

	six_unlock_read(&b->lock);

	return (bytes * 100) / btree_bytes(c);
}

static size_t bch_btree_cache_size(struct cache_set *c)
{
	size_t ret = 0;
	struct btree *b;

	mutex_lock(&c->btree_cache_lock);
	list_for_each_entry(b, &c->btree_cache, list)
		ret += btree_bytes(c);

	mutex_unlock(&c->btree_cache_lock);
	return ret;
}

static unsigned bch_fs_available_percent(struct cache_set *c)
{
	return div64_u64((u64) sectors_available(c) * 100,
			 c->capacity ?: 1);
}

#if 0
static unsigned bch_btree_used(struct cache_set *c)
{
	return div64_u64(c->gc_stats.key_bytes * 100,
			 (c->gc_stats.nodes ?: 1) * btree_bytes(c));
}

static unsigned bch_average_key_size(struct cache_set *c)
{
	return c->gc_stats.nkeys
		? div64_u64(c->gc_stats.data, c->gc_stats.nkeys)
		: 0;
}
#endif

static ssize_t show_fs_alloc_debug(struct cache_set *c, char *buf)
{
	struct bch_fs_usage stats = bch_fs_usage_read(c);

	return scnprintf(buf, PAGE_SIZE,
			 "capacity:\t\t%llu\n"
			 "compressed:\n"
			 "\tmeta:\t\t%llu\n"
			 "\tdirty:\t\t%llu\n"
			 "\tcached:\t\t%llu\n"
			 "uncompressed:\n"
			 "\tmeta:\t\t%llu\n"
			 "\tdirty:\t\t%llu\n"
			 "\tcached:\t\t%llu\n"
			 "persistent reserved sectors:\t%llu\n"
			 "online reserved sectors:\t%llu\n",
			 c->capacity,
			 stats.s[S_COMPRESSED][S_META],
			 stats.s[S_COMPRESSED][S_DIRTY],
			 stats.s[S_COMPRESSED][S_CACHED],
			 stats.s[S_UNCOMPRESSED][S_META],
			 stats.s[S_UNCOMPRESSED][S_DIRTY],
			 stats.s[S_UNCOMPRESSED][S_CACHED],
			 stats.persistent_reserved,
			 stats.online_reserved);
}

static ssize_t bch_compression_stats(struct cache_set *c, char *buf)
{
	struct btree_iter iter;
	struct bkey_s_c k;
	u64 nr_uncompressed_extents = 0, uncompressed_sectors = 0,
	    nr_compressed_extents = 0,
	    compressed_sectors_compressed = 0,
	    compressed_sectors_uncompressed = 0;

	for_each_btree_key(&iter, c, BTREE_ID_EXTENTS, POS_MIN, k)
		if (k.k->type == BCH_EXTENT) {
			struct bkey_s_c_extent e = bkey_s_c_to_extent(k);
			const struct bch_extent_ptr *ptr;
			const union bch_extent_crc *crc;

			extent_for_each_ptr_crc(e, ptr, crc) {
				if (crc_compression_type(crc) == BCH_COMPRESSION_NONE) {
					nr_uncompressed_extents++;
					uncompressed_sectors += e.k->size;
				} else {
					nr_compressed_extents++;
					compressed_sectors_compressed +=
						crc_compressed_size(e.k, crc);
					compressed_sectors_uncompressed +=
						crc_uncompressed_size(e.k, crc);
				}

				/* only looking at the first ptr */
				break;
			}
		}
	bch_btree_iter_unlock(&iter);

	return snprintf(buf, PAGE_SIZE,
			"uncompressed data:\n"
			"	nr extents:			%llu\n"
			"	size (bytes):			%llu\n"
			"compressed data:\n"
			"	nr extents:			%llu\n"
			"	compressed size (bytes):	%llu\n"
			"	uncompressed size (bytes):	%llu\n",
			nr_uncompressed_extents,
			uncompressed_sectors << 9,
			nr_compressed_extents,
			compressed_sectors_compressed << 9,
			compressed_sectors_uncompressed << 9);
}

SHOW(bch_fs)
{
	struct cache_set *c = container_of(kobj, struct cache_set, kobj);

	sysfs_print(minor,			c->minor);

	sysfs_print(journal_write_delay_ms,	c->journal.write_delay_ms);
	sysfs_print(journal_reclaim_delay_ms,	c->journal.reclaim_delay_ms);
	sysfs_hprint(journal_entry_size_max,	c->journal.entry_size_max);

	sysfs_hprint(block_size,		block_bytes(c));
	sysfs_print(block_size_bytes,		block_bytes(c));
	sysfs_hprint(btree_node_size,		c->sb.btree_node_size << 9);
	sysfs_print(btree_node_size_bytes,	c->sb.btree_node_size << 9);

	sysfs_hprint(btree_cache_size,		bch_btree_cache_size(c));
	sysfs_print(cache_available_percent,	bch_fs_available_percent(c));

	sysfs_print(btree_gc_running,		c->gc_pos.phase != GC_PHASE_DONE);

#if 0
	/* XXX: reimplement */
	sysfs_print(btree_used_percent,	bch_btree_used(c));
	sysfs_print(btree_nodes,	c->gc_stats.nodes);
	sysfs_hprint(average_key_size,	bch_average_key_size(c));
#endif

	sysfs_print(cache_read_races,
		    atomic_long_read(&c->cache_read_races));

	sysfs_print(writeback_keys_done,
		    atomic_long_read(&c->writeback_keys_done));
	sysfs_print(writeback_keys_failed,
		    atomic_long_read(&c->writeback_keys_failed));

	/* See count_io_errors for why 88 */
	sysfs_print(io_error_halflife,	c->error_decay * 88);
	sysfs_print(io_error_limit,	c->error_limit >> IO_ERROR_SHIFT);

	sysfs_hprint(congested,
		     ((uint64_t) bch_get_congested(c)) << 9);
	sysfs_print(congested_read_threshold_us,
		    c->congested_read_threshold_us);
	sysfs_print(congested_write_threshold_us,
		    c->congested_write_threshold_us);

	sysfs_printf(foreground_write_ratelimit_enabled, "%i",
		     c->foreground_write_ratelimit_enabled);
	sysfs_printf(copy_gc_enabled, "%i", c->copy_gc_enabled);
	sysfs_pd_controller_show(foreground_write, &c->foreground_write_pd);

	sysfs_print(pd_controllers_update_seconds,
		    c->pd_controllers_update_seconds);
	sysfs_print(foreground_target_percent, c->foreground_target_percent);

	sysfs_printf(tiering_enabled,		"%i", c->tiering_enabled);
	sysfs_print(tiering_percent,		c->tiering_percent);

	sysfs_pd_controller_show(tiering,	&c->tiers[1].pd); /* XXX */

	sysfs_printf(meta_replicas_have, "%u",	c->sb.meta_replicas_have);
	sysfs_printf(data_replicas_have, "%u",	c->sb.data_replicas_have);

	/* Debugging: */

	if (attr == &sysfs_journal_debug)
		return bch_journal_print_debug(&c->journal, buf);

#define BCH_DEBUG_PARAM(name, description) sysfs_print(name, c->name);
	BCH_DEBUG_PARAMS()
#undef BCH_DEBUG_PARAM

	if (!bch_fs_running(c))
		return -EPERM;

	if (attr == &sysfs_bset_tree_stats)
		return bch_bset_print_stats(c, buf);
	if (attr == &sysfs_alloc_debug)
		return show_fs_alloc_debug(c, buf);

	sysfs_print(tree_depth, c->btree_roots[BTREE_ID_EXTENTS].b->level);
	sysfs_print(root_usage_percent,		bch_root_usage(c));

	if (attr == &sysfs_compression_stats)
		return bch_compression_stats(c, buf);

	sysfs_printf(internal_uuid, "%pU", c->sb.uuid.b);

	return 0;
}

STORE(__bch_fs)
{
	struct cache_set *c = container_of(kobj, struct cache_set, kobj);

	if (attr == &sysfs_unregister) {
		bch_fs_detach(c);
		return size;
	}

	if (attr == &sysfs_stop) {
		bch_fs_stop_async(c);
		return size;
	}

	if (attr == &sysfs_clear_stats) {
		atomic_long_set(&c->writeback_keys_done,	0);
		atomic_long_set(&c->writeback_keys_failed,	0);
		bch_cache_accounting_clear(&c->accounting);

		return size;
	}

	sysfs_strtoul(congested_read_threshold_us,
		      c->congested_read_threshold_us);
	sysfs_strtoul(congested_write_threshold_us,
		      c->congested_write_threshold_us);

	if (attr == &sysfs_io_error_limit) {
		c->error_limit = strtoul_or_return(buf) << IO_ERROR_SHIFT;
		return size;
	}

	/* See count_io_errors() for why 88 */
	if (attr == &sysfs_io_error_halflife) {
		c->error_decay = strtoul_or_return(buf) / 88;
		return size;
	}

	sysfs_strtoul(journal_write_delay_ms, c->journal.write_delay_ms);
	sysfs_strtoul(journal_reclaim_delay_ms, c->journal.reclaim_delay_ms);

	sysfs_strtoul(foreground_write_ratelimit_enabled,
		      c->foreground_write_ratelimit_enabled);

	if (attr == &sysfs_copy_gc_enabled) {
		struct cache *ca;
		unsigned i;
		ssize_t ret = strtoul_safe(buf, c->copy_gc_enabled)
			?: (ssize_t) size;

		for_each_cache(ca, c, i)
			if (ca->moving_gc_read)
				wake_up_process(ca->moving_gc_read);
		return ret;
	}

	if (attr == &sysfs_tiering_enabled) {
		ssize_t ret = strtoul_safe(buf, c->tiering_enabled)
			?: (ssize_t) size;

		bch_tiering_start(c); /* issue wakeups */
		return ret;
	}

	sysfs_pd_controller_store(foreground_write, &c->foreground_write_pd);

	sysfs_strtoul(pd_controllers_update_seconds,
		      c->pd_controllers_update_seconds);
	sysfs_strtoul(foreground_target_percent, c->foreground_target_percent);

	sysfs_strtoul(tiering_percent,		c->tiering_percent);
	sysfs_pd_controller_store(tiering,	&c->tiers[1].pd); /* XXX */

	/* Debugging: */

#define BCH_DEBUG_PARAM(name, description) sysfs_strtoul(name, c->name);
	BCH_DEBUG_PARAMS()
#undef BCH_DEBUG_PARAM

	if (!bch_fs_running(c))
		return -EPERM;

	if (attr == &sysfs_journal_flush) {
		bch_journal_meta_async(&c->journal, NULL);

		return size;
	}

	if (attr == &sysfs_blockdev_volume_create) {
		u64 v = strtoi_h_or_return(buf);
		int r = bch_blockdev_volume_create(c, v);

		if (r)
			return r;
	}

	if (attr == &sysfs_trigger_btree_coalesce)
		bch_coalesce(c);

	/* Debugging: */

	if (attr == &sysfs_trigger_gc)
		bch_gc(c);

	if (attr == &sysfs_prune_cache) {
		struct shrink_control sc;

		sc.gfp_mask = GFP_KERNEL;
		sc.nr_to_scan = strtoul_or_return(buf);
		c->btree_cache_shrink.scan_objects(&c->btree_cache_shrink, &sc);
	}

	return size;
}

STORE(bch_fs)
{
	struct cache_set *c = container_of(kobj, struct cache_set, kobj);

	mutex_lock(&c->state_lock);
	size = __bch_fs_store(kobj, attr, buf, size);
	mutex_unlock(&c->state_lock);

	if (attr == &sysfs_add_device) {
		char *path = kstrdup(buf, GFP_KERNEL);
		int r = bch_dev_add(c, strim(path));

		kfree(path);
		if (r)
			return r;
	}

	return size;
}

static struct attribute *bch_fs_files[] = {
	&sysfs_unregister,
	&sysfs_stop,
	&sysfs_journal_write_delay_ms,
	&sysfs_journal_reclaim_delay_ms,
	&sysfs_journal_entry_size_max,
	&sysfs_blockdev_volume_create,
	&sysfs_add_device,

	&sysfs_block_size,
	&sysfs_block_size_bytes,
	&sysfs_btree_node_size,
	&sysfs_btree_node_size_bytes,
	&sysfs_tree_depth,
	&sysfs_root_usage_percent,
	&sysfs_btree_cache_size,
	&sysfs_cache_available_percent,
	&sysfs_compression_stats,

	&sysfs_average_key_size,

	&sysfs_io_error_limit,
	&sysfs_io_error_halflife,
	&sysfs_congested,
	&sysfs_congested_read_threshold_us,
	&sysfs_congested_write_threshold_us,
	&sysfs_clear_stats,

	&sysfs_meta_replicas_have,
	&sysfs_data_replicas_have,

	&sysfs_foreground_target_percent,
	&sysfs_tiering_percent,

	&sysfs_journal_flush,
	NULL
};
KTYPE(bch_fs);

/* internal dir - just a wrapper */

SHOW(bch_fs_internal)
{
	struct cache_set *c = container_of(kobj, struct cache_set, internal);
	return bch_fs_show(&c->kobj, attr, buf);
}

STORE(bch_fs_internal)
{
	struct cache_set *c = container_of(kobj, struct cache_set, internal);
	return bch_fs_store(&c->kobj, attr, buf, size);
}

static void bch_fs_internal_release(struct kobject *k)
{
}

static struct attribute *bch_fs_internal_files[] = {
	&sysfs_journal_debug,

	&sysfs_alloc_debug,

	&sysfs_btree_gc_running,

	&sysfs_btree_nodes,
	&sysfs_btree_used_percent,

	&sysfs_bset_tree_stats,
	&sysfs_cache_read_races,
	&sysfs_writeback_keys_done,
	&sysfs_writeback_keys_failed,

	&sysfs_trigger_btree_coalesce,
	&sysfs_trigger_gc,
	&sysfs_prune_cache,
	&sysfs_foreground_write_ratelimit_enabled,
	&sysfs_copy_gc_enabled,
	&sysfs_tiering_enabled,
	sysfs_pd_controller_files(tiering),
	sysfs_pd_controller_files(foreground_write),
	&sysfs_internal_uuid,

#define BCH_DEBUG_PARAM(name, description) &sysfs_##name,
	BCH_DEBUG_PARAMS()
#undef BCH_DEBUG_PARAM

	NULL
};
KTYPE(bch_fs_internal);

/* options */

SHOW(bch_fs_opts_dir)
{
	struct cache_set *c = container_of(kobj, struct cache_set, opts_dir);

	return bch_opt_show(&c->opts, attr->name, buf, PAGE_SIZE);
}

STORE(bch_fs_opts_dir)
{
	struct cache_set *c = container_of(kobj, struct cache_set, opts_dir);
	const struct bch_option *opt;
	enum bch_opt_id id;
	u64 v;

	id = bch_parse_sysfs_opt(attr->name, buf, &v);
	if (id < 0)
		return id;

	opt = &bch_opt_table[id];

	mutex_lock(&c->sb_lock);

	if (id == Opt_compression) {
		int ret = bch_check_set_has_compressed_data(c, v);
		if (ret) {
			mutex_unlock(&c->sb_lock);
			return ret;
		}
	}

	if (opt->set_sb != SET_NO_SB_OPT) {
		opt->set_sb(c->disk_sb, v);
		bch_write_super(c);
	}

	bch_opt_set(&c->opts, id, v);

	mutex_unlock(&c->sb_lock);

	return size;
}

static void bch_fs_opts_dir_release(struct kobject *k)
{
}

static struct attribute *bch_fs_opts_dir_files[] = {
#define BCH_OPT(_name, ...)						\
	&sysfs_opt_##_name,

	BCH_VISIBLE_OPTS()
#undef BCH_OPT

	NULL
};
KTYPE(bch_fs_opts_dir);

/* time stats */

SHOW(bch_fs_time_stats)
{
	struct cache_set *c = container_of(kobj, struct cache_set, time_stats);

#define BCH_TIME_STAT(name, frequency_units, duration_units)		\
	sysfs_print_time_stats(&c->name##_time, name,			\
			       frequency_units, duration_units);
	BCH_TIME_STATS()
#undef BCH_TIME_STAT

	return 0;
}

STORE(bch_fs_time_stats)
{
	struct cache_set *c = container_of(kobj, struct cache_set, time_stats);

#define BCH_TIME_STAT(name, frequency_units, duration_units)		\
	sysfs_clear_time_stats(&c->name##_time, name);
	BCH_TIME_STATS()
#undef BCH_TIME_STAT

	return size;
}

static void bch_fs_time_stats_release(struct kobject *k)
{
}

static struct attribute *bch_fs_time_stats_files[] = {
#define BCH_TIME_STAT(name, frequency_units, duration_units)		\
	sysfs_time_stats_attribute_list(name, frequency_units, duration_units)
	BCH_TIME_STATS()
#undef BCH_TIME_STAT

	NULL
};
KTYPE(bch_fs_time_stats);

typedef unsigned (bucket_map_fn)(struct cache *, struct bucket *, void *);

static unsigned bucket_priority_fn(struct cache *ca, struct bucket *g,
				   void *private)
{
	int rw = (private ? 1 : 0);

	return ca->set->prio_clock[rw].hand - g->prio[rw];
}

static unsigned bucket_sectors_used_fn(struct cache *ca, struct bucket *g,
				       void *private)
{
	return bucket_sectors_used(g);
}

static unsigned bucket_oldest_gen_fn(struct cache *ca, struct bucket *g,
				     void *private)
{
	return bucket_gc_gen(ca, g);
}

static ssize_t show_quantiles(struct cache *ca, char *buf,
			      bucket_map_fn *fn, void *private)
{
	int cmp(const void *l, const void *r)
	{	return *((unsigned *) r) - *((unsigned *) l); }

	size_t n = ca->mi.nbuckets, i;
	/* Compute 31 quantiles */
	unsigned q[31], *p;
	ssize_t ret = 0;

	p = vzalloc(ca->mi.nbuckets * sizeof(unsigned));
	if (!p)
		return -ENOMEM;

	for (i = ca->mi.first_bucket; i < n; i++)
		p[i] = fn(ca, &ca->buckets[i], private);

	sort(p, n, sizeof(unsigned), cmp, NULL);

	while (n &&
	       !p[n - 1])
		--n;

	for (i = 0; i < ARRAY_SIZE(q); i++)
		q[i] = p[n * (i + 1) / (ARRAY_SIZE(q) + 1)];

	vfree(p);

	for (i = 0; i < ARRAY_SIZE(q); i++)
		ret += scnprintf(buf + ret, PAGE_SIZE - ret,
				 "%u ", q[i]);
	buf[ret - 1] = '\n';

	return ret;

}

static ssize_t show_reserve_stats(struct cache *ca, char *buf)
{
	enum alloc_reserve i;
	ssize_t ret;

	spin_lock(&ca->freelist_lock);

	ret = scnprintf(buf, PAGE_SIZE,
			"free_inc:\t%zu\t%zu\n",
			fifo_used(&ca->free_inc),
			ca->free_inc.size);

	for (i = 0; i < RESERVE_NR; i++)
		ret += scnprintf(buf + ret, PAGE_SIZE - ret,
				 "free[%u]:\t%zu\t%zu\n", i,
				 fifo_used(&ca->free[i]),
				 ca->free[i].size);

	spin_unlock(&ca->freelist_lock);

	return ret;
}

static ssize_t show_dev_alloc_debug(struct cache *ca, char *buf)
{
	struct cache_set *c = ca->set;
	struct bch_dev_usage stats = bch_dev_usage_read(ca);

	return scnprintf(buf, PAGE_SIZE,
		"free_inc:               %zu/%zu\n"
		"free[RESERVE_PRIO]:     %zu/%zu\n"
		"free[RESERVE_BTREE]:    %zu/%zu\n"
		"free[RESERVE_MOVINGGC]: %zu/%zu\n"
		"free[RESERVE_NONE]:     %zu/%zu\n"
		"alloc:                  %llu/%llu\n"
		"meta:                   %llu/%llu\n"
		"dirty:                  %llu/%llu\n"
		"available:              %llu/%llu\n"
		"freelist_wait:          %s\n"
		"open buckets:           %u/%u (reserved %u)\n"
		"open_buckets_wait:      %s\n",
		fifo_used(&ca->free_inc),		ca->free_inc.size,
		fifo_used(&ca->free[RESERVE_PRIO]),	ca->free[RESERVE_PRIO].size,
		fifo_used(&ca->free[RESERVE_BTREE]),	ca->free[RESERVE_BTREE].size,
		fifo_used(&ca->free[RESERVE_MOVINGGC]),	ca->free[RESERVE_MOVINGGC].size,
		fifo_used(&ca->free[RESERVE_NONE]),	ca->free[RESERVE_NONE].size,
		stats.buckets_alloc,			ca->mi.nbuckets - ca->mi.first_bucket,
		stats.buckets_meta,			ca->mi.nbuckets - ca->mi.first_bucket,
		stats.buckets_dirty,			ca->mi.nbuckets - ca->mi.first_bucket,
		__buckets_available_cache(ca, stats),	ca->mi.nbuckets - ca->mi.first_bucket,
		c->freelist_wait.list.first		? "waiting" : "empty",
		c->open_buckets_nr_free, OPEN_BUCKETS_COUNT, BTREE_NODE_RESERVE,
		c->open_buckets_wait.list.first		? "waiting" : "empty");
}

static u64 sectors_written(struct cache *ca)
{
	u64 ret = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		ret += *per_cpu_ptr(ca->sectors_written, cpu);

	return ret;
}

SHOW(bch_dev)
{
	struct cache *ca = container_of(kobj, struct cache, kobj);
	struct cache_set *c = ca->set;
	struct bch_dev_usage stats = bch_dev_usage_read(ca);

	sysfs_printf(uuid,		"%pU\n", ca->uuid.b);

	sysfs_hprint(bucket_size,	bucket_bytes(ca));
	sysfs_print(bucket_size_bytes,	bucket_bytes(ca));
	sysfs_hprint(block_size,	block_bytes(c));
	sysfs_print(block_size_bytes,	block_bytes(c));
	sysfs_print(first_bucket,	ca->mi.first_bucket);
	sysfs_print(nbuckets,		ca->mi.nbuckets);
	sysfs_print(discard,		ca->mi.discard);
	sysfs_hprint(written, sectors_written(ca) << 9);
	sysfs_hprint(btree_written,
		     atomic64_read(&ca->btree_sectors_written) << 9);
	sysfs_hprint(metadata_written,
		     (atomic64_read(&ca->meta_sectors_written) +
		      atomic64_read(&ca->btree_sectors_written)) << 9);

	sysfs_print(io_errors,
		    atomic_read(&ca->io_errors) >> IO_ERROR_SHIFT);

	sysfs_hprint(dirty_data,	stats.sectors_dirty << 9);
	sysfs_print(dirty_bytes,	stats.sectors_dirty << 9);
	sysfs_print(dirty_buckets,	stats.buckets_dirty);
	sysfs_hprint(cached_data,	stats.sectors_cached << 9);
	sysfs_print(cached_bytes,	stats.sectors_cached << 9);
	sysfs_print(cached_buckets,	stats.buckets_cached);
	sysfs_print(meta_buckets,	stats.buckets_meta);
	sysfs_print(alloc_buckets,	stats.buckets_alloc);
	sysfs_print(available_buckets,	buckets_available_cache(ca));
	sysfs_print(free_buckets,	buckets_free_cache(ca));
	sysfs_print(has_data,		ca->mi.has_data);
	sysfs_print(has_metadata,	ca->mi.has_metadata);

	sysfs_pd_controller_show(copy_gc, &ca->moving_gc_pd);

	if (attr == &sysfs_cache_replacement_policy)
		return bch_snprint_string_list(buf, PAGE_SIZE,
					       bch_cache_replacement_policies,
					       ca->mi.replacement);

	sysfs_print(tier,		ca->mi.tier);

	if (attr == &sysfs_state_rw)
		return bch_snprint_string_list(buf, PAGE_SIZE,
					       bch_dev_state,
					       ca->mi.state);

	if (attr == &sysfs_read_priority_stats)
		return show_quantiles(ca, buf, bucket_priority_fn, (void *) 0);
	if (attr == &sysfs_write_priority_stats)
		return show_quantiles(ca, buf, bucket_priority_fn, (void *) 1);
	if (attr == &sysfs_fragmentation_stats)
		return show_quantiles(ca, buf, bucket_sectors_used_fn, NULL);
	if (attr == &sysfs_oldest_gen_stats)
		return show_quantiles(ca, buf, bucket_oldest_gen_fn, NULL);
	if (attr == &sysfs_reserve_stats)
		return show_reserve_stats(ca, buf);
	if (attr == &sysfs_alloc_debug)
		return show_dev_alloc_debug(ca, buf);

	return 0;
}

STORE(__bch_dev)
{
	struct cache *ca = container_of(kobj, struct cache, kobj);
	struct cache_set *c = ca->set;
	struct bch_member *mi;

	sysfs_pd_controller_store(copy_gc, &ca->moving_gc_pd);

	if (attr == &sysfs_discard) {
		bool v = strtoul_or_return(buf);

		mutex_lock(&c->sb_lock);
		mi = &bch_sb_get_members(c->disk_sb)->members[ca->dev_idx];

		if (v != BCH_MEMBER_DISCARD(mi)) {
			SET_BCH_MEMBER_DISCARD(mi, v);
			bch_write_super(c);
		}
		mutex_unlock(&c->sb_lock);
	}

	if (attr == &sysfs_cache_replacement_policy) {
		ssize_t v = bch_read_string_list(buf, bch_cache_replacement_policies);

		if (v < 0)
			return v;

		mutex_lock(&c->sb_lock);
		mi = &bch_sb_get_members(c->disk_sb)->members[ca->dev_idx];

		if ((unsigned) v != BCH_MEMBER_REPLACEMENT(mi)) {
			SET_BCH_MEMBER_REPLACEMENT(mi, v);
			bch_write_super(c);
		}
		mutex_unlock(&c->sb_lock);
	}

	if (attr == &sysfs_tier) {
		unsigned prev_tier;
		unsigned v = strtoul_restrict_or_return(buf,
					0, BCH_TIER_MAX - 1);

		mutex_lock(&c->sb_lock);
		prev_tier = ca->mi.tier;

		if (v == ca->mi.tier) {
			mutex_unlock(&c->sb_lock);
			return size;
		}

		mi = &bch_sb_get_members(c->disk_sb)->members[ca->dev_idx];
		SET_BCH_MEMBER_TIER(mi, v);
		bch_write_super(c);

		bch_dev_group_remove(&c->tiers[prev_tier].devs, ca);
		bch_dev_group_add(&c->tiers[ca->mi.tier].devs, ca);
		mutex_unlock(&c->sb_lock);

		bch_recalc_capacity(c);
		bch_tiering_start(c);
	}

	if (attr == &sysfs_clear_stats) {
		int cpu;

		for_each_possible_cpu(cpu)
			*per_cpu_ptr(ca->sectors_written, cpu) = 0;

		atomic64_set(&ca->btree_sectors_written, 0);
		atomic64_set(&ca->meta_sectors_written, 0);
		atomic_set(&ca->io_count, 0);
		atomic_set(&ca->io_errors, 0);
	}

	return size;
}
STORE_LOCKED(bch_dev)

static struct attribute *bch_dev_files[] = {
	&sysfs_uuid,
	&sysfs_bucket_size,
	&sysfs_bucket_size_bytes,
	&sysfs_block_size,
	&sysfs_block_size_bytes,
	&sysfs_first_bucket,
	&sysfs_nbuckets,
	&sysfs_read_priority_stats,
	&sysfs_write_priority_stats,
	&sysfs_fragmentation_stats,
	&sysfs_oldest_gen_stats,
	&sysfs_reserve_stats,
	&sysfs_available_buckets,
	&sysfs_free_buckets,
	&sysfs_dirty_data,
	&sysfs_dirty_bytes,
	&sysfs_dirty_buckets,
	&sysfs_cached_data,
	&sysfs_cached_bytes,
	&sysfs_cached_buckets,
	&sysfs_meta_buckets,
	&sysfs_alloc_buckets,
	&sysfs_has_data,
	&sysfs_has_metadata,
	&sysfs_discard,
	&sysfs_written,
	&sysfs_btree_written,
	&sysfs_metadata_written,
	&sysfs_io_errors,
	&sysfs_clear_stats,
	&sysfs_cache_replacement_policy,
	&sysfs_tier,
	&sysfs_state_rw,
	&sysfs_alloc_debug,

	sysfs_pd_controller_files(copy_gc),
	NULL
};
KTYPE(bch_dev);
