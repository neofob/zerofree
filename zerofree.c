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

#include "zerofree.h"

#ifndef EXT2_FLAG_64BITS
#error "zerofree requires e2fsprogs 1.42 or newer"
#endif

#define OPEN_FLAGS (EXT2_FLAG_RW | EXT2_FLAG_64BITS)
#define USAGE "usage: %s [-n] [-v] [-d] [-f fillval] filesystem\n"

/* I/O is batched in chunks of at most this many bytes */
#define CHUNK_BYTES ((size_t)1024 * 1024)

enum zero_method {
	ZERO_PWRITE,		/* write the fill buffer (always works) */
	ZERO_RANGE,		/* fallocate(FALLOC_FL_ZERO_RANGE), files */
	ZERO_BLKZEROOUT,	/* BLKZEROOUT ioctl, block devices */
};

struct zf_ctx {
	ext2_filsys	fs;
	unsigned char	*restrict buf;	/* read buffer */
	unsigned char	*restrict fillbuf;	/* pre-filled with fillval */
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

/* Zero one dirty sub-run; writes the fill buffer when offload is off. */
static int flush_dirty(struct zf_ctx *c, blk64_t blk, blk64_t count)
{
	errcode_t ret;

	if (c->method != ZERO_PWRITE) {
		int rc = zero_range(c, blk, count);
		if (rc == 1) {	/* unsupported: fall back permanently */
			c->method = ZERO_PWRITE;
			if (c->zero_fd >= 0)
				close(c->zero_fd);
			c->zero_fd = -1;
		} else if (rc == 2)
			return 1;
		else
			return 0;
	}

	ret = io_channel_write_blk64(c->fs->io, blk, (int)count, c->fillbuf);
	return ret != 0;
}

static int read_piece(struct zf_ctx *c, blk64_t blk, unsigned int count)
{
	errcode_t ret = io_channel_read_blk64(c->fs->io, blk, count, c->buf);
	if (ret) {
		fprintf(stderr, "error while reading block\n");
		return 1;
	}
	return 0;
}

/*
 * The main pass: scan the block bitmap and process maximal runs of
 * consecutive free blocks.
 *
 *   discard mode:  one io_channel_discard() per whole run (no reads);
 *                  every block in the run counts as modified.
 *   fill mode:     read each run in <= chunk_blocks pieces (one syscall
 *                  per piece), find the dirty blocks inside, and write
 *                  each maximal dirty sub-run with one syscall (or one
 *                  kernel zero-offload call when fill == 0).
 *
 * Only free blocks whose content differs from the fill value are
 * written, so sparse image files stay sparse and a second run does
 * no work.  In dry-run nothing is written or discarded, but the work
 * that would have been done is still counted.
 */
static int zf_pass(struct zf_ctx *c)
{
	blk64_t blk = c->fs->super->s_first_data_block;
	blk64_t blocks_count = ext2fs_blocks_count(c->fs->super);

	if (c->verbose)
		fprintf(stderr, "\r%4.1f%%", 0.0);

	while (blk < blocks_count) {
		blk64_t run_start, run_len, pos, remaining;
		unsigned int chunk;
		size_t nb, i;

		if (ext2fs_test_block_bitmap2(c->fs->block_map, blk)) {
			blk++;		/* in use */
			continue;
		}

		/* maximal run of consecutive free blocks */
		run_start = blk;
		run_len = 0;
		while (blk < blocks_count &&
		       !ext2fs_test_block_bitmap2(c->fs->block_map, blk)) {
			blk++;
			run_len++;
		}

		if (c->discard) {
			errcode_t ret = 0;

			if (!c->dryrun)
				ret = io_channel_discard(c->fs->io,
							 run_start, run_len);
			for (i = 0; i < (size_t)run_len; i++) {
				c->free_blk++;
				c->modified++;
				progress(c);
			}
			if (ret) {
				fprintf(stderr, "error while discarding block\n");
				return 1;
			}
			continue;
		}

		/* fill mode: read the run in chunks, flush the dirty
		 * sub-runs */
		pos = run_start;
		remaining = run_len;
		while (remaining > 0) {
			chunk = c->chunk_blocks;
			if ((blk64_t)chunk > remaining)
				chunk = (unsigned int)remaining;

			if (read_piece(c, pos, chunk))
				return 1;

			/* find and flush maximal dirty sub-runs */
			nb = 0;
			while (nb < chunk) {
				if (zf_block_is_filled(c->buf + nb * c->bs,
						       c->bs, c->fill)) {
					nb++;
					continue;
				}

				i = nb;
				while (i < chunk &&
				       !zf_block_is_filled(c->buf + i * c->bs,
							   c->bs, c->fill))
					i++;

				if (!c->dryrun &&
				    flush_dirty(c, pos + nb, (blk64_t)(i - nb))) {
					fprintf(stderr,
						"error while writing block\n");
					return 1;
				}
				c->modified += (blk64_t)(i - nb);
				nb = i;
			}

			for (i = 0; i < chunk; i++) {
				c->free_blk++;
				progress(c);
			}

			pos += chunk;
			remaining -= chunk;
		}
	}

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
finish(const struct zf_ctx *c, int rc)
{
	ext2fs_close(c->fs);
	if (c->zero_fd >= 0)
		close(c->zero_fd);
	free(c->buf);
	free(c->fillbuf);
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

	c.buf = aligned_alloc(4096, CHUNK_BYTES);
	c.fillbuf = aligned_alloc(4096, CHUNK_BYTES);
	if (c.buf == NULL || c.fillbuf == NULL) {
		fprintf(stderr, "%s: out of memory (surely not?)\n", argv[0]);
		free(c.buf);
		free(c.fillbuf);
		ext2fs_close(c.fs);
		return 1;
	}

	c.bs = c.fs->blocksize;
	c.chunk_blocks = (unsigned int)(CHUNK_BYTES / c.bs);
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
	memset(c.fillbuf, fillval, CHUNK_BYTES);

	offload_init(&c, fs_path);

	ret = ext2fs_read_block_bitmap(c.fs);
	if (ret) {
		fprintf(stderr, "%s: error while reading block bitmap\n",
			argv[0]);
		finish(&c, 1);
	}

	if (zf_pass(&c))
		finish(&c, 1);

	ret = ext2fs_close(c.fs);
	if (c.zero_fd >= 0)
		close(c.zero_fd);
	free(c.buf);
	free(c.fillbuf);
	if (ret) {
		fprintf(stderr, "%s: error while closing filesystem\n",
			argv[0]);
		return 1;
	}
	return 0;
}
