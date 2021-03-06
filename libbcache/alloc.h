#ifndef _BCACHE_ALLOC_H
#define _BCACHE_ALLOC_H

#include "alloc_types.h"

struct bkey;
struct bucket;
struct cache;
struct cache_set;
struct cache_group;

static inline size_t prios_per_bucket(const struct cache *ca)
{
	return (bucket_bytes(ca) - sizeof(struct prio_set)) /
		sizeof(struct bucket_disk);
}

static inline size_t prio_buckets(const struct cache *ca)
{
	return DIV_ROUND_UP((size_t) (ca)->mi.nbuckets, prios_per_bucket(ca));
}

void bch_dev_group_remove(struct cache_group *, struct cache *);
void bch_dev_group_add(struct cache_group *, struct cache *);

int bch_prio_read(struct cache *);

void bch_recalc_min_prio(struct cache *, int);

size_t bch_bucket_alloc(struct cache *, enum alloc_reserve);

void bch_open_bucket_put(struct cache_set *, struct open_bucket *);

struct open_bucket *bch_alloc_sectors_start(struct cache_set *,
					    struct write_point *,
					    unsigned, unsigned,
					    enum alloc_reserve,
					    struct closure *);

void bch_alloc_sectors_append_ptrs(struct cache_set *, struct bkey_i_extent *,
				   unsigned, struct open_bucket *, unsigned);
void bch_alloc_sectors_done(struct cache_set *, struct write_point *,
			    struct open_bucket *);

struct open_bucket *bch_alloc_sectors(struct cache_set *, struct write_point *,
				      struct bkey_i_extent *, unsigned, unsigned,
				      enum alloc_reserve, struct closure *);

static inline void bch_wake_allocator(struct cache *ca)
{
	struct task_struct *p;

	rcu_read_lock();
	if ((p = ACCESS_ONCE(ca->alloc_thread)))
		wake_up_process(p);
	rcu_read_unlock();
}

static inline struct cache *cache_group_next_rcu(struct cache_group *devs,
						 unsigned *iter)
{
	struct cache *ret = NULL;

	while (*iter < devs->nr &&
	       !(ret = rcu_dereference(devs->d[*iter].dev)))
		(*iter)++;

	return ret;
}

#define group_for_each_cache_rcu(ca, devs, iter)			\
	for ((iter) = 0;						\
	     ((ca) = cache_group_next_rcu((devs), &(iter)));		\
	     (iter)++)

static inline struct cache *cache_group_next(struct cache_group *devs,
					     unsigned *iter)
{
	struct cache *ret;

	rcu_read_lock();
	if ((ret = cache_group_next_rcu(devs, iter)))
		percpu_ref_get(&ret->ref);
	rcu_read_unlock();

	return ret;
}

#define group_for_each_cache(ca, devs, iter)				\
	for ((iter) = 0;						\
	     (ca = cache_group_next(devs, &(iter)));			\
	     percpu_ref_put(&ca->ref), (iter)++)

#define __open_bucket_next_online_device(_c, _ob, _ptr, _ca)            \
({									\
	(_ca) = NULL;							\
									\
	while ((_ptr) < (_ob)->ptrs + (_ob)->nr_ptrs &&			\
	       !((_ca) = PTR_CACHE(_c, _ptr)))				\
		(_ptr)++;						\
	(_ca);								\
})

#define open_bucket_for_each_online_device(_c, _ob, _ptr, _ca)		\
	for ((_ptr) = (_ob)->ptrs;					\
	     ((_ca) = __open_bucket_next_online_device(_c, _ob,	_ptr, _ca));\
	     (_ptr)++)

void bch_recalc_capacity(struct cache_set *);
void bch_dev_allocator_stop(struct cache *);
int bch_dev_allocator_start(struct cache *);
void bch_fs_allocator_init(struct cache_set *);

#endif /* _BCACHE_ALLOC_H */
