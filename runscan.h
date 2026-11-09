/*
 * runscan.h - word-at-a-time iterator over an ext2-style block bitmap.
 *
 * Yields maximal runs of FREE blocks (bit clear) fully inside [lo, hi).
 * Dependency-free (stdint.h/string.h only); all functions are static
 * inline so the header can be used from any TU.
 *
 * Bit order is the ext2 one, LSB-first per byte: block b is USED iff
 * map[b >> 3] & (1u << (b & 7)), matching ext2fs_test_block_bitmap2().
 */
#ifndef ZF_RUNSCAN_H
#define ZF_RUNSCAN_H

#include <stdint.h>
#include <string.h>

struct run_it {
	const unsigned char *map;	/* block bitmap bytes (ext2 bit order) */
	uint64_t lo, hi;		/* scan range [lo, hi) in blocks */
	uint64_t pos;			/* private: next block to examine */
};

static inline void run_it_init(struct run_it *it, const unsigned char *map,
			       uint64_t lo, uint64_t hi)
{
	it->map = map;
	it->lo = lo;
	it->hi = hi;
	it->pos = lo;
}

/*
 * Next maximal run of FREE blocks at/after it->pos, fully inside
 * [lo, hi). Returns 0 and sets *start and *len, or 1 when done.
 *
 * Per word w covering blocks base = w*64 .. base+63: load the word with
 * memcpy (the map need not be aligned), keep only the valid window
 * [it->pos, min(base+64, hi)) -- hi - base == 64 means "all ones", so
 * the shift below is always in [0, 63] -- and take free bits as
 * ~bits & mask. The first free bit starts the run (ctzll); the run
 * ends at the first used bit found by stepping word by word from
 * there, or at hi.
 *
 * Bounded loads: the iterator never reads past byte ceil(hi/8) of map.
 * For the final partial word (hi < base + 64) only
 * ceil((hi - base)/8) bytes are memcpy'd, into a zero-initialized
 * word; every use of a loaded word masks off bits at/above hi before
 * interpreting them, so the zero-filled high bytes are never observed.
 */
static inline int next_run(struct run_it *it, uint64_t *start, uint64_t *len)
{
	uint64_t hi = it->hi, pos;
	while (1) {
		uint64_t w = it->pos >> 6, base = w << 6;
		uint64_t lo_b = it->pos > base ? it->pos : base;
		uint64_t hi_b = hi < base + 64 ? hi : base + 64;
		uint64_t bits = 0, free_bits, mask;
		size_t nb;
		if (lo_b >= hi_b)
			return 1;
		nb = hi < base + 64 ? (size_t)((hi - base + 7) / 8) : 8;
		memcpy(&bits, it->map + (w << 3), nb);
		mask = (hi_b - base == 64 ? ~0ULL
				       : (1ULL << (hi_b - base)) - 1) &
			 (~0ULL << (lo_b - base));
		free_bits = ~bits & mask;
		if (!free_bits) {
			it->pos = hi_b;
			continue;
		}
		pos = base + __builtin_ctzll(free_bits);	/* first free */
		/* now find the first USED bit at/after pos, word by word */
		w = pos >> 6;
		while (1) {
			uint64_t b2 = 0, m, shift = (w << 6) < pos ?
						 pos - (w << 6) : 0;
			size_t nb = hi < (w << 6) + 64 ?
				     (size_t)((hi - (w << 6) + 7) / 8) : 8;
			memcpy(&b2, it->map + (w << 3), nb);
			m = b2 & (~0ULL << shift);
			if (hi < (w << 6) + 64)
				m &= (1ULL << (hi - (w << 6))) - 1;
			if (m) {
				*start = pos;
				*len = (w << 6) + __builtin_ctzll(m) - pos;
				it->pos = pos + *len;
				return 0;
			}
			if (hi <= (w << 6) + 64) {	/* free run extends to hi */
				*start = pos;
				*len = hi - pos;
				it->pos = hi;
				return 0;
			}
			w++;
		}
	}
}

#endif /* ZF_RUNSCAN_H */
