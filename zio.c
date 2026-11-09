#include "zio.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#define RING_ENTRIES 256
#define WRITE_TAG (1ULL << 63)

static const char read_msg[] = "error while reading block\n";
static const char write_msg[] = "error while writing block\n";

static void ring_init(struct zio_ring *r)
{
	memset(r, 0, sizeof(*r));
	r->fd = -1;
}

static void ring_teardown(struct zio_ring *r)
{
	if (r->sqes)
		munmap(r->sqes, r->sqes_sz);
	if (r->cq)
		munmap(r->cq, r->cq_sz);
	if (r->sq)
		munmap(r->sq, r->sq_sz);
	if (r->fd >= 0)
		close(r->fd);
	ring_init(r);
}

/* 0 = ring ready; -1 = nothing left behind, caller falls back to sync */
static int ring_setup(struct zio_ring *r)
{
	struct io_uring_params p;
	char *sq, *cq;
	int fd;

	memset(&p, 0, sizeof(p));
	fd = (int)syscall(SYS_io_uring_setup, RING_ENTRIES, &p);
	if (fd < 0)
		return -1;

	ring_init(r);
	r->fd = fd;
	r->sq_entries = p.sq_entries;
	r->sq_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned int);
	r->cq_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
	r->sqes_sz = p.sq_entries * sizeof(struct io_uring_sqe);

	r->sq = mmap(NULL, r->sq_sz, PROT_READ | PROT_WRITE,
		     MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
	if (r->sq == MAP_FAILED) {
		r->sq = NULL;
		goto fail;
	}
	r->cq = mmap(NULL, r->cq_sz, PROT_READ | PROT_WRITE,
		     MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
	if (r->cq == MAP_FAILED) {
		r->cq = NULL;
		goto fail;
	}
	r->sqes = mmap(NULL, r->sqes_sz, PROT_READ | PROT_WRITE,
		       MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
	if (r->sqes == MAP_FAILED) {
		r->sqes = NULL;
		goto fail;
	}

	sq = r->sq;
	cq = r->cq;
	r->sq_head = (unsigned int *)(sq + p.sq_off.head);
	r->sq_tail = (unsigned int *)(sq + p.sq_off.tail);
	r->sq_mask = (unsigned int *)(sq + p.sq_off.ring_mask);
	r->sq_array = (unsigned int *)(sq + p.sq_off.array);
	r->cq_head = (unsigned int *)(cq + p.cq_off.head);
	r->cq_tail = (unsigned int *)(cq + p.cq_off.tail);
	r->cq_mask = (unsigned int *)(cq + p.cq_off.ring_mask);
	r->cqes = (unsigned char *)(cq + p.cq_off.cqes);
	return 0;

fail:
	ring_teardown(r);
	return -1;
}

static long ring_enter(struct zio_ring *r, unsigned int submit,
		       unsigned int min_complete, unsigned int flags)
{
	return syscall(SYS_io_uring_enter, r->fd, submit, min_complete, flags,
		       NULL, 0);
}

/* Submit everything written so far. 0 = ok, -1 = fatal enter failure.
 * enter returns the number of SQEs consumed, not just 0. */
static int ring_submit(struct zio_ring *r)
{
	while (r->pending) {
		long rv = ring_enter(r, r->pending, 0, 0);

		if (rv < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return -1;
		}
		if (rv == 0)
			return -1;
		r->pending -= (unsigned int)rv < r->pending ?
			(unsigned int)rv : r->pending;
	}
	return 0;
}

/* Block until at least one CQE is available. 0 = ok, -1 = fatal. */
static int ring_wait(struct zio_ring *r)
{
	for (;;) {
		long rv = ring_enter(r, 0, 1, IORING_ENTER_GETEVENTS);

		if (rv >= 0)
			return 0;
		if (errno == EINTR || errno == EAGAIN)
			continue;
		return -1;
	}
}

static struct io_uring_sqe *ring_get_sqe(struct zio_ring *r)
{
	unsigned int tail = *r->sq_tail;
	unsigned int head = __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE);
	unsigned int idx;
	struct io_uring_sqe *sqe;

	if (tail - head == r->sq_entries)
		return NULL;
	idx = tail & *r->sq_mask;
	sqe = (struct io_uring_sqe *)r->sqes + idx;
	memset(sqe, 0, sizeof(*sqe));
	r->sq_array[idx] = idx;
	return sqe;
}

static void ring_publish_sqe(struct zio_ring *r)
{
	__atomic_store_n(r->sq_tail, *r->sq_tail + 1, __ATOMIC_RELEASE);
	r->pending++;
}

/* Queue one READ/WRITE and submit it eagerly. 0 = ok, -1 = fatal. */
static int ring_queue(struct zio_ring *r, uint8_t opcode, int fd,
		      uint64_t off, const void *addr, size_t len,
		      uint64_t user_data)
{
	struct io_uring_sqe *sqe = ring_get_sqe(r);

	if (!sqe) {
		if (ring_submit(r) < 0)
			return -1;
		sqe = ring_get_sqe(r);
		if (!sqe)
			return -1;
	}
	sqe->opcode = opcode;
	sqe->fd = fd;
	sqe->off = off;
	sqe->addr = (uint64_t)(uintptr_t)addr;
	sqe->len = (uint32_t)len;
	sqe->user_data = user_data;
	ring_publish_sqe(r);
	return ring_submit(r);
}

static void read_slot_fail(struct zio *z, struct zio_slot *s)
{
	s->err = true;
	s->in_flight = false;
	z->rd_inflight--;
}

static int read_slot_queue(struct zio *z, uint64_t seq)
{
	struct zio_slot *s = &z->slots[seq & (ZIO_MAX_SLOTS - 1)];

	return ring_queue(&z->ring, IORING_OP_READ, z->fd,
			  s->blk * z->bs + s->done_bytes,
			  z->bufs[seq & (ZIO_MAX_SLOTS - 1)] + s->done_bytes,
			  s->count * z->bs - s->done_bytes, seq);
}

static void write_rec_fail(struct zio *z, struct zio_wrec *w)
{
	w->used = false;
	z->wr_inflight--;
	z->wr_failed = true;
}

static int write_rec_queue(struct zio *z, unsigned int idx)
{
	struct zio_wrec *w = &z->wrecs[idx];

	return ring_queue(&z->ring, IORING_OP_WRITE, z->fd, w->off + w->done,
			  z->fillbuf + w->done, w->len - w->done,
			  WRITE_TAG | idx);
}

/* Route every CQE currently in the CQ ring; reads and writes alike.
 * Short transfers are resubmitted for the remainder, res <= 0 is a
 * failure (no resubmit), so every op strictly progresses or ends. */
static void reap_cqes(struct zio *z)
{
	struct zio_ring *r = &z->ring;
	unsigned int head = *r->cq_head;
	unsigned int tail = __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE);

	while (head != tail) {
		struct io_uring_cqe *cqe = (struct io_uring_cqe *)
			(r->cqes + (size_t)(head & *r->cq_mask) *
			 sizeof(struct io_uring_cqe));
		uint64_t ud = cqe->user_data;
		int res = cqe->res;

		head++;
		if (ud & WRITE_TAG) {
			unsigned int idx = (unsigned int)(ud & ~WRITE_TAG);
			struct zio_wrec *w;

			if (idx >= ZIO_MAX_WRITES || !z->wrecs[idx].used)
				continue;
			w = &z->wrecs[idx];
			if (res <= 0) {
				write_rec_fail(z, w);
				continue;
			}
			w->done += (size_t)res;
			if (w->done >= w->len) {
				w->used = false;
				z->wr_inflight--;
			} else if (write_rec_queue(z, idx) < 0) {
				write_rec_fail(z, w);
			}
		} else {
			struct zio_slot *s = &z->slots[ud & (ZIO_MAX_SLOTS - 1)];

			if (!s->in_flight)
				continue;
			s->res = res;
			if (res <= 0) {
				read_slot_fail(z, s);
				continue;
			}
			s->done_bytes += (uint64_t)res;
			if (s->done_bytes >= s->count * z->bs) {
				s->in_flight = false;
				z->rd_inflight--;
			} else if (read_slot_queue(z, ud) < 0) {
				read_slot_fail(z, s);
			}
		}
	}
	__atomic_store_n(r->cq_head, head, __ATOMIC_RELEASE);
}

static bool env_no_uring(void)
{
	const char *v = getenv("ZEROFREE_NO_URING");

	return v && !strcmp(v, "1");
}

int zio_open(struct zio *z, const char *path, size_t bs,
	     uint8_t fill, bool rdonly)
{
	int flags = O_CLOEXEC | (rdonly ? O_RDONLY : O_RDWR);
	unsigned int nbufs;

	z->fd = open(path, flags);
	if (z->fd < 0)
		return -1;

	z->use_uring = false;
	z->bs = bs;
	z->pending_blk = 0;
	z->pending_count = 0;
	z->done = 0;
	z->fillbuf = NULL;
	ring_init(&z->ring);
	memset(z->slots, 0, sizeof(z->slots));
	memset(z->wrecs, 0, sizeof(z->wrecs));
	z->next_seq = z->want_seq = 0;
	z->rd_inflight = z->wr_inflight = 0;
	z->wr_failed = false;
	for (unsigned int i = 0; i < ZIO_MAX_SLOTS; i++)
		z->bufs[i] = NULL;

	z->fillbuf = aligned_alloc(4096, ZIO_CHUNK_BYTES);
	if (!z->fillbuf) {
		zio_close(z);
		return -2;
	}
	memset(z->fillbuf, fill, ZIO_CHUNK_BYTES);

	if (!env_no_uring() && ring_setup(&z->ring) == 0)
		z->use_uring = true;

	nbufs = z->use_uring ? ZIO_MAX_SLOTS : 1;
	for (unsigned int i = 0; i < nbufs; i++) {
		z->bufs[i] = aligned_alloc(4096, ZIO_CHUNK_BYTES);
		if (!z->bufs[i]) {
			zio_close(z);
			return -2;
		}
	}

	return 0;
}

void zio_close(struct zio *z)
{
	if (z->fd == -1)
		return;

	/* Best effort: let in-flight ops finish before their buffers go. */
	while (z->use_uring && (z->rd_inflight || z->wr_inflight)) {
		if (ring_submit(&z->ring) < 0 || ring_wait(&z->ring) < 0)
			break;
		reap_cqes(z);
	}
	ring_teardown(&z->ring);

	for (unsigned int i = 0; i < ZIO_MAX_SLOTS; i++) {
		free(z->bufs[i]);
		z->bufs[i] = NULL;
	}
	free(z->fillbuf);
	z->fillbuf = NULL;

	close(z->fd);
	z->fd = -1;
	z->use_uring = false;
	z->bs = 0;
	z->pending_blk = 0;
	z->pending_count = 0;
	z->done = 0;
	z->rd_inflight = z->wr_inflight = 0;
}

void zio_prime(struct zio *z, uint64_t blk, unsigned int count)
{
	if (z->use_uring) {
		uint64_t seq = z->next_seq++;
		struct zio_slot *s = &z->slots[seq & (ZIO_MAX_SLOTS - 1)];

		s->blk = blk;
		s->count = count;
		s->done_bytes = 0;
		s->res = 0;
		s->err = false;
		s->in_flight = true;
		z->rd_inflight++;
		if (read_slot_queue(z, seq) < 0)
			read_slot_fail(z, s);
		return;
	}
	z->pending_blk = blk;
	z->pending_count = count;
}

static unsigned char *uring_read(struct zio *z)
{
	uint64_t seq = z->want_seq;
	struct zio_slot *s = &z->slots[seq & (ZIO_MAX_SLOTS - 1)];

	if (z->want_seq == z->next_seq) {
		fputs(read_msg, stderr);
		return NULL;
	}
	z->want_seq++;

	reap_cqes(z);
	while (s->in_flight) {
		if (ring_wait(&z->ring) < 0) {
			read_slot_fail(z, s);
			break;
		}
		reap_cqes(z);
	}
	if (s->err) {
		fputs(read_msg, stderr);
		return NULL;
	}
	return z->bufs[seq & (ZIO_MAX_SLOTS - 1)];
}

unsigned char *zio_read(struct zio *z)
{
	unsigned char *buf = z->bufs[0];
	size_t len = (size_t)z->pending_count * z->bs;
	off_t base = (off_t)(z->pending_blk * z->bs);

	if (z->use_uring)
		return uring_read(z);
	if (z->pending_count == 0) {
		fputs(read_msg, stderr);
		return NULL;
	}

	z->done = 0;
	while (z->done < len) {
		ssize_t r = pread(z->fd, buf + z->done, len - z->done,
				  base + (off_t)z->done);

		if (r < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		if (r <= 0) {
			fputs(read_msg, stderr);
			return NULL;
		}
		z->done += (size_t)r;
	}
	return buf;
}

static int uring_flush_dirty(struct zio *z, uint64_t blk, unsigned int count)
{
	size_t len = (size_t)count * z->bs;
	unsigned int idx = 0;
	struct zio_wrec *w;

	if (len == 0)
		return 0;
	for (;;) {
		reap_cqes(z);
		while (idx < ZIO_MAX_WRITES && z->wrecs[idx].used)
			idx++;
		if (idx < ZIO_MAX_WRITES)
			break;
		idx = 0;
		if (ring_wait(&z->ring) < 0) {
			fputs(write_msg, stderr);
			return 1;
		}
	}
	w = &z->wrecs[idx];
	w->off = blk * z->bs;
	w->len = len;
	w->done = 0;
	w->used = true;
	z->wr_inflight++;
	if (write_rec_queue(z, idx) < 0) {
		write_rec_fail(z, w);
		fputs(write_msg, stderr);
		return 1;
	}
	return 0;
}

int zio_flush_dirty(struct zio *z, uint64_t blk, unsigned int count)
{
	size_t len = (size_t)count * z->bs;
	off_t base = (off_t)(blk * z->bs);

	if (z->use_uring)
		return uring_flush_dirty(z, blk, count);
	z->done = 0;
	while (z->done < len) {
		ssize_t r = pwrite(z->fd, z->fillbuf + z->done, len - z->done,
				   base + (off_t)z->done);

		if (r < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		if (r <= 0) {
			fputs(write_msg, stderr);
			return 1;
		}
		z->done += (size_t)r;
	}
	return 0;
}

int zio_drain(struct zio *z)
{
	if (!z->use_uring)
		return 0;

	reap_cqes(z);
	while (z->wr_inflight) {
		if (ring_wait(&z->ring) < 0) {
			fputs(write_msg, stderr);
			return 1;
		}
		reap_cqes(z);
	}
	if (z->wr_failed) {
		z->wr_failed = false;
		fputs(write_msg, stderr);
		return 1;
	}
	return 0;
}
