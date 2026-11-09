/* zio engine unit test: zio_test {sync|uring} */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "zio.h"

#define BS 4096u
#define NBLK 2432u
#define FILE_BYTES ((size_t)NBLK * BS)

static char path[4096];
static const char *mode;

static uint8_t pat(uint64_t off)
{
	return (uint8_t)((uint8_t)((off * 2654435761u) >> 24) ^ (uint8_t)(off >> 8));
}

static void cleanup(void)
{
	unlink(path);
}

static void fail(const char *what) __attribute__((noreturn));
static void fail(const char *what)
{
	printf("FAIL: zio_test (%s): %s\n", mode, what);
	exit(1);
}

#define CHECK(cond) do { if (!(cond)) fail(#cond); } while (0)

static bool read_exact(int fd, unsigned char *buf, size_t len, off_t off)
{
	size_t done = 0;

	while (done < len) {
		ssize_t r = pread(fd, buf + done, len - done, off + (off_t)done);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return false;
		done += (size_t)r;
	}
	return true;
}

static bool is_pat(const unsigned char *buf, uint64_t off, size_t len)
{
	for (size_t i = 0; i < len; i++)
		if (buf[i] != pat(off + i))
			return false;
	return true;
}

static void fill_file(int fd)
{
	unsigned char *buf = malloc(FILE_BYTES);
	size_t done = 0;

	if (!buf)
		fail("out of memory");
	for (size_t i = 0; i < FILE_BYTES; i++)
		buf[i] = pat(i);
	while (done < FILE_BYTES) {
		ssize_t r = pwrite(fd, buf + done, FILE_BYTES - done, (off_t)done);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			fail("pattern write");
		done += (size_t)r;
	}
	free(buf);
	if (fsync(fd) != 0)
		fail("fsync");
}

int main(int argc, char **argv)
{
	static const struct { uint64_t blk; unsigned int count; } chunks[3] = {
		{ 0, 1024 }, { 1024, 1024 }, { 2048, 384 },
	};
	unsigned char *ptr[3];
	unsigned char *run, edge[BS], blk0[BS];
	struct zio z;
	bool want_sync;
	const char *dir;
	int fd, vfd;
	FILE *f;
	size_t got;

	if (argc != 2 || (strcmp(argv[1], "sync") && strcmp(argv[1], "uring"))) {
		fprintf(stderr, "usage: %s sync|uring\n", argv[0]);
		return 2;
	}
	mode = argv[1];
	want_sync = !strcmp(mode, "sync");

	dir = getenv("TMPDIR");
	if (!dir || !*dir)
		dir = "/tmp";
	snprintf(path, sizeof(path), "%s/zio_test.img", dir);
	atexit(cleanup);

	if (want_sync)
		setenv("ZEROFREE_NO_URING", "1", 1);
	else
		unsetenv("ZEROFREE_NO_URING");

	fd = open(path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
	if (fd < 0)
		fail("create scratch file");
	fill_file(fd);
	close(fd);

	if (zio_open(&z, path, BS, 0x5A, false) != 0)
		fail("zio_open");
	CHECK(z.fd >= 0);
	if (want_sync) {
		CHECK(z.use_uring == false);
		if (zio_read(&z) != NULL)
			fail("unprimed zio_read returned a buffer");
	} else if (!z.use_uring) {
		printf("SKIP: zio_test (no io_uring)\n");
		zio_close(&z);
		return 0;
	}

	for (int i = 0; i < 3; i++) {
		unsigned char *b;

		zio_prime(&z, chunks[i].blk, chunks[i].count);
		b = zio_read(&z);
		if (!b)
			fail("zio_read");
		if (!is_pat(b, chunks[i].blk * BS, (size_t)chunks[i].count * BS))
			fail("read data mismatch");
		ptr[i] = b;
	}
	if (!want_sync)
		CHECK(ptr[0] != ptr[1] && ptr[1] != ptr[2] && ptr[0] != ptr[2]);

	if (zio_flush_dirty(&z, 100, 50) != 0)
		fail("zio_flush_dirty");
	if (zio_drain(&z) != 0)
		fail("zio_drain");

	vfd = open(path, O_RDONLY | O_CLOEXEC);
	if (vfd < 0)
		fail("reopen scratch file");
	run = malloc(50 * BS);
	if (!run || !read_exact(vfd, run, 50 * BS, 100 * BS))
		fail("read back flushed run");
	for (size_t i = 0; i < 50 * BS; i++)
		if (run[i] != 0x5A)
			fail("flushed run not filled");
	free(run);
	if (!read_exact(vfd, edge, BS, 99 * BS) || !is_pat(edge, 99 * BS, BS))
		fail("block before run clobbered");
	if (!read_exact(vfd, edge, BS, 150 * BS) || !is_pat(edge, 150 * BS, BS))
		fail("block after run clobbered");
	close(vfd);

	zio_close(&z);
	f = fopen(path, "rb");
	if (!f)
		fail("fopen scratch file");
	got = fread(blk0, 1, BS, f);
	fclose(f);
	if (got != BS || !is_pat(blk0, 0, BS))
		fail("block 0 changed after zio_close");

	printf("PASS: zio_test (%s)\n", mode);
	return 0;
}
