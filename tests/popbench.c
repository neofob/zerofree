/* Microbenchmark: can POPCNT speed up block_is_filled?
 * CUR: current zerofree impl (first-byte bail + 512B word-OR chunks, AVX2 auto-vec)
 * P1 : popcnt-sum per 64-bit word (popcnt(w^fill8) accumulated, test per 512B)
 * P2 : OR-accumulate, but final zero-test done via popcnt(acc)
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static __attribute__((noinline)) bool is_filled_cur(const unsigned char *restrict p, size_t n, uint64_t fill8)
{
	if (p[0] != (unsigned char)fill8)
		return false;
	for (size_t off = 0; off < n; off += 512) {
		uint64_t acc = 0;
		for (size_t i = 0; i < 512; i += 8) {
			uint64_t w;
			memcpy(&w, p + off + i, 8);
			acc |= w ^ fill8;
		}
		if (acc)
			return false;
	}
	return true;
}

static __attribute__((noinline)) bool is_filled_p1(const unsigned char *restrict p, size_t n, uint64_t fill8)
{
	if (p[0] != (unsigned char)fill8)
		return false;
	for (size_t off = 0; off < n; off += 512) {
		uint64_t cnt = 0;
		for (size_t i = 0; i < 512; i += 8) {
			uint64_t w;
			memcpy(&w, p + off + i, 8);
			cnt += (uint64_t)__builtin_popcountll(w ^ fill8);
		}
		if (cnt)
			return false;
	}
	return true;
}

static __attribute__((noinline)) bool is_filled_p2(const unsigned char *restrict p, size_t n, uint64_t fill8)
{
	if (p[0] != (unsigned char)fill8)
		return false;
	for (size_t off = 0; off < n; off += 512) {
		uint64_t acc = 0;
		for (size_t i = 0; i < 512; i += 8) {
			uint64_t w;
			memcpy(&w, p + off + i, 8);
			acc |= w ^ fill8;
		}
		if (__builtin_popcountll(acc))
			return false;
	}
	return true;
}

/* forced hardware popcnt: compiler cannot elide or transform it */
static inline uint64_t popcnt_forced(uint64_t x)
{
	uint64_t r;
	__asm__ ("popcnt %1, %0" : "=r"(r) : "r"(x));
	return r;
}

static __attribute__((noinline)) bool is_filled_p3(const unsigned char *restrict p, size_t n, uint64_t fill8)
{
	if (p[0] != (unsigned char)fill8)
		return false;
	for (size_t off = 0; off < n; off += 512) {
		uint64_t cnt = 0;
		for (size_t i = 0; i < 512; i += 8) {
			uint64_t w;
			memcpy(&w, p + off + i, 8);
			cnt += popcnt_forced(w ^ fill8);
		}
		if (cnt)
			return false;
	}
	return true;
}

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

#define BUF_BYTES (1u << 20)

int main(void)
{
	unsigned char *buf = aligned_alloc(4096, BUF_BYTES);
	const uint64_t fill8 = 0;
	size_t sizes[] = { 1024, 4096 };
	const char *cases[] = { "clean", "dirty@first", "dirty@last" };
	volatile unsigned sink = 0;

	for (int s = 0; s < 2; s++) {
		size_t bs = sizes[s];
		size_t nblk = BUF_BYTES / bs;
		for (int c = 0; c < 3; c++) {
			memset(buf, 0, BUF_BYTES);
			if (c == 1)
				for (size_t b = 0; b < nblk; b++) buf[b*bs] = 0xFF;
			if (c == 2)
				for (size_t b = 0; b < nblk; b++) buf[b*bs + bs-1] = 0xFF;

			int iters = (c == 1) ? 40000 : 4000;
			double t, best[4] = { 1e9, 1e9, 1e9, 1e9 };
			for (int rep = 0; rep < 3; rep++) {
				t = now();
				for (int i = 0; i < iters; i++)
					for (size_t b = 0; b < nblk; b++)
						sink += is_filled_cur(buf + b*bs, bs, fill8);
				t = now() - t; if (t < best[0]) best[0] = t;

				t = now();
				for (int i = 0; i < iters; i++)
					for (size_t b = 0; b < nblk; b++)
						sink += is_filled_p1(buf + b*bs, bs, fill8);
				t = now() - t; if (t < best[1]) best[1] = t;

				t = now();
				for (int i = 0; i < iters; i++)
					for (size_t b = 0; b < nblk; b++)
						sink += is_filled_p2(buf + b*bs, bs, fill8);
				t = now() - t; if (t < best[2]) best[2] = t;

				t = now();
				for (int i = 0; i < iters; i++)
					for (size_t b = 0; b < nblk; b++)
						sink += is_filled_p3(buf + b*bs, bs, fill8);
				t = now() - t; if (t < best[3]) best[3] = t;
			}
			double gib = (double)iters * BUF_BYTES / (1u << 30);
			printf("bs=%-5zu %-12s  CUR %7.2f   P1(builtin) %7.2f   P2(test) %7.2f   P3(forced) %7.2f GiB/s\n",
			       bs, cases[c], gib/best[0], gib/best[1], gib/best[2], gib/best[3]);
		}
	}
	(void)sink;
	free(buf);
	return 0;
}
