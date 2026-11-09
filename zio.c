#include "zio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

int zio_open(struct zio *z, const char *path, size_t bs,
	     uint8_t fill, bool rdonly)
{
	int flags = O_CLOEXEC | (rdonly ? O_RDONLY : O_RDWR);

	z->fd = open(path, flags);
	if (z->fd < 0)
		return -1;

	z->use_uring = false;
	z->bs = bs;
	z->pending_blk = 0;
	z->pending_count = 0;
	z->done = 0;
	for (unsigned int i = 0; i < ZIO_MAX_SLOTS; i++)
		z->bufs[i] = NULL;

	z->fillbuf = aligned_alloc(4096, ZIO_CHUNK_BYTES);
	if (!z->fillbuf) {
		zio_close(z);
		return -2;
	}
	memset(z->fillbuf, fill, ZIO_CHUNK_BYTES);

	z->bufs[0] = aligned_alloc(4096, ZIO_CHUNK_BYTES);
	if (!z->bufs[0]) {
		zio_close(z);
		return -2;
	}

	return 0;
}

void zio_close(struct zio *z)
{
	if (z->fd == -1)
		return;

	for (unsigned int i = 0; i < ZIO_MAX_SLOTS; i++) {
		free(z->bufs[i]);
		z->bufs[i] = NULL;
	}
	free(z->fillbuf);
	z->fillbuf = NULL;

	close(z->fd);
	z->fd = -1;
}

void zio_prime(struct zio *z, uint64_t blk, unsigned int count)
{
	z->pending_blk = blk;
	z->pending_count = count;
}

unsigned char *zio_read(struct zio *z)
{
	unsigned char *buf = z->bufs[0];
	size_t len = (size_t)z->pending_count * z->bs;
	off_t base = (off_t)(z->pending_blk * z->bs);

	z->done = 0;
	while (z->done < len) {
		ssize_t r = pread(z->fd, buf + z->done, len - z->done,
				  base + (off_t)z->done);

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0) {
			fputs("error while reading block\n", stderr);
			return NULL;
		}
		z->done += (size_t)r;
	}
	return buf;
}

int zio_flush_dirty(struct zio *z, uint64_t blk, unsigned int count)
{
	size_t len = (size_t)count * z->bs;
	off_t base = (off_t)(blk * z->bs);

	z->done = 0;
	while (z->done < len) {
		ssize_t r = pwrite(z->fd, z->fillbuf + z->done, len - z->done,
				   base + (off_t)z->done);

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0) {
			fputs("error while writing block\n", stderr);
			return 1;
		}
		z->done += (size_t)r;
	}
	return 0;
}

int zio_drain(struct zio *z)
{
	(void)z;
	return 0;
}
