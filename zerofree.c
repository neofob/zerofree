/*
 * zerofree - a tool to zero free blocks in an ext2 filesystem
 *
 * Copyright (C) 2004-2012 R M Yorston
 *
 * This file may be redistributed under the terms of the GNU General Public
 * License, version 2.
 *
 * Changes:
 * 2014-11-23  Add thread option Tuan T. Pham
 * 2014-11-22  Fix memory leak. Tuan T. Pham
 * 2010-10-17  Allow non-zero fill value.   Patch from Jacob Nevins.
 * 2007-08-12  Allow use on filesystems mounted read-only.   Patch from
 *             Jan Krämer.
 */

#include <ext2fs/ext2fs.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#define USAGE "usage: %s [-t count] [-n] [-v] [-d] [-f fillval] filesystem\n"

pthread_barrier_t g_thread_barrier;

void zero_func(ext2_filsys fs, unsigned long blk, unsigned char* buf,
		unsigned char* empty, unsigned int fillval, int dryrun,
		int discard, int* error);

struct thread_arg {
	ext2_filsys	fs;
	unsigned long	start_blk;
	unsigned long 	end_blk;
	unsigned int	fillval;
	int		dryrun;
	int		discard;
	unsigned char	*empty;		/* shared by all threads */
};

void single_thread(ext2_filsys fs, unsigned int fillval, int dryrun,
		int verbose, int discard, unsigned char *empty,
		unsigned char *buf);

void* zero_thread(void* arg);
void multi_thread(ext2_filsys fs, long thread_count, unsigned int fillval,
		int dryrun, int discard, unsigned char *empty,
		unsigned char *buf);

void bailout(void* mem0, void* mem1) __attribute__ ((noreturn));

int main(int argc, char **argv)
{
	errcode_t ret;
	int flags;
	int superblock = 0;
	int open_flags = EXT2_FLAG_RW;
	int blocksize = 0;
	ext2_filsys fs = NULL;
	unsigned long blk;
	unsigned char *buf;
	unsigned char *empty;
	int i, c;
	unsigned int fillval = 0;
	int verbose = 0;
	int dryrun = 0;
	int discard = 0;
	long thread_count = 1;

	while ( (c=getopt(argc, argv, "t:nvdf:")) != -1 ) {
		switch (c) {
		case 't':
			{
				char *endptr;
				thread_count = strtol(optarg, &endptr, 0);
				if (!*optarg || *endptr || thread_count < 0) {
					fprintf(stderr, "%s: invalid argument"
						" to -t\n", argv[0]);
					return 1;
				}
				fprintf(stderr, "USE %d threads\n", thread_count);
				fprintf(stderr, "WARNING: Running multiple threads"
					" might damage your spinning device!\n");
			}
			break;
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

	ret = ext2fs_open(argv[optind], open_flags, superblock, blocksize,
							unix_io_manager, &fs);
	if ( ret ) {
		fprintf(stderr, "%s: failed to open filesystem %s\n",
			argv[0], argv[optind]);
		return 1;
	}

	empty = (unsigned char *)malloc(fs->blocksize);
	buf = (unsigned char *)malloc(fs->blocksize);

	if ( empty == NULL || buf == NULL ) {
		fprintf(stderr, "%s: out of memory (surely not?)\n", argv[0]);
		bailout((void*) empty, (void*) buf);
	}

	memset(empty, fillval, fs->blocksize);

	ret = ext2fs_read_inode_bitmap(fs);
	if ( ret ) {
		fprintf(stderr, "%s: error while reading inode bitmap\n", argv[0]);
		bailout((void*) empty, (void*) buf);
	}

	ret = ext2fs_read_block_bitmap(fs);
	if ( ret ) {
		fprintf(stderr, "%s: error while reading block bitmap\n", argv[0]);
		bailout((void*) empty, (void*) buf);
	}

	if (thread_count == 1) {
		single_thread(fs, fillval, dryrun, verbose, discard, empty, buf);
	}
	else {
		multi_thread(fs, thread_count, fillval, dryrun, discard, empty, buf);
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

void multi_thread(ext2_filsys fs, long thread_count, unsigned int fillval,
		int dryrun, int discard, unsigned char *empty,
		unsigned char *buf)
{
	int 			i, ret;
	pthread_t		*tid_array;
	struct thread_arg	*arg_array;
	unsigned long		blk, part_size, pivot;
	int			error = 0;

	tid_array = malloc(sizeof(pthread_t)*thread_count);
	arg_array = malloc(sizeof(struct thread_arg)*thread_count);

	pthread_barrier_init(&g_thread_barrier, NULL, thread_count+1);

	pivot = fs->super->s_first_data_block;
	part_size = (fs->super->s_blocks_count - fs->super->s_first_data_block)
			/thread_count;

	for (i=0; i < thread_count; i++) {
		arg_array[i].fs = fs;
		arg_array[i].start_blk = pivot;
		arg_array[i].end_blk = part_size*(i+1);
		arg_array[i].dryrun = dryrun;
		arg_array[i].discard = discard;
		arg_array[i].empty = empty;

		pivot += part_size;

		ret = pthread_create(&tid_array[i], NULL, zero_thread,
					&arg_array[i]);
	}

	/* process the remaining blocks */
	if (pivot < fs->super->s_blocks_count) {
		for (blk = pivot; blk < fs->super->s_blocks_count; blk++)
		{
			zero_func(fs, blk, buf, empty, fillval, dryrun,
				discard, &error);
		}
	}

	pthread_barrier_wait(&g_thread_barrier);
	for (i=0; i < thread_count; i++) {
		pthread_join(tid_array[i], NULL);
	}

	free(tid_array);
	free(arg_array);
}

inline void zero_func(ext2_filsys fs, unsigned long blk, unsigned char* buf,
		unsigned char* empty, unsigned int fillval, int dryrun,
		int discard, int *error)
{
	int ret, i;
	if ( ext2fs_test_block_bitmap(fs->block_map, blk) ) {
		return;
	}

	if (!discard) {
		ret = io_channel_read_blk(fs->io, blk, 1, buf);
		if ( ret ) {
			fprintf(stderr, "error while reading block\n");
			*error = 1;
			return;
		}

		for ( i=0; i < fs->blocksize; ++i ) {
			if ( buf[i] != fillval ) {
				break;
			}
		}

		if ( i == fs->blocksize ) {
			return;
		}
	}

	if ( !dryrun ) {
		if (!discard) {
			ret = io_channel_write_blk(fs->io, blk, 1, empty);
			if ( ret ) {
				fprintf(stderr, "error while writing block\n");
				*error = 1;
				return;
			}
		} else { /* discard */
			ret = io_channel_discard(fs->io, blk, 1);
			if ( ret ) {
				fprintf(stderr, "error while discarding block\n");
				*error = 1;
				return;
			}
		}
	}
}

void* zero_thread(void* arg)
{
	struct thread_arg m_arg = *(struct thread_arg*) arg;
	unsigned char *buf;
	unsigned long blk;
	int	error = 0;

	buf = (unsigned char*) malloc(m_arg.fs->blocksize);

	for (blk = m_arg.start_blk; blk < m_arg.end_blk; blk++) {
		zero_func(m_arg.fs, blk, buf, m_arg.empty, m_arg.fillval,
				m_arg.dryrun, m_arg.discard, &error);
		if (error) {
			break;
		}
	}

	free(buf);
	pthread_barrier_wait(&g_thread_barrier);
	return (void*) ((unsigned long) error);
}

void single_thread(ext2_filsys fs, unsigned int fillval, int dryrun,
		int verbose, int discard, unsigned char *empty,
		unsigned char *buf)
{
	unsigned long	blk, free_blk, modified;
	double		percent;
	int		old_percent, ret, i;

	free_blk = modified = 0;
	percent = 0.0;
	old_percent = -1;

	if ( verbose ) {
		fprintf(stderr, "\r%4.1f%%", percent);
	}

	for ( blk=fs->super->s_first_data_block;
		blk < fs->super->s_blocks_count; blk++ ) {

		if ( ext2fs_test_block_bitmap(fs->block_map, blk) ) {
			continue;
		}

		++free_blk;
		percent = 100.0 * (double)free_blk/
					(double)fs->super->s_free_blocks_count;

		if ( verbose && (int)(percent*10) != old_percent ) {
			fprintf(stderr, "\r%4.1f%%", percent);
			old_percent = (int)(percent*10);
		}

		if (!discard) {
			ret = io_channel_read_blk(fs->io, blk, 1, buf);
			if ( ret ) {
				fprintf(stderr, "error while reading block\n");
				bailout((void*) empty, (void*) buf);
			}

			for ( i=0; i < fs->blocksize; ++i ) {
				if ( buf[i] != fillval ) {
					break;
				}
			}

			if ( i == fs->blocksize ) {
				continue;
			}
		}

		++modified;

		if ( !dryrun ) {
			if (!discard) {
				ret = io_channel_write_blk(fs->io, blk, 1, empty);
				if ( ret ) {
					fprintf(stderr, "error while writing block\n");
					bailout((void*) empty, (void*) buf);
				}
			} else { /* discard */
				ret = io_channel_discard(fs->io, blk, 1);
				if ( ret ) {
					fprintf(stderr, " error while discarding block\n");
					bailout((void*) empty, (void*) buf);
				}
			}
		}

		if ( verbose ) {
			printf("\r%u/%u/%u\n", modified, free_blk,
				fs->super->s_blocks_count);
		}
	}
}
