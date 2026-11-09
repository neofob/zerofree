/*
 * Unit tests for zf_block_is_filled() from zerofree.h.
 *
 * Build: gcc $(ZF_CFLAGS) -I. -o tests/unit_filled tests/unit_filled.c
 * No external dependencies; returns non-zero if any check fails.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zerofree.h"

static int failures = 0;
static int checks = 0;

#define CHECK(name, cond) do {						\
	checks++;							\
	if (cond) {							\
		printf("PASS: %s\n", name);				\
	} else {							\
		printf("FAIL: %s (line %d)\n", name, __LINE__);	\
		failures++;						\
	}								\
} while (0)

/* Deterministic LCG so the fuzz case is reproducible. */
static uint32_t lcg_state = 0x12345678u;
static uint32_t lcg(void)
{
	lcg_state = lcg_state * 1664525u + 1013904223u;
	return lcg_state;
}

int main(void)
{
	unsigned char *buf = malloc(8192);
	if (!buf)
		return 1;

#define FILL(buf, n, v) memset(buf, (v), (n))

	/* basic all-filled cases */
	FILL(buf, 1024, 0x00);
	CHECK("n=1024 all-0x00 fill=0x00", zf_block_is_filled(buf, 1024, 0x00));
	FILL(buf, 1024, 0xFF);
	CHECK("n=1024 all-0xFF fill=0xFF", zf_block_is_filled(buf, 1024, 0xFF));

	/* single-byte dirt at each position class */
	FILL(buf, 1024, 0x00);
	buf[0] = 0x11;
	CHECK("first byte dirty", !zf_block_is_filled(buf, 1024, 0x00));
	FILL(buf, 1024, 0x00);
	buf[512] = 0x22;
	CHECK("middle byte dirty", !zf_block_is_filled(buf, 1024, 0x00));
	FILL(buf, 1024, 0x00);
	buf[1023] = 0x33;
	CHECK("last byte dirty", !zf_block_is_filled(buf, 1024, 0x00));

	/* single dirt well inside a larger block */
	FILL(buf, 4096, 0x00);
	buf[37] = 0x44;
	CHECK("single dirt at off=37 n=4096", !zf_block_is_filled(buf, 4096, 0x00));

	/* n = 1 and n = 0 */
	const unsigned char one = 0x7E;
	CHECK("n=1 equal", zf_block_is_filled(&one, 1, 0x7E));
	CHECK("n=1 different", !zf_block_is_filled(&one, 1, 0x7F));
	CHECK("n=0 vacuously filled", zf_block_is_filled(buf, 0, 0x5A));

	/* n not a multiple of 8 (tail handling) */
	FILL(buf, 1000, 0x66);
	CHECK("n=1000 all-fill", zf_block_is_filled(buf, 1000, 0x66));
	FILL(buf, 1000, 0x66);
	buf[999] = 0x77;
	CHECK("n=1000 last byte dirty", !zf_block_is_filled(buf, 1000, 0x66));

	/* large block crossing many 512B sub-chunks (buf is 8192) */
	FILL(buf, 8192, 0x00);
	CHECK("n=8192 all-fill", zf_block_is_filled(buf, 8192, 0x00));
	FILL(buf, 8192, 0x00);
	buf[511] = 0x55;
	CHECK("n=8192 dirt at sub-chunk boundary (off=511)",
	      !zf_block_is_filled(buf, 8192, 0x00));

	/* patterned fill */
	FILL(buf, 4096, 0x5A);
	CHECK("patterned fill 0x5A", zf_block_is_filled(buf, 4096, 0x5A));
	FILL(buf, 4096, 0x5A);
	buf[4000] = 0x5B;
	CHECK("patterned fill one byte off", !zf_block_is_filled(buf, 4096, 0x5A));

	/* fuzz: compare against a naive byte loop */
	int fuzz_fail = 0;
	for (int round = 0; round < 20000; round++) {
		size_t n = lcg() % 8193;
		uint8_t fill = (uint8_t)lcg();
		FILL(buf, n, fill);
		int flips = n ? (int)(lcg() % 3) : 0; /* 0, 1 or 2 flipped bytes */
		for (int i = 0; i < flips; i++) {
			uint8_t v;
			do {
				v = (uint8_t)lcg();
			} while (v == fill); /* the flip must actually differ */
			buf[lcg() % n] = v;
		}
		bool expect = flips == 0;
		bool got = zf_block_is_filled(buf, n, fill);
		if (got != expect)
			fuzz_fail++;
	}
	CHECK("fuzz 20000 rounds vs naive loop", fuzz_fail == 0);

	free(buf);

	printf("----------------------------------------\n");
	printf("RESULT: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
