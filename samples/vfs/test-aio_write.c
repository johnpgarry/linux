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

#include <libaio.h>

#define DEFAULT_WRITE_SIZE 1024
#define RWF_ATOMIC      (0x00000020)

int main(int argc, char **argv)
{
	struct iovec *iov;
	void **buffers;
	ssize_t written;
	int fd;
	int rw_flags = RWF_SYNC;
	int o_flags = O_RDWR;
	off_t offset = 0;
	int opt = 0;
	unsigned int write_size = DEFAULT_WRITE_SIZE;
	char *file = NULL;
	char **argv_orig = argv;
	int argc_i;
	int middle_start_end_align_4096 = 0;
	int large_vectors = 0;
	int i, remain;
	int multi_vectors = 0;
	int max_vectors;
	int vectors;
	int byte_index;
	int multi_vector_alloc_size = -1;
	int ret;

	argv_orig++;
	for (argc_i = 0; argc_i < argc - 1; argc_i++, argv_orig++) {
		file = *argv_orig;
	}

	while ((opt = getopt(argc, argv, "l:p:PadmhS:")) != -1) {
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
				offset = atoi(optarg);
				if (offset % 512) {
					printf("offset must be multiple of 512\n");
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
			case 'h':
				printf("Options:\n");
				printf("l: write size\n");
				printf("p: write offset\n");
				printf("m: multi-vectors\n");
				printf("a: atomic\n");
				printf("d: direct I/O\n");
				printf("S: vector size\n");
				printf("P: middle start+end align to 4096\n");
				exit(0);
		}
	}

	if (multi_vectors == 0) {
		if (multi_vector_alloc_size > 0) {
			printf("multi-vector size set but not multi-vector mode\n");
			exit(0);
		}
	} else {
		if (multi_vector_alloc_size < 0) {
			multi_vector_alloc_size = 8192;

			if (multi_vector_alloc_size > write_size)
				multi_vector_alloc_size = write_size;

			printf("default multi-vector alloc size set to %d\n",
				multi_vector_alloc_size);
		}

		if (multi_vector_alloc_size > write_size) {
			printf("multi-vector size too large\n");
			exit(0);
		}
	}

	if (multi_vectors) {
		max_vectors = (4 * write_size) / multi_vector_alloc_size;
		if (max_vectors > 4096) {
			max_vectors = 4096;
		}
		if (max_vectors == 0)
			max_vectors = 1;
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

	printf("file=%s write_size=%d offset=%d o_flags=0x%x wr_flags=0x%x multi_vector_alloc_size=%d\n", file, write_size, offset, o_flags, rw_flags, multi_vector_alloc_size);
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
		int multi_vector_alloc_size_kb = multi_vector_alloc_size / 1024;

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
				sz = 1024 + (1024 * (rand() % multi_vector_alloc_size_kb));
				if (sz + offset > multi_vector_alloc_size)
					sz = multi_vector_alloc_size - offset;
			}
		}

		if (sz > remain)
			sz = remain;

		iov[i].iov_len = sz;
		iov[i].iov_base = buffers[i];
		iov[i].iov_base += offset;
		remain -= sz;
		printf("3 i=%d remain=%d offset=%d sz=%d\n", i, remain, offset, sz);
	}

	printf("vectors=%d\n", vectors);
do_write:
	for (i = 0; i < vectors; i++) {
		if (iov[i].iov_len)
			printf("iov[%d].iov_base=0x%x, .iov_len=%d offset=%d\n", 
				i, iov[i].iov_base, iov[i].iov_len, iov[i].iov_base - buffers[i]);
	}

	{
		io_context_t ctx = 0;
		int bytes_read;
		printf("calling io_setup\n");
		int r = io_setup(100, &ctx);
		printf("called io_setup r=%d\n", r);


		char buf[4096];
		struct iocb cb;
		struct iocb *list_of_iocb[1] = {&cb};
		printf("calling io_prep_pwrite\n");
		io_prep_pwrite(&cb, fd, buf, sizeof(buf), offset);
		printf("called io_prep_pwrite\n");
		cb.__pad2 |= rw_flags;

		printf("calling io_submit\n");
		struct iocb* iocbs = &cb;
		r = io_submit(ctx, 1, &iocbs);
		printf("called io_submit, r=%d\n", r);


		struct io_event events[1] = {{0}};
		r = io_getevents(ctx, 1, 1, events, NULL);
		printf("called io_getevents r=%d\n", r);

		bytes_read = events[0].res;
		printf("bytes_read=%d\n", bytes_read);
	}

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
