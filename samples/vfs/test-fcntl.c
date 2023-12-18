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
#define RWF_ATOMIC      (0x00000020)

#define F_SET_ATOMIC_WRITE_SIZE  (F_LINUX_SPECIFIC_BASE + 15)
int main(int argc, char **argv)
{
	struct iovec *iov;
	void **buffers;
	ssize_t written;
	int fd;
	int wr_flags = RWF_SYNC;
	int o_flags = O_RDWR;
	off_t offset = 0;
	int opt = 0;
	int write_size = DEFAULT_WRITE_SIZE;
	char *file = NULL;
	char **argv_orig = argv;
	int argc_i;
	int middle_start_end_align_4096 = 0;
	int large_vectors = 0;
	int i, remain;
	int avg_size;
	int multi_vectors = 0;
	int max_vectors;
	int vectors;
	int byte_index;
	int multi_vector_alloc_size;
	int result;

	argv_orig++;
	for (argc_i = 0; argc_i < argc - 1; argc_i++, argv_orig++) {
		file = *argv_orig;
	}

#if 0
	while ((opt = getopt(argc, argv, "l:p:Padm")) != -1) {
		switch (opt) {
			case 'l':
				write_size = atoi(optarg);
				break;
			case 'p':
				offset = atoi(optarg);
				break;
			case 'm':
				multi_vectors = 1;
				break;
			case 'a':
				wr_flags |= RWF_ATOMIC;
				break;
			case 'd':
				o_flags |= O_DIRECT;
				break;
			case 'P':
				middle_start_end_align_4096 = 1;
				break;
			case 'L':
				large_vectors = 1;
				break;
		}
	}
#endif
	printf("file=%s write_size=%d offset=%d o_flags=0x%x wr_flags=0x%x multi_vector_alloc_size=%d\n", file, write_size, offset, o_flags, wr_flags, multi_vector_alloc_size);
	fd = open(file, o_flags, 777);
	if (fd < 0) {
		printf("could not open %s\n", file);
		return -1;	
	}


	result = fcntl(fd, F_SET_ATOMIC_WRITE_SIZE, 123456);

	return result;
}
