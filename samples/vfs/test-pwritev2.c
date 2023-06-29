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
#include <fcntl.h>

#define statx foo
#define statx_timestamp foo_timestamp
struct statx;
struct statx_timestamp;
#include <sys/stat.h>
#undef statx
#undef statx_timestamp

#include <sys/uio.h>

#ifndef __NR_statx
#define __NR_statx -1
#endif

#define DEFAULT_WRITE_SIZE 1024
#define FILE_TO_OPEN "/root/mnt/file_50M"
#define RWF_ATOMIC      (0x00000020)

int main(int argc, char **argv)
{
	char *buffer;
	struct iovec iov[1];
	ssize_t written;
	int fd;
	int flags = RWF_SYNC | RWF_ATOMIC;
	off_t offset = 0;
	int opt = 0;
	int write_size = DEFAULT_WRITE_SIZE;

	while ((opt = getopt(argc, argv, "l:p:")) != -1) {
		switch (opt) {
			case 'l':
				write_size = atoi(optarg);
				break;
			case 'p':
				offset = atoi(optarg);
				break;
		}
	}
	printf("write_size=%d offset=%d\n", write_size, offset);
	fd = open(FILE_TO_OPEN, O_RDWR | O_DIRECT, 777);
	if (fd < 0) {
		printf("could not open %s\n", FILE_TO_OPEN);
		return -1;	
	}

	buffer = malloc(write_size);
	if (!buffer) {
		printf("could not allocate buffer of size %d\n", write_size);
		close(fd);
		return -1;
	}
	iov[0].iov_base = buffer;
	iov[0].iov_len = write_size;

	written = pwritev2(fd, iov, 1, offset, flags);
	printf("%s wrote %zd bytes at offset %zd\n", __func__, written, offset);
	close(fd);
	free(buffer);
	
	return 0;
}
