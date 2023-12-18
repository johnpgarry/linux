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
	int ret;

	argv_orig++;
	for (argc_i = 0; argc_i < argc - 1; argc_i++, argv_orig++) {
		file = *argv_orig;
	}

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

	if (multi_vectors) {
		if (large_vectors) {
			multi_vector_alloc_size = 5 * 4096;
		} else {
			multi_vector_alloc_size = 4096;
		}
		max_vectors = (4 * write_size) / multi_vector_alloc_size;
		if (max_vectors > 1024) {
			max_vectors = 1024;
			multi_vector_alloc_size = (write_size * 4) / max_vectors;
		}
		if (max_vectors == 0)
			max_vectors = 1;
		avg_size = write_size / max_vectors;
	} else {
		max_vectors = 1;
		if (large_vectors) {
			printf("large vectors should not be set without -m\n");
			return -1;
		}
		if (middle_start_end_align_4096) {
			printf("PAGE-aligned vectors should not be set without -m\n");
			return -1;
		}
	}

	iov = malloc(sizeof(*iov) * max_vectors);
	if (!iov)
		return -1;
	buffers = malloc(sizeof(*buffers) * max_vectors);
	if (!buffers)
		return -1;

	printf("file=%s write_size=%d offset=%d o_flags=0x%x wr_flags=0x%x multi_vector_alloc_size=%d\n", file, write_size, offset, o_flags, wr_flags, multi_vector_alloc_size);
	fd = open(file, o_flags, 777);
	if (fd < 0) {
		printf("could not open %s\n", file);
		return -1;	
	}

	for (i = 0; i < max_vectors; i++)
		buffers[i] = NULL;

	if (multi_vectors == 0) {
		unsigned char *ptr = buffers[0] = malloc(write_size);
		if (!buffers[0])
			return -1;
		for (byte_index = 0; byte_index < write_size; byte_index++, ptr++) {
			*ptr = rand();
		}

		iov[0].iov_len = write_size;
		iov[0].iov_base = buffers[0];
		vectors = 1;
		goto do_write;		
	}

	for (i = 0; i < max_vectors; i++) {
		unsigned char *ptr;
		posix_memalign(&buffers[i], 4096, multi_vector_alloc_size);
		if (!buffers[i])
			return -1;
		ptr = buffers[i];
		for (byte_index = 0; byte_index < multi_vector_alloc_size; byte_index++, ptr++) {
			*ptr = rand();
		}
		//printf("buffers[%d]=%p\n", i, buffers[i]);
	}

	remain = write_size;

/*
	int middle_start_align_4096 = 0;
	int middle_end_align_4096 = 0;*/

	printf("max_vectors=%d\n", max_vectors);
	for (vectors = 0, i = 0; i < max_vectors && remain > 0; i++, vectors++) {
		int sz;
		int offset;

		if (i == 0) {
			sz = 1024;
			if (middle_start_end_align_4096)
				offset = 3072;
			else
				offset = 2048;
		} else {
			if (middle_start_end_align_4096)
				offset = 0;
			else
				offset = 1024 * (rand() % 4);
			//printf("2 i=%d offset=%d\n", i, offset);
			if (remain < 4096)
				sz = remain;
			else if (middle_start_end_align_4096)
				sz = 4096;
			else {
				if (offset == 0)
					sz = 1024 + (1024 * (rand() % 3));
				else if (offset == 1024)
					sz = 1024 + (1024 * (rand() % 2));
				else if (offset == 2048)
					sz = 1024 + (1024 * (rand() % 1));
				else
					sz = 1024;
			}
		}

		iov[i].iov_len = sz;
		iov[i].iov_base = buffers[i];
		iov[i].iov_base += offset;
		remain -= sz;
		//printf("3 i=%d remain=%d\n", i, remain);
	}

do_write:
	for (i = 0; i < vectors; i++) {
		if (iov[i].iov_len)
			printf("iov[%d].iov_base=0x%x, .iov_len=%d offset=%d\n", 
				i, iov[i].iov_base, iov[i].iov_len, iov[i].iov_base - buffers[i]);
	}
	written = pwritev2(fd, iov, vectors, offset, wr_flags);
	printf("wrote %zd bytes at offset %zd\n", written, offset);
	if (written == write_size) {
		ret = 0;
	} else {
		ret = -1;
	}

	close(fd);
	for (i = 0; i < max_vectors; i++)
		free(buffers[i]);
	free(buffers);
	free(iov);
	
	return 0;
}
