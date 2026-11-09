/*
 * Unit tests for the runscan.h bitmap run iterator.
 *
 * The iterator yields maximal runs of FREE blocks (bit clear, ext2
 * LSB-first bit order: block b is used iff map[b >> 3] & (1u << (b & 7)))
 * from a block bitmap, scanning one 64-bit word at a time.
 *
 * Build: gcc $(ZF_CFLAGS) -I. -o tests/run_scan tests/run_scan.c
 * No external dependencies; prints "PASS: run_scan" on success,
 * "FAIL: ..." and returns 1 otherwise.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runscan.h"

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

/* Deterministic LCG so the random case is reproducible. */
static uint32_t lcg_state;
static uint32_t lcg(void)
{
	lcg_state = lcg_state * 1664525u + 1013904223u;
	return lcg_state;
}

struct run {
	uint64_t start, len;
};

/* Worst case (alternating bits in the 64 KiB bitmap) yields 32768 runs. */
#define MAX_RUNS 32769
static struct run got_runs[MAX_RUNS];
static struct run ref_runs[MAX_RUNS];

static void clear_bit(unsigned char *map, uint64_t b)
{
	map[b >> 3] &= (unsigned char)~(1u << (b & 7));
}

/* Collect all runs the iterator yields; returns count or -1 on overflow. */
static int collect_runs(struct run_it *it, struct run *out, int cap)
{
	int n = 0;
	for (;;) {
		uint64_t s, l;
		if (next_run(it, &s, &l))
			break;
		if (n >= cap)
			return -1;
		out[n].start = s;
		out[n].len = l;
		n++;
	}
	return n;
}

/* Reference: per-block scan of [lo, hi) using the exact ext2 bit test. */
static int reference_runs(const unsigned char *map, uint64_t lo, uint64_t hi,
			  struct run *out, int cap)
{
	int n = 0;
	bool in_run = false;
	for (uint64_t b = lo; b < hi; b++) {
		bool free_bit = !(map[b >> 3] & (1u << (b & 7)));
		if (free_bit) {
			if (in_run)
				out[n - 1].len++;
			else {
				if (n >= cap)
					return -1;
				out[n].start = b;
				out[n].len = 1;
				n++;
				in_run = true;
			}
		} else {
			in_run = false;
		}
	}
	return n;
}

static int runs_equal(const struct run *a, int na, const struct run *b)
{
	for (int i = 0; i < na; i++)
		if (a[i].start != b[i].start || a[i].len != b[i].len)
			return 0;
	return 1;
}

/* Assert the iterator yields exactly the expected (start, len) list. */
static void check_exact(const char *name, const unsigned char *map,
			uint64_t lo, uint64_t hi,
			const struct run *expect, int n)
{
	struct run_it it;
	int got_n;

	run_it_init(&it, map, lo, hi);
	got_n = collect_runs(&it, got_runs, MAX_RUNS);
	CHECK(name, got_n == n && runs_equal(got_runs, got_n, expect));
}

/* Assert the iterator agrees with the reference per-block scan. */
static void check_range(const char *name, const unsigned char *map,
			uint64_t lo, uint64_t hi)
{
	struct run_it it;
	int exp_n, got_n;

	run_it_init(&it, map, lo, hi);
	got_n = collect_runs(&it, got_runs, MAX_RUNS);
	exp_n = reference_runs(map, lo, hi, ref_runs, MAX_RUNS);
	CHECK(name, exp_n >= 0 && got_n == exp_n &&
	      runs_equal(got_runs, got_n, ref_runs));
}

int main(void)
{
	unsigned char *m = malloc(65536);
	char name[64];
	int i;
	if (!m)
		return 1;

	/* Case 1: all free -- 4096 zeroed bytes -> single run (0, 4096) */
	memset(m, 0x00, 4096);
	{
		const struct run exp[] = { {0, 4096} };
		check_exact("all free -> (0, 4096)", m, 0, 4096, exp, 1);
	}

	/* Case 2: all used -- 0xFF bytes -> no runs */
	memset(m, 0xFF, 4096);
	check_exact("all used -> no runs", m, 0, 4096, NULL, 0);

	/* Case 3: single free block at 100 -> (100, 1) */
	memset(m, 0xFF, 4096);
	clear_bit(m, 100);
	{
		const struct run exp[] = { {100, 1} };
		check_exact("single free block 100 -> (100, 1)", m,
			    0, 4096, exp, 1);
	}

	/* Case 4: run across word boundary -- blocks 60..70 free -> (60, 11) */
	memset(m, 0xFF, 4096);
	for (i = 60; i <= 70; i++)
		clear_bit(m, (uint64_t)i);
	{
		const struct run exp[] = { {60, 11} };
		check_exact("run across word boundary -> (60, 11)", m,
			    0, 4096, exp, 1);
	}

	/* Case 5: unaligned lo/hi -- range [100, 1000), free run 500..599
	 * -> (500, 100); free bits at 50 and 2000 lie outside the range
	 * and must not be reported. */
	memset(m, 0xFF, 4096);
	for (i = 500; i < 600; i++)
		clear_bit(m, (uint64_t)i);
	clear_bit(m, 50);
	clear_bit(m, 2000);
	{
		const struct run exp[] = { {500, 100} };
		check_exact("unaligned lo/hi [100, 1000) -> (500, 100)", m,
			    100, 1000, exp, 1);
	}

	/* Case 6: hi exactly word-aligned -- range [0, 128), all free
	 * -> (0, 128) (guards the 1ULL << 64 edge). */
	memset(m, 0x00, 4096);
	{
		const struct run exp[] = { {0, 128} };
		check_exact("hi word-aligned [0, 128) -> (0, 128)", m,
			    0, 128, exp, 1);
	}

	/* Case 7: random -- 3 seeded PRNGs x 64 KiB random bitmaps, full
	 * extent plus a mid-range [37, 5000) window, compared against the
	 * reference per-block scan. Bytes come from the LCG's middle byte
	 * (bits 16..23): the low byte's bit 0 toggles on every LCG step,
	 * so two adjacent bit-0 positions are never both free, capping
	 * free runs at 15 blocks; the mixed middle byte removes that
	 * correlation so runs of random length (up to ~20 blocks,
	 * including runs that cross a 64-block word boundary and exercise
	 * the iterator's word-by-word stepping) occur. */
	static const uint32_t seeds[] = { 0x5EED01u, 0x5EED02u, 0x5EED03u };
	for (i = 0; i < 3; i++) {
		lcg_state = seeds[i];
		for (int b = 0; b < 65536; b++)
			m[b] = (unsigned char)(lcg() >> 16);	/* middle byte */
		snprintf(name, sizeof name, "random seed %u full extent",
			 seeds[i]);
		check_range(name, m, 0, 65536);
		snprintf(name, sizeof name, "random seed %u window [37, 5000)",
			 seeds[i]);
		check_range(name, m, 37, 5000);
	}

	free(m);

	printf("----------------------------------------\n");
	printf("RESULT: %d checks, %d failed\n", checks, failures);
	if (failures)
		return 1;
	printf("PASS: run_scan\n");
	return 0;
}
