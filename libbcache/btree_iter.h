#ifndef _BCACHE_BTREE_ITER_H
#define _BCACHE_BTREE_ITER_H

#include "btree_types.h"

struct btree_iter {
	/* Current btree depth */
	u8			level;

	/*
	 * Used in bch_btree_iter_traverse(), to indicate whether we're
	 * searching for @pos or the first key strictly greater than @pos
	 */
	u8			is_extents;

	/* Bitmasks for read/intent locks held per level */
	u8			nodes_locked;
	u8			nodes_intent_locked;

	/* Btree level below which we start taking intent locks */
	u8			locks_want;

	enum btree_id		btree_id:8;

	/*
	 * indicates we need to call bch_btree_iter_traverse() to revalidate
	 * iterator:
	 */
	u8			at_end_of_leaf;

	s8			error;

	struct cache_set	*c;

	/* Current position of the iterator */
	struct bpos		pos;

	u32			lock_seq[BTREE_MAX_DEPTH];

	/*
	 * NOTE: Never set iter->nodes to NULL except in btree_iter_lock_root().
	 *
	 * This is because iter->nodes[iter->level] == NULL is how
	 * btree_iter_next_node() knows that it's finished with a depth first
	 * traversal. Just unlocking a node (with btree_node_unlock()) is fine,
	 * and if you really don't want that node used again (e.g. btree_split()
	 * freed it) decrementing lock_seq will cause btree_node_relock() to
	 * always fail (but since freeing a btree node takes a write lock on the
	 * node, which increments the node's lock seq, that's not actually
	 * necessary in that example).
	 *
	 * One extra slot for a sentinel NULL:
	 */
	struct btree		*nodes[BTREE_MAX_DEPTH + 1];
	struct btree_node_iter	node_iters[BTREE_MAX_DEPTH];

	/*
	 * Current unpacked key - so that bch_btree_iter_next()/
	 * bch_btree_iter_next_with_holes() can correctly advance pos.
	 */
	struct bkey		k;

	/*
	 * Circular linked list of linked iterators: linked iterators share
	 * locks (e.g. two linked iterators may have the same node intent
	 * locked, or read and write locked, at the same time), and insertions
	 * through one iterator won't invalidate the other linked iterators.
	 */

	/* Must come last: */
	struct btree_iter	*next;
};

static inline bool btree_iter_linked(const struct btree_iter *iter)
{
	return iter->next != iter;
}

/**
 * for_each_linked_btree_iter - iterate over all iterators linked with @_iter
 */
#define for_each_linked_btree_iter(_iter, _linked)			\
	for ((_linked) = (_iter)->next;					\
	     (_linked) != (_iter);					\
	     (_linked) = (_linked)->next)

static inline struct btree_iter *
__next_linked_btree_node(struct btree_iter *iter, struct btree *b,
			 struct btree_iter *linked)
{
	do {
		linked = linked->next;

		if (linked == iter)
			return NULL;

		/*
		 * We don't compare the low bits of the lock sequence numbers
		 * because @iter might have taken a write lock on @b, and we
		 * don't want to skip the linked iterator if the sequence
		 * numbers were equal before taking that write lock. The lock
		 * sequence number is incremented by taking and releasing write
		 * locks and is even when unlocked:
		 */
	} while (linked->nodes[b->level] != b ||
		 linked->lock_seq[b->level] >> 1 != b->lock.state.seq >> 1);

	return linked;
}

/**
 * for_each_linked_btree_node - iterate over all iterators linked with @_iter
 * that also point to @_b
 *
 * @_b is assumed to be locked by @_iter
 *
 * Filters out iterators that don't have a valid btree_node iterator for @_b -
 * i.e. iterators for which btree_node_relock() would not succeed.
 */
#define for_each_linked_btree_node(_iter, _b, _linked)			\
	for ((_linked) = (_iter);					\
	     ((_linked) = __next_linked_btree_node(_iter, _b, _linked));)

#ifdef CONFIG_BCACHE_DEBUG
void bch_btree_iter_verify(struct btree_iter *, struct btree *);
#else
static inline void bch_btree_iter_verify(struct btree_iter *iter,
					 struct btree *b) {}
#endif

void bch_btree_node_iter_fix(struct btree_iter *, struct btree *,
			     struct btree_node_iter *, struct bset_tree *,
			     struct bkey_packed *, unsigned, unsigned);

int bch_btree_iter_unlock(struct btree_iter *);
bool __bch_btree_iter_set_locks_want(struct btree_iter *, unsigned);

static inline bool bch_btree_iter_set_locks_want(struct btree_iter *iter,
						 unsigned new_locks_want)
{
	new_locks_want = min(new_locks_want, BTREE_MAX_DEPTH);

	if (iter->locks_want == new_locks_want &&
	    iter->nodes_intent_locked == (1 << new_locks_want) - 1)
		return true;

	return __bch_btree_iter_set_locks_want(iter, new_locks_want);
}

bool bch_btree_iter_node_replace(struct btree_iter *, struct btree *);
void bch_btree_iter_node_drop_linked(struct btree_iter *, struct btree *);
void bch_btree_iter_node_drop(struct btree_iter *, struct btree *);

void bch_btree_iter_reinit_node(struct btree_iter *, struct btree *);

int __must_check bch_btree_iter_traverse(struct btree_iter *);

struct btree *bch_btree_iter_peek_node(struct btree_iter *);
struct btree *bch_btree_iter_next_node(struct btree_iter *, unsigned);

struct bkey_s_c bch_btree_iter_peek(struct btree_iter *);
struct bkey_s_c bch_btree_iter_peek_with_holes(struct btree_iter *);
void bch_btree_iter_set_pos_same_leaf(struct btree_iter *, struct bpos);
void bch_btree_iter_set_pos(struct btree_iter *, struct bpos);
void bch_btree_iter_advance_pos(struct btree_iter *);
void bch_btree_iter_rewind(struct btree_iter *, struct bpos);

void __bch_btree_iter_init(struct btree_iter *, struct cache_set *,
			   enum btree_id, struct bpos, unsigned , unsigned);

static inline void bch_btree_iter_init(struct btree_iter *iter,
				       struct cache_set *c,
				       enum btree_id btree_id,
				       struct bpos pos)
{
	__bch_btree_iter_init(iter, c, btree_id, pos, 0, 0);
}

static inline void bch_btree_iter_init_intent(struct btree_iter *iter,
					      struct cache_set *c,
					      enum btree_id btree_id,
					      struct bpos pos)
{
	__bch_btree_iter_init(iter, c, btree_id, pos, 1, 0);
}

void bch_btree_iter_link(struct btree_iter *, struct btree_iter *);
void bch_btree_iter_copy(struct btree_iter *, struct btree_iter *);

static inline struct bpos btree_type_successor(enum btree_id id,
					       struct bpos pos)
{
	if (id == BTREE_ID_INODES) {
		pos.inode++;
		pos.offset = 0;
	} else if (id != BTREE_ID_EXTENTS) {
		pos = bkey_successor(pos);
	}

	return pos;
}

static inline int __btree_iter_cmp(enum btree_id id,
				   struct bpos pos,
				   const struct btree_iter *r)
{
	if (id != r->btree_id)
		return id < r->btree_id ? -1 : 1;
	return bkey_cmp(pos, r->pos);
}

static inline int btree_iter_cmp(const struct btree_iter *l,
				 const struct btree_iter *r)
{
	return __btree_iter_cmp(l->btree_id, l->pos, r);
}

#define __for_each_btree_node(_iter, _c, _btree_id, _start, _depth,	\
			      _b, _locks_want)				\
	for (__bch_btree_iter_init((_iter), (_c), (_btree_id),		\
				   _start, _locks_want, _depth),	\
	     (_iter)->is_extents = false,				\
	     _b = bch_btree_iter_peek_node(_iter);			\
	     (_b);							\
	     (_b) = bch_btree_iter_next_node(_iter, _depth))

#define for_each_btree_node(_iter, _c, _btree_id, _start, _depth, _b)	\
	__for_each_btree_node(_iter, _c, _btree_id, _start, _depth, _b, 0)

#define __for_each_btree_key(_iter, _c, _btree_id,  _start,		\
			     _k, _locks_want)				\
	for (__bch_btree_iter_init((_iter), (_c), (_btree_id),		\
				   _start, _locks_want, 0);		\
	     !IS_ERR_OR_NULL(((_k) = bch_btree_iter_peek(_iter)).k);	\
	     bch_btree_iter_advance_pos(_iter))

#define for_each_btree_key(_iter, _c, _btree_id,  _start, _k)		\
	__for_each_btree_key(_iter, _c, _btree_id, _start, _k, 0)

#define for_each_btree_key_intent(_iter, _c, _btree_id,  _start, _k)	\
	__for_each_btree_key(_iter, _c, _btree_id, _start, _k, 1)

#define __for_each_btree_key_with_holes(_iter, _c, _btree_id,		\
					_start, _k, _locks_want)	\
	for (__bch_btree_iter_init((_iter), (_c), (_btree_id),		\
				   _start, _locks_want, 0);		\
	     !IS_ERR_OR_NULL(((_k) = bch_btree_iter_peek_with_holes(_iter)).k);\
	     bch_btree_iter_advance_pos(_iter))

#define for_each_btree_key_with_holes(_iter, _c, _btree_id, _start, _k)	\
	__for_each_btree_key_with_holes(_iter, _c, _btree_id, _start, _k, 0)

#define for_each_btree_key_with_holes_intent(_iter, _c, _btree_id,	\
					     _start, _k)		\
	__for_each_btree_key_with_holes(_iter, _c, _btree_id, _start, _k, 1)

static inline int btree_iter_err(struct bkey_s_c k)
{
	return IS_ERR(k.k) ? PTR_ERR(k.k) : 0;
}

/*
 * Unlocks before scheduling
 * Note: does not revalidate iterator
 */
static inline void bch_btree_iter_cond_resched(struct btree_iter *iter)
{
	struct btree_iter *linked;

	if (need_resched()) {
		for_each_linked_btree_iter(iter, linked)
			bch_btree_iter_unlock(linked);
		bch_btree_iter_unlock(iter);
		schedule();
	} else if (race_fault()) {
		for_each_linked_btree_iter(iter, linked)
			bch_btree_iter_unlock(linked);
		bch_btree_iter_unlock(iter);
	}
}

#endif /* _BCACHE_BTREE_ITER_H */
