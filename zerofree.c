/*
 * zerofree - a tool to zero free blocks in an ext2 filesystem
 *
 * Copyright (C) 2004-2012 R M Yorston
 *
 * This file may be redistributed under the terms of the GNU General Public
 * License, version 2.
 */

#define _POSIX_C_SOURCE 200112L // Enables POSIX.1-2001 features including posix_memalign

#include <sys/types.h>
#include <ext2fs/ext2fs.h>
#include <getopt.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <immintrin.h>

#define USAGE "usage: %s [-n] [-v] [-d] [-f fillval] filesystem\n"

/*
 * 512 blocks is 2MB for a standard 4KB filesystem block size.
 * This is the sweet spot for maximum I/O performance on modern Linux systems.
 */
#define BATCH_BLOCKS 512

#if defined(__GNUC__) || defined(__clang__)
#define ASSUME_ALIGNED(ptr, align) __builtin_assume_aligned((ptr), (align))
#define force_inline __attribute__((always_inline)) inline
#else
#define ASSUME_ALIGNED(ptr, align) (ptr)
#define force_inline inline
#endif

void bailout(void* mem0, void* mem1) __attribute__ ((noreturn));

static force_inline int is_block_zero(const unsigned char * restrict buf, size_t len) {
	size_t i = 0;
	const unsigned char *p = buf;

#if defined(__AVX512F__) && defined(__AVX512BW__)
	while (i + 64 <= len) {
		__m512i v_data = _mm512_loadu_si512((const __m512i *)(p + i));
		if (!_mm512_testz_epi32(v_data, v_data)) return 0;
		i += 64;
	}
#elif defined(__AVX2__)
	while (i + 32 <= len) {
		__m256i v_data = _mm256_loadu_si256((const __m256i *)(p + i));
		if (!_mm256_testz_si256(v_data, v_data)) return 0;
		i += 32;
	}
#endif

	while (i < len) {
		if (p[i] != 0) return 0;
		i++;
	}
	return 1;
}

static force_inline int is_block_filled(const unsigned char * restrict buf, size_t len, unsigned char fillval) {
	size_t i = 0;
	const unsigned char *p = buf;

#if defined(__AVX512F__) && defined(__AVX512BW__)
	__m512i v_fill = _mm512_set1_epi8(fillval);
	while (i + 64 <= len) {
		__m512i v_data = _mm512_loadu_si512((const __m512i *)(p + i));
		__mmask64 mask = _mm512_cmpeq_epi8_mask(v_data, v_fill);
		if (mask != 0xFFFFFFFFFFFFFFFFULL) return 0;
		i += 64;
	}
#elif defined(__AVX2__)
	__m256i v_fill = _mm256_set1_epi8(fillval);
	while (i + 32 <= len) {
		__m256i v_data = _mm256_loadu_si256((const __m256i *)(p + i));
		__m256i v_cmp = _mm256_cmpeq_epi8(v_data, v_fill);
		if (_mm256_movemask_epi8(v_cmp) != 0xFFFFFFFF) return 0;
		i += 32;
	}
#endif

	/* Scalar fallback for remaining bytes */
	while (i < len) {
		if (p[i] != fillval) return 0;
		i++;
	}

	return 1;
}


int main(int argc, char **argv)
{
	errcode_t ret;
	int flags;
	int superblock = 0;
	int open_flags = EXT2_FLAG_RW;
	int blocksize = 0;
	ext2_filsys fs = NULL;
	unsigned char *buf = NULL;
	unsigned char *empty = NULL;
	int c;
	unsigned int fillval = 0;
	int verbose = 0;
	int dryrun = 0;
	int discard = 0;
	unsigned long free_blk = 0;
	unsigned long modified = 0;
	double percent = 0.0;
	int old_percent = -1;

	while ( (c=getopt(argc, argv, "nvdf:")) != -1 ) {
		switch (c) {
		case 'n' :
			dryrun = 1;
			break;
		case 'v' :
			verbose = 1;
			break;
		case 'd':
			discard = 1;
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
				printf("fillval = %u\n", fillval);
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
		fprintf(stderr, "%s: failed to determine filesystem mount state	%s\n",
			argv[0], argv[optind]);
		return 1;
	}

	if ( (flags & EXT2_MF_MOUNTED) && !(flags & EXT2_MF_READONLY) ) {
		fprintf(stderr, "%s: filesystem %s is mounted rw\n",
			argv[0], argv[optind]);
		return 1;
	}

	/*
	 * Try opening with Linux Direct I/O enabled first to bypass Page Cache.
	 * Bypasses physical memory buffer thrashing.
	 */
	ret = ext2fs_open(argv[optind], open_flags | EXT2_FLAG_DIRECT_IO, superblock, blocksize,
							unix_io_manager, &fs);
	if ( ret ) {
		/* Fallback to buffered standard I/O if Direct I/O is unsupported (e.g. on legacy virtual target drives) */
		ret = ext2fs_open(argv[optind], open_flags, superblock, blocksize,
								unix_io_manager, &fs);
		if ( ret ) {
			fprintf(stderr, "%s: failed to open filesystem %s\n",
				argv[0], argv[optind]);
			return 1;
		}
	} else {
		if (verbose) {
			printf("Opened filesystem with Linux Direct I/O enabled (Bypassing Page Cache).\n");
		}
	}

	const size_t batch_bytes = (size_t)BATCH_BLOCKS * fs->blocksize;
	const size_t alignment = 4096; // 4KB Boundary aligns physical NVMe sectors and CPU vector registers

	if (posix_memalign((void **)&empty, alignment, batch_bytes) != 0 ||
			posix_memalign((void **)&buf, alignment, batch_bytes) != 0) {
		fprintf(stderr, "%s: out of memory\n", argv[0]);
		bailout((void*) empty, (void*) buf);
	}

	memset(empty, fillval, batch_bytes);

	ret = ext2fs_read_block_bitmap(fs);
	if ( ret ) {
		fprintf(stderr, "%s: error while reading block bitmap\n", argv[0]);
		bailout((void*) empty, (void*) buf);
	}

	if ( verbose ) {
		fprintf(stderr, "\r%4.1f%%", percent);
	}

	const unsigned long max_blocks = fs->super->s_blocks_count;
	unsigned long blk = fs->super->s_first_data_block;

	double total_free_blocks = (double)fs->super->s_free_blocks_count;
	if (total_free_blocks < 1.0) {
		total_free_blocks = 1.0;
	}

	while (blk < max_blocks) {
		// 1. Skip blocks that are in use (not free)
		if (ext2fs_test_block_bitmap(fs->block_map, blk)) {
			blk++;
			continue;
		}

		// 2. Identify a contiguous run of free blocks
		unsigned long run_start = blk;
		unsigned long run_len = 0;
		while (blk < max_blocks &&
					 !ext2fs_test_block_bitmap(fs->block_map, blk) &&
					 run_len < BATCH_BLOCKS) {
			run_len++;
			blk++;
		}

		// 3. Process the contiguous run of blocks
		if (!discard) {
			// Read the entire contiguous run in one unified Direct I/O call
			ret = io_channel_read_blk(fs->io, run_start, run_len, buf);
			if ( ret ) {
				fprintf(stderr, "error while reading blocks starting at %lu\n", run_start);
				bailout((void*) empty, (void*) buf);
			}

			unsigned char * restrict r_buf = buf;
			unsigned char * restrict r_empty = empty;
			const size_t b_size = fs->blocksize;

			unsigned long write_run_start = 0;
			unsigned long write_run_len = 0;

			for (unsigned long i = 0; i < run_len; i++) {
				unsigned char *block_ptr = r_buf + (i * b_size);
				int is_different = (fillval == 0) ? !is_block_zero(block_ptr, b_size) : !is_block_filled(block_ptr, b_size, fillval);

				if (is_different) {
					modified++;
					if (!dryrun) {
						if (write_run_len == 0) {
							write_run_start = run_start + i;
						}
						write_run_len++;
					}
				} else {
					// We hit a matching block. Flush any pending write runs.
					if (write_run_len > 0) {
						ret = io_channel_write_blk(fs->io, write_run_start, write_run_len, r_empty);
						if ( ret ) {
							fprintf(stderr, "error while writing blocks starting at %lu\n", write_run_start);
							bailout((void*) empty, (void*) buf);
						}
						write_run_len = 0;
					}
				}
			}

			// Write any remaining trailing dirty blocks
			if (write_run_len > 0) {
				ret = io_channel_write_blk(fs->io, write_run_start, write_run_len, r_empty);
				if ( ret ) {
					fprintf(stderr, "error while writing blocks starting at %lu\n", write_run_start);
					bailout((void*) empty, (void*) buf);
				}
			}
		} else { /* discard/TRIM mode */
			modified += run_len;
			if (!dryrun) {
				// Discard the whole sequence at once (translates to a single BLKDISCARD ioctl on Linux)
				ret = io_channel_discard(fs->io, run_start, run_len);
				if ( ret ) {
					fprintf(stderr, "error while discarding blocks starting at %lu\n", run_start);
					bailout((void*) empty, (void*) buf);
				}
			}
		}

		// 4. Update the CLI progress stats (massively reduced frequency due to chunk processing)
		free_blk += run_len;
		if ( verbose ) {
			percent = 100.0 * (double)free_blk / total_free_blocks;
			if ( (int)(percent * 10) != old_percent ) {
				fprintf(stderr, "\r%4.1f%%", percent);
				old_percent = (int)(percent * 10);
			}
		}
	}

	if ( verbose ) {
		printf("\r%lu/%lu/%u\n", modified, free_blk, fs->super->s_blocks_count);
	}

	ret = ext2fs_close(fs);
	if ( ret ) {
		fprintf(stderr, "%s: error while closing filesystem\n", argv[0]);
		bailout((void*) empty, (void*) buf);
	}

	free(buf);
	free(empty);
	return 0;
}

void bailout(void* mem0, void* mem1)
{
	if (mem0) {
		free(mem0);
	}

	if (mem1) {
		free(mem1);
	}

	exit(1);
}
