#ifndef _BCACHE_BLOCKDEV_H
#define _BCACHE_BLOCKDEV_H

#include "blockdev_types.h"
#include "io_types.h"

struct search {
	/* Stack frame for bio_complete */
	struct closure		cl;

	union {
	struct bch_read_bio	rbio;
	struct bch_write_bio	wbio;
	};
	/* Not modified */
	struct bio		*orig_bio;
	struct bcache_device	*d;

	unsigned		inode;
	unsigned		write:1;

	/* Flags only used for reads */
	unsigned		recoverable:1;
	unsigned		read_dirty_data:1;
	unsigned		cache_miss:1;

	/*
	 * For reads:  bypass read from cache and insertion into cache
	 * For writes: discard key range from cache, sending the write to
	 *             the backing device (if there is a backing device)
	 */
	unsigned		bypass:1;

	unsigned long		start_time;

	/*
	 * Mostly only used for writes. For reads, we still make use of
	 * some trivial fields:
	 * - c
	 * - error
	 */
	struct bch_write_op	iop;
};

#ifndef NO_BCACHE_BLOCKDEV

extern struct kobj_type bch_cached_dev_ktype;
extern struct kobj_type bch_blockdev_volume_ktype;

void bch_write_bdev_super(struct cached_dev *, struct closure *);

void bch_cached_dev_release(struct kobject *);
void bch_blockdev_volume_release(struct kobject *);

int bch_cached_dev_attach(struct cached_dev *, struct cache_set *);
void bch_attach_backing_devs(struct cache_set *);

void bch_cached_dev_detach(struct cached_dev *);
void bch_cached_dev_run(struct cached_dev *);
void bch_blockdev_stop(struct bcache_device *);

bool bch_is_open_backing_dev(struct block_device *);
const char *bch_backing_dev_register(struct bcache_superblock *);

int bch_blockdev_volume_create(struct cache_set *, u64);
int bch_blockdev_volumes_start(struct cache_set *);

void bch_blockdevs_stop(struct cache_set *);

void bch_fs_blockdev_exit(struct cache_set *);
int bch_fs_blockdev_init(struct cache_set *);
void bch_blockdev_exit(void);
int bch_blockdev_init(void);

#else

static inline void bch_write_bdev_super(struct cached_dev *dc,
					struct closure *cl) {}

static inline void bch_cached_dev_release(struct kobject *kobj) {}
static inline void bch_blockdev_volume_release(struct kobject *kobj) {}

static inline int bch_cached_dev_attach(struct cached_dev *dc, struct cache_set *c)
{
	return 0;
}
static inline void bch_attach_backing_devs(struct cache_set *c) {}

static inline void bch_cached_dev_detach(struct cached_dev *dc) {}
static inline void bch_cached_dev_run(struct cached_dev *dc) {}
static inline void bch_blockdev_stop(struct bcache_device *d) {}

static inline bool bch_is_open_backing_dev(struct block_device *bdev)
{
	return false;
}
static inline const char *bch_backing_dev_register(struct bcache_superblock *sb)
{
	return "not implemented";
}

static inline int bch_blockdev_volume_create(struct cache_set *c, u64 s) { return 0; }
static inline int bch_blockdev_volumes_start(struct cache_set *c) { return 0; }

static inline void bch_blockdevs_stop(struct cache_set *c) {}
static inline void bch_fs_blockdev_exit(struct cache_set *c) {}
static inline int bch_fs_blockdev_init(struct cache_set *c) { return 0; }
static inline void bch_blockdev_exit(void) {}
static inline int bch_blockdev_init(void) { return 0; }

#endif

static inline void cached_dev_put(struct cached_dev *dc)
{
	if (atomic_dec_and_test(&dc->count))
		schedule_work(&dc->detach);
}

static inline bool cached_dev_get(struct cached_dev *dc)
{
	if (!atomic_inc_not_zero(&dc->count))
		return false;

	/* Paired with the mb in cached_dev_attach */
	smp_mb__after_atomic();
	return true;
}

static inline u64 bcache_dev_inum(struct bcache_device *d)
{
	return d->inode.k.p.inode;
}

static inline struct bcache_device *bch_dev_find(struct cache_set *c, u64 inode)
{
	return radix_tree_lookup(&c->devices, inode);
}

#endif /* _BCACHE_BLOCKDEV_H */
