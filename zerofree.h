/*
 * zerofree - helpers
 *
 * Copyright (C) 2004-2012 R M Yorston
 *
 * This file may be redistributed under the terms of the GNU General Public
 * License, version 2.
 */
#ifndef ZEROFREE_H
#define ZEROFREE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Return true if all n bytes of p equal fill (n == 0 is vacuously true).
 *
 * Single data stream: one O(1) first-byte bail, then 64-bit words XORed
 * against the splatted fill byte and OR-accumulated per 512-byte
 * sub-chunk with early exit.  At -march=x86-64-v3 the inner loop
 * auto-vectorizes to AVX2 vpxor/vpor (32 bytes/iteration); words are
 * loaded via memcpy to stay strict-aliasing safe.
 */
static inline bool zf_block_is_filled(const unsigned char *restrict p,
		size_t n, uint8_t fill)
{
	if (n == 0)
		return true;
	if (p[0] != fill)
		return false;

	const uint64_t fill8 = 0x0101010101010101ULL * fill;
	size_t off = 0;

	while (off < n && (off & 7)) {		/* align to a word (rare) */
		if (p[off] != fill)
			return false;
		off++;
	}

	for (; off + 512 <= n; off += 512) {
		uint64_t acc = 0;
		for (size_t i = 0; i < 512; i += 8) {
			uint64_t w;
			memcpy(&w, p + off + i, 8);
			acc |= w ^ fill8;
		}
		if (acc)
			return false;
	}

	while (off < n) {				/* tail (< 512 bytes) */
		if (p[off] != fill)
			return false;
		off++;
	}
	return true;
}

#endif /* ZEROFREE_H */
