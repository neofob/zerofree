/*
 * zerofree - a tool to zero free blocks in an ext2 filesystem
 *
 * Copyright (C) 2004-2012 R M Yorston
 *
 * This file may be redistributed under the terms of the GNU General Public
 * License, version 2.
 *
 * Changes:
 * 2026-07-16  Modern x86-64 Linux rework: single-stream vectorized fill
 *             compare, and kernel zero-offload (BLKZEROOUT on block
 *             devices, fallocate FALLOC_FL_ZERO_RANGE on image files)
 *             when the fill value is zero.  Requires Linux and
 *             e2fsprogs 1.42+.  (posix_fadvise hints were prototyped
 *             and removed: measured as a net loss versus the kernel's
 *             native sequential readahead.)
 *             Note: ZERO_RANGE zeroes image-file ranges at the extent
 *             metadata level; reads return zeros, but stale bits may
 *             physically persist until the extents are rewritten.  Use
 *             -f 0 semantics accordingly if physical overwrite matters.
 * 2026-07-14  Batch I/O over runs of free blocks; remove threading.
 * 2015-10-18  Use memcmp. Suggested by Damien Clarke
 * 2014-11-23  Add thread option Tuan T. Pham
 * 2014-11-22  Fix memory leak. Tuan T. Pham
 * 2010-10-17  Allow non-zero fill value.   Patch from Jacob Nevins.
 * 2007-08-12  Allow use on filesystems mounted read-only.   Patch from
 *             Jan Krämer.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <ext2fs/ext2fs.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef EXT2_FLAG_64BITS
#error "zerofree requires e2fsprogs 1.42 or newer"
#endif

#define OPEN_FLAGS (EXT2_FLAG_RW|EXT2_FLAG_64BITS)
#define FORMAT "\r%llu/%llu/%llu\n"
#define USAGE "usage: %s [-n] [-v] [-d] [-f fillval] filesystem\n"

/* read/write I/O is batched into chunks of at most this many bytes */
#define CHUNK_BYTES ((size_t)(1024*1024))

enum zero_method {
	ZERO_PWRITE,		/* write fill bytes through libext2fs */
	ZERO_BLKZEROOUT,	/* BLKZEROOUT ioctl on a block device */
	ZERO_FALLOC		/* fallocate(FALLOC_FL_ZERO_RANGE) on a file */
};

struct zf_ctx {
	ext2_filsys		fs;
	unsigned int		bs;		/* filesystem block size */
	unsigned int		chunk_blocks;	/* blocks per I/O chunk */
	unsigned char *restrict	buf;		/* chunk read buffer */
	unsigned char *restrict	empty;		/* chunk of fill bytes */
	uint64_t		fill8;		/* fill byte splatted to 64 bits */
	int			zero_fd;	/* offload fd, -1 if disabled */
	enum zero_method	method;
	bool			dryrun;
	bool			verbose;
	bool			discard;
	uint8_t			fill;
	blk64_t			total_blocks;
	blk64_t			free_total;
	blk64_t			free_seen;
	blk64_t			modified;
	int			old_percent;
};

static _Noreturn void bailout(void *mem0, void *mem1)
{
	free(mem0);
	free(mem1);
	exit(1);
}

/*
 * True iff the n bytes at p all equal the fill byte.  Reads a single
 * stream in 512-byte sub-chunks; the inner loop auto-vectorizes at
 * x86-64-v3 and the per-sub-chunk test preserves early exit on dirty
 * blocks.  n is a multiple of 512 (ext2 block sizes are 1KiB..64KiB).
 */
static inline bool block_is_filled(const unsigned char *restrict p, size_t n,
		uint64_t fill8)
{
	/* dirty blocks usually differ in the first byte: bail in O(1) */
	if (p[0] != (unsigned char)fill8) {
		return false;
	}

	for (size_t off = 0; off < n; off += 512) {
		uint64_t acc = 0;
		for (size_t i = 0; i < 512; i += 8) {
			uint64_t w;
			memcpy(&w, p + off + i, 8);
			acc |= w ^ fill8;
		}
		if (acc) {
			return false;
		}
	}
	return true;
}

static void progress(struct zf_ctx *c)
{
	double percent;

	if (!c->verbose) {
		return;
	}

	percent = 100.0 * (double)c->free_seen/(double)c->free_total;
	if ((int)(percent*10) != c->old_percent) {
		fprintf(stderr, "\r%4.1f%%", percent);
		c->old_percent = (int)(percent*10);
	}
}

/*
 * Ask the kernel to zero a block range without transferring data.
 * Returns true on success.  The first "not supported" style failure
 * permanently disables offload; real I/O errors are fatal.
 */
static bool zero_offload(struct zf_ctx *c, blk64_t blk, blk64_t count)
{
	uint64_t off = (uint64_t)blk * c->bs;
	uint64_t len = (uint64_t)count * c->bs;
	int ret;

	if (c->method == ZERO_BLKZEROOUT) {
		uint64_t range[2] = { off, len };
		ret = ioctl(c->zero_fd, BLKZEROOUT, range);
	} else {
		ret = fallocate(c->zero_fd, FALLOC_FL_ZERO_RANGE,
					(off_t)off, (off_t)len);
	}

	if (ret == 0) {
		return true;
	}

	if (errno == EOPNOTSUPP || errno == ENOSYS || errno == ENOTTY ||
			errno == EINVAL) {
		close(c->zero_fd);
		c->zero_fd = -1;
		c->method = ZERO_PWRITE;
		return false;
	}

	fprintf(stderr, "error while writing block\n");
	bailout(c->empty, c->buf);
}

/* Zero (or fill) a run of blocks known to differ from the fill value. */
static void flush_dirty(struct zf_ctx *c, blk64_t blk, unsigned int count)
{
	errcode_t ret;

	if (c->dryrun) {
		return;
	}

	if (c->method != ZERO_PWRITE && zero_offload(c, blk, count)) {
		return;
	}

	ret = io_channel_write_blk64(c->fs->io, blk, (int)count, c->empty);
	if (ret) {
		fprintf(stderr, "error while writing block\n");
		bailout(c->empty, c->buf);
	}
}

static void discard_run(struct zf_ctx *c, blk64_t start, blk64_t end)
{
	errcode_t ret;

	if (!c->dryrun) {
		ret = io_channel_discard(c->fs->io, start, end - start);
		if (ret) {
			fprintf(stderr, " error while discarding block\n");
			bailout(c->empty, c->buf);
		}
	}

	c->modified += end - start;
	for (blk64_t pos = start; pos < end; pos++) {
		++c->free_seen;
		progress(c);
	}
}

static void scan_run(struct zf_ctx *c, blk64_t start, blk64_t end)
{
	for (blk64_t pos = start; pos < end; pos += c->chunk_blocks) {
		unsigned int n = c->chunk_blocks;
		unsigned int dirty = 0, dirty_start = 0;
		errcode_t ret;

		if ((blk64_t)n > end - pos) {
			n = (unsigned int)(end - pos);
		}

		ret = io_channel_read_blk64(c->fs->io, pos, (int)n, c->buf);
		if (ret) {
			fprintf(stderr, "error while reading block\n");
			bailout(c->empty, c->buf);
		}

		for (unsigned int i = 0; i < n; i++) {
			++c->free_seen;
			progress(c);

			if (!block_is_filled(c->buf + (size_t)i * c->bs,
						c->bs, c->fill8)) {
				++c->modified;
				if (!dirty) {
					dirty_start = i;
					dirty = 1;
				}
			} else if (dirty) {
				flush_dirty(c, pos + dirty_start,
						i - dirty_start);
				dirty = 0;
			}
		}

		if (dirty) {
			flush_dirty(c, pos + dirty_start, n - dirty_start);
		}
	}
}

static void zerofree_pass(struct zf_ctx *c)
{
	blk64_t blk = c->fs->super->s_first_data_block;

	if (c->verbose) {
		fprintf(stderr, "\r%4.1f%%", 0.0);
	}

	while (blk < c->total_blocks) {
		blk64_t run_start;

		if (ext2fs_test_block_bitmap2(c->fs->block_map, blk)) {
			blk++;
			continue;
		}

		/* find the maximal run of consecutive free blocks */
		run_start = blk;
		do {
			blk++;
		} while (blk < c->total_blocks &&
				!ext2fs_test_block_bitmap2(c->fs->block_map, blk));

		if (c->discard) {
			discard_run(c, run_start, blk);
		} else {
			scan_run(c, run_start, blk);
		}
	}

	if (c->verbose) {
		printf(FORMAT, c->modified, c->free_seen, c->total_blocks);
	}
}

static void offload_init(struct zf_ctx *c, const char *path)
{
	struct stat st;
	int fd;

	c->zero_fd = -1;
	c->method = ZERO_PWRITE;

	if (c->fill != 0 || c->dryrun || c->discard) {
		return;
	}

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		return;
	}

	if (fstat(fd, &st) == 0 && (S_ISBLK(st.st_mode) || S_ISREG(st.st_mode))) {
		c->method = S_ISBLK(st.st_mode) ? ZERO_BLKZEROOUT : ZERO_FALLOC;
		c->zero_fd = fd;
		return;
	}

	close(fd);
}

int main(int argc, char **argv)
{
	struct zf_ctx ctx = { .zero_fd = -1, .old_percent = -1 };
	errcode_t ret;
	int flags;
	ext2_filsys fs = NULL;
	size_t chunk_bytes;
	int c;
	unsigned int fillval = 0;
	bool verbose = false;
	bool dryrun = false;
	bool discard = false;

	while ( (c=getopt(argc, argv, "nvdf:")) != -1 ) {
		switch (c) {
		case 'n' :
			dryrun = true;
			break;
		case 'v' :
			verbose = true;
			break;
		case 'd':
			discard = true;
			break;
		case 'f' :
			{
				char *endptr;
				fillval = strtol(optarg, &endptr, 0);
				if ( !*optarg || *endptr ) {
					fprintf(stderr, "%s: invalid argument to -f\n", argv[0]);
					return 1;
				} else if ( fillval > 0xFFu ) {
					fprintf(stderr, "%s: fill value must be 0-255\n", argv[0]);
					return 1;
				}
				printf("fillval = %d\n", fillval);
			}
			break;
		default :
			fprintf(stderr, USAGE, argv[0]);
			return 1;
		}
	}

	if ( argc != optind+1 ) {
		fprintf(stderr, USAGE, argv[0]);
		return 1;
	}

	ret = ext2fs_check_if_mounted(argv[optind], &flags);
	if ( ret ) {
		fprintf(stderr, "%s: failed to determine filesystem mount state  %s\n",
			argv[0], argv[optind]);
		return 1;
	}

	if ( (flags & EXT2_MF_MOUNTED) && !(flags & EXT2_MF_READONLY) ) {
		fprintf(stderr, "%s: filesystem %s is mounted rw\n",
			argv[0], argv[optind]);
		return 1;
	}

	ret = ext2fs_open(argv[optind], OPEN_FLAGS, 0, 0, unix_io_manager, &fs);
	if ( ret ) {
		fprintf(stderr, "%s: failed to open filesystem %s\n",
			argv[0], argv[optind]);
		return 1;
	}

	ctx.fs = fs;
	ctx.bs = fs->blocksize;
	ctx.chunk_blocks = CHUNK_BYTES / fs->blocksize;
	if (ctx.chunk_blocks == 0) {
		ctx.chunk_blocks = 1;
	}
	chunk_bytes = (size_t)ctx.chunk_blocks * fs->blocksize;

	ctx.empty = aligned_alloc(4096, chunk_bytes);
	ctx.buf = aligned_alloc(4096, chunk_bytes);

	if ( ctx.empty == NULL || ctx.buf == NULL ) {
		fprintf(stderr, "%s: out of memory (surely not?)\n", argv[0]);
		bailout(ctx.empty, ctx.buf);
	}

	memset(ctx.empty, fillval, chunk_bytes);

	ctx.fill = (uint8_t)fillval;
	ctx.fill8 = 0x0101010101010101ull * (uint8_t)fillval;
	ctx.dryrun = dryrun;
	ctx.verbose = verbose;
	ctx.discard = discard;
	ctx.total_blocks = ext2fs_blocks_count(fs->super);
	ctx.free_total = ext2fs_free_blocks_count(fs->super);

	ret = ext2fs_read_block_bitmap(fs);
	if ( ret ) {
		fprintf(stderr, "%s: error while reading block bitmap\n", argv[0]);
		bailout(ctx.empty, ctx.buf);
	}

	offload_init(&ctx, argv[optind]);

	zerofree_pass(&ctx);

	ret = ext2fs_close(fs);
	if ( ret ) {
		fprintf(stderr, "%s: error while closing filesystem\n", argv[0]);
		bailout(ctx.empty, ctx.buf);
	}

	if (ctx.zero_fd >= 0) {
		close(ctx.zero_fd);
	}
	free(ctx.buf);
	free(ctx.empty);
	return 0;
}
