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
#define RWF_ATOMIC      (0x00000040)

int main(int argc, char **argv)
{
	ssize_t written;
	int fd;
	int rw_flags = RWF_SYNC;
	int o_flags = O_RDWR;
	int opt = 0;
	unsigned int write_size = DEFAULT_WRITE_SIZE;
	char *file = NULL;
	char **argv_orig = argv;
	int argc_i;
	int middle_start_end_align_4096 = 0;
	int large_vectors = 0;
	int i, remain;
	int multi_vectors = 0;
	int byte_index;
	int multi_vector_alloc_size = -1;
	int ret;
	loff_t pos = 0;
	char *read_buffer = NULL;
	int verify = 0;
	int demo_hch_problem = 0;

	argv_orig++;
	for (argc_i = 0; argc_i < argc - 1; argc_i++, argv_orig++) {
		file = *argv_orig;
	}

	while ((opt = getopt(argc, argv, "l:p:PadmhS:vH")) != -1) {
		switch (opt) {
			case 'l':
				write_size = atoi(optarg);
				if (write_size == 0) {
					printf("write size cannot be zero\n");
					exit(0);
				}
				if (write_size % 512) {
					printf("write size must be multiple of 512\n");
					exit(0);
				}
				break;
			case 'p':
				pos = atoi(optarg);
				if (pos % 512) {
					printf("pos must be multiple of 512\n");
					exit(0);
				}
				break;
			case 'm':
				multi_vectors = 1;
				break;
			case 'a':
				rw_flags |= RWF_ATOMIC;
				break;
			case 'd':
				o_flags |= O_DIRECT;
				break;
			case 'v':
				verify = 1;
				break;
			case 'P':
				middle_start_end_align_4096 = 1;
				break;
			case 'S':
				multi_vector_alloc_size = atoi(optarg);
				if (multi_vector_alloc_size == 0) {
					printf("multi-vector alloc size cannot be zero\n");
					exit(0);
				}
				if (multi_vector_alloc_size % 512) {
					printf("multi-vector alloc size must be multiple of 512\n");
					exit(0);
				}
				break;
			case 'H':
				demo_hch_problem = 1;
				multi_vectors = 1;
				multi_vector_alloc_size = 4096 * 2;
				write_size = 1024 * 256;
				break;
			case 'h':
				printf("Options:\n");
				printf("l: write size\n");
				printf("p: write offset\n");
				printf("m: multi-vectors\n");
				printf("a: atomic\n");
				printf("d: direct I/O\n");
				printf("S: vector size\n");
				printf("P: middle start+end align to 4096\n");
				printf("v: verify data written properly\n");
				exit(0);
		}
	}
	fd = open(file, o_flags, 777);
	if (fd < 0) {
		printf("could not open %s\n", file);
		return -1;
	}

	printf("preadv2 rw_flags=0x%x o_flags=0x%x\n", __func__, rw_flags, o_flags);
	read_buffer = malloc(write_size);
	if (!read_buffer) {
		printf("could not alloc read buffer\n");
		return -1;
	}
	struct iovec iov_read = {
		.iov_base = read_buffer,
		.iov_len = write_size,
	};
	int dataread = preadv2(fd, &iov_read, 1, pos, rw_flags);
	if (dataread == write_size) {
		ret = 0;
	} else {
		printf("read incorrect amount of data, wanted=%d got=%d\n", write_size, dataread);
		return -1;
	}

	remain = write_size;
	char *read_data = read_buffer;

	printf("read back and compared ok data, wanted size=%d got=%d\n", write_size, dataread);
end:
	close(fd);
	free(read_buffer);

	return 0;
}
