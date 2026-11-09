/* zio: block I/O engine for zerofree (synchronous pread/pwrite or io_uring) */

#ifndef ZIO_H
#define ZIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZIO_MAX_SLOTS 8
#define ZIO_CHUNK_BYTES ((size_t)1 << 22)	/* 4 MiB */

struct zio {
	int		fd;		/* -1 until zio_open succeeds */
	bool		use_uring;
	size_t		bs;
	unsigned char	*restrict fillbuf;	/* ZIO_CHUNK_BYTES, pre-filled */
	unsigned char	*restrict bufs[ZIO_MAX_SLOTS];
	/* private engine state follows (ring, slots, seq counters) */
	uint64_t	pending_blk;
	unsigned int	pending_count;
	size_t		done;
};

/* Open the data fd (rdonly ? O_RDONLY : O_RDWR, both |O_CLOEXEC),
 * allocate buffers (8 slot bufs + fillbuf in uring mode; 1 slot buf +
 * fillbuf in sync mode; all 4096-aligned), memset fillbuf with fill,
 * and try io_uring_setup unless ZEROFREE_NO_URING=1 (env checked here).
 * 0 = ok (use_uring set accordingly); -1 = fd open failed; -2 = OOM.
 * uring init failure is NOT an error: use_uring stays false. */
int zio_open(struct zio *z, const char *path, size_t bs,
	     uint8_t fill, bool rdonly);

/* Free buffers, tear down the ring if any, close the fd.
 * No-op if fd == -1. Never fails. */
void zio_close(struct zio *z);

/* Prime a read of blocks [blk, blk+count) (count*bs <= ZIO_CHUNK_BYTES).
 * uring: submit now (next in-order seq). sync: no-op.
 * Must not be called more than ZIO_MAX_SLOTS ahead of zio_read. */
void zio_prime(struct zio *z, uint64_t blk, unsigned int count);

/* Wait for the next in-order primed read to complete; return its buffer,
 * or NULL on error (having printed "error while reading block\n" to
 * stderr). sync mode: performs the pread here (blocking loop).
 * Protocol: res==0 -> error; 0<res<len -> bounded remainder resubmit. */
unsigned char *zio_read(struct zio *z);

/* Write fillbuf[0 .. count*bs) to blocks [blk, blk+count).
 * uring: submit, return 0 immediately (completion checked at drain).
 * sync: blocking pwrite loop. Returns 1 on error (having printed
 * "error while writing block\n"). */
int zio_flush_dirty(struct zio *z, uint64_t blk, unsigned int count);

/* Wait for all in-flight writes; 0 ok, 1 on error (message printed).
 * sync: no-op, returns 0. */
int zio_drain(struct zio *z);

#endif
