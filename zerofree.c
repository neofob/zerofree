/*
 * zerofree - a tool to zero free blocks in an ext2 filesystem
 *
 * Copyright (C) 2004-2012 R M Yorston
 *
 * This file may be redistributed under the terms of the GNU General Public
 * License, version 2.
 *
 * Changes:
 * 2026-11-08  Modern x86-64 Linux rewrite (Linux + e2fsprogs 1.42+):
 *             - remove the threaded -t mode entirely (its per-IO global
 *               mutex serialized all I/O anyway, and its partitioning
 *               silently skipped boundary blocks);
 *             - coalesce runs of consecutive free blocks into batched
 *               multi-block reads/writes (up to 1 MiB per syscall);
 *             - vectorized single-stream fill compare (zerofree.h),
 *               auto-vectorized to AVX2 at -march=x86-64-v3;
 *             - 64-bit filesystem support (EXT2_FLAG_64BITS);
 *             - close the filesystem on I/O errors (old code leaked it);
 *             - when the fill value is zero, dirty sub-runs are zeroed
 *               by the kernel instead of by writing zeros:
 *               BLKZEROOUT on block devices, fallocate(FALLOC_FL_ZERO_RANGE)
 *               on image files.
 *             NOTE on zero-offload: FALLOC_FL_ZERO_RANGE zeroes image-file
 *             ranges at the extent-metadata level; the bytes read back as
 *             zero (this tool's contract), but stale data may physically
 *             persist in the host storage until the extents are rewritten.
 *             BLKZEROOUT asks the device to zero sectors, which is stronger
 *             but device-dependent.  For either, SSD/FTL remanence
 *             applies.  Use the plain zero-write path (e.g. a non-zero -f
 *             value) if physical overwrite is what you need.
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "runscan.h"
#include "zerofree.h"
#include "zio.h"

#ifndef EXT2_FLAG_64BITS
#error "zerofree requires e2fsprogs 1.42 or newer"
#endif

#define OPEN_FLAGS (EXT2_FLAG_RW | EXT2_FLAG_64BITS)
#define USAGE "usage: %s [-n] [-v] [-d] [-f fillval] filesystem\n"

enum zero_method {
	ZERO_PWRITE,		/* write the fill buffer (always works) */
	ZERO_RANGE,		/* fallocate(FALLOC_FL_ZERO_RANGE), files */
	ZERO_BLKZEROOUT,	/* BLKZEROOUT ioctl, block devices */
};

struct zf_ctx {
	ext2_filsys	fs;
	struct zio	zio;		/* data I/O engine (fill mode only) */
	unsigned char	*fmap;		/* free-block map: bit i = block
					 * s_first_data_block + i, set = used */
	size_t		bs;		/* filesystem block size (bytes) */
	unsigned int	chunk_blocks;	/* max blocks per batched I/O */
	uint8_t		fill;
	bool		dryrun;
	bool		discard;
	bool		verbose;
	blk64_t		free_blk;
	blk64_t		free_total;
	blk64_t		modified;
	blk64_t		total;
	int		old_percent;
	int		zero_fd;	/* side fd for zero-offload, -1 if off */
	enum zero_method	method;
};

static void progress(struct zf_ctx *c)
{
	double percent;

	if (!c->verbose)
		return;

	/* Guard against an inconsistent superblock (free_total == 0):
	 * inside the loop free_blk >= 1, so percent is 100.0. */
	percent = 100.0 * (double)c->free_blk /
		(double)(c->free_total ? c->free_total : c->free_blk);
	if ((int)(percent * 10) != c->old_percent) {
		fprintf(stderr, "\r%4.1f%%", percent);
		c->old_percent = (int)(percent * 10);
	}
}

/*
 * Ask the kernel to zero blocks [blk, blk+count) without transferring
 * data.  Returns 0 on success, 1 if the device/file system does not
 * support it (caller falls back to the fill-buffer write permanently),
 * 2 on a real I/O error.
 */
static int zero_range(struct zf_ctx *c, blk64_t blk, blk64_t count)
{
	const off_t off = (off_t)blk * c->bs;
	const off_t len = (off_t)count * c->bs;
	int err;

	if (c->method == ZERO_BLKZEROOUT) {
		struct {
			__u64	length;
			__u64	start;
		} range = { .length = (__u64)len, .start = (__u64)off };

		if (ioctl(c->zero_fd, BLKZEROOUT, &range) == 0)
			return 0;
		err = errno;
	} else {
		if (fallocate(c->zero_fd, FALLOC_FL_ZERO_RANGE, off, len) == 0)
			return 0;
		err = errno;
	}

	switch (err) {
	case EOPNOTSUPP:
	case ENOSYS:
	case ENOTTY:
	case EINVAL:
		return 1;
	default:
		return 2;
	}
}

/* Zero one dirty sub-run; writes the fill buffer when offload is off.
 * On any failure prints the pinned "error while writing block" exactly
 * once and returns 1.  The engine prints it on the pwrite path; we print
 * it here only for the offload path, which the engine never sees. */
static int flush_dirty(struct zf_ctx *c, blk64_t blk, blk64_t count)
{
	if (c->method != ZERO_PWRITE) {
		int rc = zero_range(c, blk, count);
		if (rc == 1) {	/* unsupported: fall back permanently */
			c->method = ZERO_PWRITE;
			if (c->zero_fd >= 0)
				close(c->zero_fd);
			c->zero_fd = -1;
			/* fall through to the engine write below */
		} else if (rc == 2) {
			fprintf(stderr, "error while writing block\n");
			return 1;
		} else
			return 0;
	}

	return zio_flush_dirty(&c->zio, (uint64_t)blk, (unsigned int)count);
}

/* Discard mode: one io_channel_discard() per whole run (no reads);
 * every block in the run counts as modified. */
static int zf_pass_discard(struct zf_ctx *c, struct run_it *it)
{
	uint64_t run_start, run_len;

	while (next_run(it, &run_start, &run_len) == 0) {
		errcode_t ret = 0;
		size_t i;

		run_start += c->fs->super->s_first_data_block;
		if (!c->dryrun)
			ret = io_channel_discard(c->fs->io, run_start, run_len);
		for (i = 0; i < (size_t)run_len; i++) {
			c->free_blk++;
			c->modified++;
			progress(c);
		}
		if (ret) {
			fprintf(stderr, "error while discarding block\n");
			return 1;
		}
	}
	return 0;
}

/* One primed read: blocks [blk, blk+count). */
struct zf_work {
	uint64_t	blk;
	unsigned int	count;
};

/*
 * Fill mode: a software pipeline over the free runs.
 *
 * Invariants:
 *  - at most ZIO_MAX_SLOTS reads are primed ahead of the consume point
 *    (nwin <= depth <= ZIO_MAX_SLOTS), as the engine requires; depth is
 *    1 in the synchronous engine, which has a single slot;
 *  - reads are consumed strictly in submission order (zio_read enforces
 *    it), so chunks are processed, and progress is reported, in
 *    ascending block order exactly as the old per-block scan did;
 *  - a prime never crosses a run boundary (it is clamped to run_end),
 *    but the window as a whole does span runs;
 *  - a returned buffer must be fully consumed before the next zio_prime
 *    that could reuse its slot: zio_read of seq N frees slot N&7 for the
 *    prime of seq N+8, which only happens after this chunk is done.
 */
static int zf_pass_fill(struct zf_ctx *c, struct run_it *it)
{
	struct zf_work win[ZIO_MAX_SLOTS];
	unsigned int head = 0, nwin = 0;
	bool have_run = false, no_more = false;
	uint64_t run_pos = 0, run_end = 0;
	/* The sync engine has one slot buffer and remembers a single primed
	 * read, so it must be primed and read in lockstep (depth 1). */
	const unsigned int depth = c->zio.use_uring ? ZIO_MAX_SLOTS : 1;

	for (;;) {
		/* window_fill */
		while (nwin < depth && !no_more) {
			uint64_t chunk;

			if (!have_run || run_pos >= run_end) {
				uint64_t start, len;

				if (next_run(it, &start, &len) != 0) {
					no_more = true;
					break;
				}
				run_pos = start + c->fs->super->s_first_data_block;
				run_end = run_pos + len;
				have_run = true;
			}
			chunk = run_end - run_pos;
			if (chunk > c->chunk_blocks)
				chunk = c->chunk_blocks;

			win[(head + nwin) % ZIO_MAX_SLOTS] =
				(struct zf_work){ run_pos, (unsigned int)chunk };
			nwin++;
			zio_prime(&c->zio, run_pos, (unsigned int)chunk);
			run_pos += chunk;
		}

		if (nwin == 0)
			break;

		{
			const unsigned char *buf = zio_read(&c->zio);
			uint64_t blk = win[head].blk;
			unsigned int chunk = win[head].count;
			size_t nb, i;

			if (!buf)
				return 1;

			/* find and flush maximal dirty sub-runs */
			nb = 0;
			while (nb < chunk) {
				if (zf_block_is_filled(buf + nb * c->bs,
						       c->bs, c->fill)) {
					nb++;
					continue;
				}

				i = nb;
				while (i < chunk &&
				       !zf_block_is_filled(buf + i * c->bs,
							   c->bs, c->fill))
					i++;

				if (!c->dryrun &&
				    flush_dirty(c, blk + nb, (blk64_t)(i - nb)))
					return 1;
				c->modified += (blk64_t)(i - nb);
				nb = i;
			}

			for (i = 0; i < chunk; i++) {
				c->free_blk++;
				progress(c);
			}

			head = (head + 1) % ZIO_MAX_SLOTS;
			nwin--;
		}
	}

	if (zio_drain(&c->zio))
		return 1;
	return 0;
}

/*
 * The main pass: process maximal runs of consecutive free blocks.
 *
 *   discard mode:  one io_channel_discard() per whole run (no reads);
 *                  every block in the run counts as modified.
 *   fill mode:     read each run in <= chunk_blocks pieces (pipelined,
 *                  up to ZIO_MAX_SLOTS ahead), find the dirty blocks
 *                  inside, and write each maximal dirty sub-run with one
 *                  write (or one kernel zero-offload call when fill == 0).
 *
 * Only free blocks whose content differs from the fill value are
 * written, so sparse image files stay sparse and a second run does
 * no work.  In dry-run nothing is written or discarded, but the work
 * that would have been done is still counted.
 */
static int zf_pass(struct zf_ctx *c)
{
	struct run_it it;
	int rc;

	/* fmap bit i is block s_first_data_block + i (see main), so the
	 * iterator yields offsets that the mode functions rebase. */
	run_it_init(&it, c->fmap, 0,
		    ext2fs_blocks_count(c->fs->super) -
		    c->fs->super->s_first_data_block);

	if (c->verbose)
		fprintf(stderr, "\r%4.1f%%", 0.0);

	rc = c->discard ? zf_pass_discard(c, &it) : zf_pass_fill(c, &it);
	if (rc)
		return 1;

	if (c->verbose)
		printf("\r%llu/%llu/%llu\n",
		       (unsigned long long)c->modified,
		       (unsigned long long)c->free_blk,
		       (unsigned long long)c->total);
	return 0;
}

/*
 * Open a side file descriptor for kernel zero-offload, when applicable.
 * Coherency: offloaded ranges are free data blocks only; libext2fs's
 * I/O cache holds metadata (superblock, bitmaps, group descriptors),
 * the scan is forward-only, and ext2fs_close() writes back only dirty
 * metadata -- so offloaded ranges are never cached, revisited, or
 * clobbered.  Any failure simply disables offload; behavior is
 * unchanged.
 */
static void offload_init(struct zf_ctx *c, const char *path)
{
	struct stat st;
	int fd;

	if (c->fill != 0 || c->dryrun || c->discard)
		return;

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	if (fstat(fd, &st) != 0) {
		close(fd);
		return;
	}
	if (S_ISREG(st.st_mode))
		c->method = ZERO_RANGE;
	else if (S_ISBLK(st.st_mode))
		c->method = ZERO_BLKZEROOUT;
	else {
		close(fd);
		return;
	}
	c->zero_fd = fd;
}

/* Error path: close everything (the old code leaked the open
 * filesystem on I/O errors) and exit.  Metadata is untouched by this
 * tool, so closing cannot clobber anything; ignore its return value. */
static void __attribute__((noreturn))
finish(struct zf_ctx *c, int rc)
{
	ext2fs_close(c->fs);
	if (c->zero_fd >= 0)
		close(c->zero_fd);
	zio_close(&c->zio);
	free(c->fmap);
	exit(rc);
}

int main(int argc, char **argv)
{
	errcode_t ret;
	int flags;
	int c_opt;
	unsigned int fillval = 0;
	bool verbose = false;
	bool dryrun = false;
	bool discard = false;
	const char *fs_path;
	struct zf_ctx c;

	while ((c_opt = getopt(argc, argv, "nvdf:")) != -1) {
		switch (c_opt) {
		case 'n':
			dryrun = true;
			break;
		case 'v':
			verbose = true;
			break;
		case 'd':
			discard = true;
			break;
		case 'f':
			{
				char *endptr;
				long v = strtol(optarg, &endptr, 0);
				/* base 0: "010" is octal 8, as before */
				if (!*optarg || *endptr || v < 0 ||
				    (unsigned int)v > 0xFFu) {
					fprintf(stderr,
						"%s: fill value must be 0-255\n",
						argv[0]);
					return 1;
				}
				fillval = (unsigned int)v;
				printf("fillval = %u\n", fillval);
			}
			break;
		default:
			fprintf(stderr, USAGE, argv[0]);
			return 1;
		}
	}

	if (argc != optind + 1) {
		fprintf(stderr, USAGE, argv[0]);
		return 1;
	}
	fs_path = argv[optind];

	ret = ext2fs_check_if_mounted(fs_path, &flags);
	if (ret) {
		fprintf(stderr,
			"%s: failed to determine filesystem mount state  %s\n",
			argv[0], fs_path);
		return 1;
	}
	if ((flags & EXT2_MF_MOUNTED) && !(flags & EXT2_MF_READONLY)) {
		fprintf(stderr, "%s: filesystem %s is mounted rw\n",
			argv[0], fs_path);
		return 1;
	}

	ret = ext2fs_open(fs_path, OPEN_FLAGS, 0, 0, unix_io_manager, &c.fs);
	if (ret) {
		fprintf(stderr, "%s: failed to open filesystem %s\n",
			argv[0], fs_path);
		return 1;
	}

	c.bs = c.fs->blocksize;
	c.chunk_blocks = (unsigned int)(ZIO_CHUNK_BYTES / c.bs);
	if (c.chunk_blocks < 1)
		c.chunk_blocks = 1;
	c.fill = (uint8_t)fillval;
	c.dryrun = dryrun;
	c.discard = discard;
	c.verbose = verbose;
	c.free_blk = 0;
	c.free_total = ext2fs_free_blocks_count(c.fs->super);
	c.modified = 0;
	c.total = ext2fs_blocks_count(c.fs->super);
	c.old_percent = -1;
	c.zero_fd = -1;
	c.method = ZERO_PWRITE;
	c.zio.fd = -1;
	c.fmap = NULL;

	offload_init(&c, fs_path);

	ret = ext2fs_read_block_bitmap(c.fs);
	if (ret) {
		fprintf(stderr, "%s: error while reading block bitmap\n",
			argv[0]);
		finish(&c, 1);
	}

	/* The bitmap type is opaque in the public header, so take a copy of
	 * its bits for [s_first_data_block, blocks_count) for the word scan. */
	{
		blk64_t first = c.fs->super->s_first_data_block;
		blk64_t nbits = c.total > first ? c.total - first : 0;

		c.fmap = calloc((size_t)((nbits + 7) / 8) + 8, 1);
		if (c.fmap == NULL) {
			fprintf(stderr, "%s: out of memory (surely not?)\n",
				argv[0]);
			finish(&c, 1);
		}
		if (nbits &&
		    ext2fs_get_block_bitmap_range2(c.fs->block_map, first,
						   nbits, c.fmap)) {
			fprintf(stderr, "%s: error while reading block bitmap\n",
				argv[0]);
			finish(&c, 1);
		}
	}

	if (!discard) {
		int rc = zio_open(&c.zio, fs_path, c.bs, c.fill, c.dryrun);

		if (rc == -1) {
			fprintf(stderr, "%s: failed to open filesystem %s\n",
				argv[0], fs_path);
			finish(&c, 1);
		} else if (rc == -2) {
			fprintf(stderr, "%s: out of memory (surely not?)\n",
				argv[0]);
			finish(&c, 1);
		}
	}

	if (zf_pass(&c))
		finish(&c, 1);

	ret = ext2fs_close(c.fs);
	if (c.zero_fd >= 0)
		close(c.zero_fd);
	zio_close(&c.zio);
	free(c.fmap);
	if (ret) {
		fprintf(stderr, "%s: error while closing filesystem\n",
			argv[0]);
		return 1;
	}
	return 0;
}
