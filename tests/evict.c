/* evict: flush and drop page cache for given files (no root needed) */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		int fd = open(argv[i], O_RDONLY);
		if (fd < 0) { perror(argv[i]); return 1; }
		fdatasync(fd);
		posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
		close(fd);
	}
	return 0;
}
