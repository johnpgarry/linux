// SPDX-License-Identifier: GPL-2.0-or-later
/* Test the statx() system call.
 *
 * Note that the output of this program is intended to look like the output of
 * /bin/stat where possible.
 *
 * Copyright (C) 2015 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <linux/stat.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include <sys/uio.h>


int main(int argc, char **argv)
{
	int fd;
	char **argv_orig = argv;
	int o_flags = O_RDWR;
	int argc_i;
	char *file;
	int opt = 0;
	int error;
	int blocksize = -1;

	argv_orig++;
	for (argc_i = 0; argc_i < argc - 1; argc_i++, argv_orig++) {
		file = *argv_orig;
	}

	printf("3 file=%s\n", file);
	fd = open(file, o_flags, 777);
	if (fd < 0) {
		printf("could not open %s\n", file);
		return -1;
	}

	while ((opt = getopt(argc, argv, "bB:")) != -1) {
		switch (opt) {
			case 'b': //BLKBSZGET
				error = ioctl(fd, BLKBSZGET, &blocksize);
				printf("BLKBSZGET error=%d blocksize=%d\n", error, blocksize);
				break;
			case 'B': //BLKBSZSET
				blocksize = atoi(optarg);
				error = ioctl(fd, BLKBSZSET, &blocksize);
				printf("BLKBSZGET error=%d blocksize=%d\n", error, blocksize);
				error = ioctl(fd, BLKBSZGET, &blocksize);
				printf("2BLKBSZGET error=%d blocksize=%d\n", error, blocksize);
				break;
			case 'h':
				printf("Options:\n");
				printf("b: BLKBSZGET\n");
				printf("B: BLKBSZSET\n");
				exit(0);
		}
	}
	close(fd);
	
	return 0;
}
