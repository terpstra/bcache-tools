#ifndef _BCACHE_COMPRESS_H
#define _BCACHE_COMPRESS_H

int bch_bio_uncompress_inplace(struct cache_set *, struct bio *,
			       unsigned, struct bch_extent_crc128);
int bch_bio_uncompress(struct cache_set *, struct bio *, struct bio *,
		       struct bvec_iter, struct bch_extent_crc128);
void bch_bio_compress(struct cache_set *, struct bio *, size_t *,
		      struct bio *, size_t *, unsigned *);

int bch_check_set_has_compressed_data(struct cache_set *, unsigned);
void bch_fs_compress_exit(struct cache_set *);
int bch_fs_compress_init(struct cache_set *);

#endif /* _BCACHE_COMPRESS_H */
