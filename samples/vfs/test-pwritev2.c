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
#define O_ATOMIC	0x800000
int main(int argc, char **argv)
{
	struct iovec *iov = NULL;
	void **buffers = NULL;
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
	int max_vectors;
	int vectors;
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
	printf("%s O_DIRECT=0x%x\n", __func__, O_DIRECT);
	while ((opt = getopt(argc, argv, "l:p:PaAdmhS:vH")) != -1) {
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
			case 'A':
				o_flags |= O_ATOMIC;
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
	printf("demo_hch_problem=%d multi_vectors=%d\n", demo_hch_problem, multi_vectors);
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
		if (demo_hch_problem)
			max_vectors = 256;
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

	if (demo_hch_problem) {
		max_vectors = 256;
		write_size = 1024 * 256;
	}

	printf("2 demo_hch_problem=%d multi_vectors=%d\n", demo_hch_problem, multi_vectors);
	iov = malloc(sizeof(*iov) * max_vectors);
	if (!iov)
		return -1;
	buffers = malloc(sizeof(*buffers) * max_vectors);
	if (!buffers)
		return -1;

	printf("file=%s write_size=%d pos=%ld o_flags=0x%x rw_flags=0x%x multi_vector_alloc_size=%d\n",
		file, write_size, pos, o_flags, rw_flags, multi_vector_alloc_size);
	fd = open(file, o_flags, 777);
	if (fd < 0) {
		printf("could not open %s\n", file);
		return -1;
	}

	for (i = 0; i < max_vectors; i++)
		buffers[i] = NULL;

	if (multi_vectors == 0) {
		posix_memalign(&buffers[0], 4096, write_size);
		if (!buffers[0])
			return -1;
		unsigned char *ptr = buffers[0];
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

		if (demo_hch_problem) {
			sz = 1024;
			offset = 3584;
		} else if (i == 0) {
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
			printf("iov[%d].iov_base=%p, .iov_len=%zd offset=%zd\n",
				i, iov[i].iov_base, iov[i].iov_len, iov[i].iov_base - buffers[i]);
	}
	written = pwritev2(fd, iov, vectors, pos, rw_flags);
	printf("wrote %zd bytes at pos %zd write_size=%d\n", written, pos, write_size);
	if (written == write_size) {
		ret = 0;
	} else {
		ret = -1;
		goto end;
	}

	if (verify == 0)
		goto end;

	read_buffer = malloc(write_size);
	if (!read_buffer) {
		printf("could not alloc read buffer\n");
		return -1;
	}
	struct iovec iov_read = {
		.iov_base = read_buffer,
		.iov_len = write_size,
	};
	int dataread = preadv2(fd, &iov_read, 1, pos, 0);
	if (dataread == write_size) {
		ret = 0;
	} else {
		printf("read incorrect amount of data, wanted=%d got=%d\n", write_size, dataread);
		return -1;
	}

	remain = write_size;
	char *read_data = read_buffer;
	for (i = 0; i < vectors; i++) {
		char *tmp_buf = iov[i].iov_base;
		if (memcmp(read_data, tmp_buf, iov[i].iov_len)) {
			int incorrect_pos = 0;

			for (;;) {
				if (*read_data != *tmp_buf)
					break;
				incorrect_pos++;
			}
			printf("read back incorrect data for vector=%d wanted for pos %d [%d] vs got [%d] iov[i].iov_len=%ld\n", i, incorrect_pos,
				*tmp_buf, *read_data, iov[i].iov_len);
			return -1;
		}
		read_data += iov[i].iov_len;
	}
	printf("read back and compared ok data, wanted size=%d got=%d\n", write_size, dataread);
end:
	close(fd);
	for (i = 0; i < max_vectors; i++)
		free(buffers[i]);
	free(buffers);
	free(iov);
	free(read_buffer);

	return 0;
}
