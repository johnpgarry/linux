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
#define RWF_ATOMIC      (0x00000040)

int main(int argc, char **argv)
{
	struct iocb **cbs = NULL, **cbs_tmp;
	struct iocb *_cbs = NULL;
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
	int start_align_4096 = 0;
	int large_vectors = 0;
	int i, remain;
	int multi_vectors = 0;
	int max_vectors;
	int vectors;
	int byte_index;
	int multi_vector_alloc_size = -1;
	int use_iovec = 0;
	int ret;
	loff_t pos = 0;
	char *read_buffer = NULL;
	int io_submit_vectors = 0;

	argv_orig++;
	for (argc_i = 0; argc_i < argc - 1; argc_i++, argv_orig++) {
		file = *argv_orig;
	}

	while ((opt = getopt(argc, argv, "l:p:PadmhS:i")) != -1) {
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
			case 'i':
				use_iovec = 1;
				break;
			case 'a':
				rw_flags |= RWF_ATOMIC;
				break;
			case 'd':
				o_flags |= O_DIRECT;
				break;
			case 'P':
				start_align_4096 = 1;
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
				printf("P: start vector align to 4096\n");
				printf("i: use iovec for multi-vector\n");
				exit(0);
		}
	}

	if (multi_vectors == 0) {
		if (multi_vector_alloc_size > 0) {
			printf("multi-vector size set but not multi-vector mode\n");
			exit(0);
		}
		if (use_iovec == 1) {
			printf("use iovec set but not multi-vector mode\n");
			exit(0);
		}
		io_submit_vectors = 1;
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
	}

	printf("start_align_4096=%d\n", start_align_4096);
	cbs = malloc(sizeof(struct iocb *) * max_vectors);
	if (!cbs)
		return -1;
	_cbs = malloc(sizeof(*_cbs) * max_vectors);
	if (!_cbs)
		return -1;
	cbs_tmp = cbs;
	for (i = 0; i < max_vectors; i++, cbs_tmp++)
		*cbs_tmp = &_cbs[i];

	iov = malloc(sizeof(*iov) * max_vectors);
	if (!iov)
		return -1;
	buffers = malloc(sizeof(*buffers) * max_vectors);
	if (!buffers)
		return -1;

	printf("file=%s write_size=%d pos=%ld o_flags=0x%x wr_flags=0x%x multi_vector_alloc_size=%d\n",
		file, write_size, pos, o_flags, rw_flags, multi_vector_alloc_size);
	fd = open(file, o_flags, 777);
	if (fd < 0) {
		printf("could not open %s\n", file);
		return -1;
	}

	for (i = 0; i < max_vectors; i++)
		buffers[i] = NULL;

	if (multi_vectors == 0) {
		if (start_align_4096)
			posix_memalign(&buffers[0], 4096, write_size);
		else
			buffers[0] = malloc(write_size);
		if (!buffers[0])
			return -1;
		unsigned char *ptr = buffers[0];
		for (byte_index = 0; byte_index < write_size; byte_index++, ptr++) {
			*ptr = rand();
		}

		io_prep_pwrite(&_cbs[0], fd, buffers[0], write_size, pos);
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

	printf("max_vectors=%d use_iovec=%d\n", max_vectors, use_iovec);
	loff_t pos_tmp = pos;
	for (vectors = 0, i = 0; i < max_vectors && remain > 0; i++, vectors++) {
		int sz;
		int offset;
		int multi_vector_alloc_size_kb = multi_vector_alloc_size / 1024;

		if (i == 0) {
			sz = 1024;
			if (start_align_4096)
				offset = 0;
			else
				offset = 2048;
		} else {
			if (start_align_4096)
				offset = 0;
			else
				offset = 1024 * (rand() % 4);
			//printf("2 i=%d offset=%d\n", i, offset);
			if (remain < 4096)
				sz = remain;
			else {
				sz = 1024 + (1024 * (rand() % multi_vector_alloc_size_kb));
				if (sz + offset > multi_vector_alloc_size)
					sz = multi_vector_alloc_size - offset;
			}
		}

		if (sz > remain)
			sz = remain;

		if (use_iovec) {
			iov[i].iov_len = sz;
			iov[i].iov_base = buffers[i];
			iov[i].iov_base += offset;

			printf("3 i=%d remain=%d offset=%d sz=%d\n", i, remain, offset, sz);
		} else {
			void *my_buf = buffers[i];
			my_buf += offset;
			printf("4 i=%d my_buf=%p remain=%d offset=%d sz=%d &_cbs[i]=%p pos_tmp=%ld\n", i, my_buf, remain, offset, sz, &_cbs[i], pos_tmp);
			io_prep_pwrite(&_cbs[i], fd, my_buf, sz, pos_tmp);
			_cbs[i].__pad2 |= rw_flags;
			pos_tmp += sz;
		}
		remain -= sz;
	}

	if (use_iovec) {
		io_submit_vectors = 1;
		io_prep_pwritev(&_cbs[0], fd, iov, vectors, pos);
	} else {
		io_submit_vectors = vectors;
	}

	printf("vectors=%d io_submit_vectors=%d\n", vectors, io_submit_vectors);
do_write:
	_cbs[0].__pad2 |= rw_flags;
	for (i = 0; i < vectors; i++) {
		printf("cbs[%d]=%p  \t", i, cbs[i]);
		if (_cbs[i].u.c.nbytes)
			printf("cbs[%d].u.c.buf=%p, .nbytes=%ld offset=%lld\n",
				i, _cbs[i].u.c.buf, _cbs[i].u.c.nbytes, _cbs[i].u.c.offset);
		if (iov[i].iov_len)
			printf("iov[%d].iov_base=%p, .iov_len=%zd offset=%ld %s\n",
				i, iov[i].iov_base, iov[i].iov_len, iov[i].iov_base - buffers[i],
				iov[i].iov_len + iov[i].iov_base - buffers[i] > 4096 ? "*" : "");
	}

	io_context_t ctx = 0;
	printf("calling io_setup vectors=%d\n", vectors);
	int r = io_setup(100, &ctx);
	printf("called io_setup r=%d\n", r);

	printf("calling io_submit io_submit_vectors=%d cbs=%p\n", io_submit_vectors, cbs);
	r = io_submit(ctx, io_submit_vectors, cbs);
	printf("called io_submit, r=%d io_submit_vectors=%d %s cbs=%p\n", r, io_submit_vectors, io_submit_vectors == r ? "" : "ERROR", cbs);

	if (r != io_submit_vectors) {
		exit(0);
	}

	struct io_event events[1024] = {{0}};
	r = io_getevents(ctx, io_submit_vectors, io_submit_vectors, events, NULL);
	printf("called io_getevents r=%d io_submit_vectors=%d\n", r, io_submit_vectors);
	written = 0;
	for (i = 0; i < io_submit_vectors; i++) {
		printf("io_getevents i=%d wrote %zd bytes\n", i, events[i].res);
		written += events[i].res;
	}

	printf("wrote %zd bytes at pos %zd write_size=%d cbs=%p\n", written, pos, write_size, cbs);
	if (written == write_size) {
		ret = 0;
	} else {
		ret = -1;
		goto end;
	}

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
	unsigned char *read_data = read_buffer;
	for (i = 0; i < vectors; i++) {
		unsigned char *tmp_buf;
		unsigned long memcmp_len;

		if (multi_vectors) {
			memcmp_len = iov[i].iov_len;
			tmp_buf = iov[i].iov_base;
		} else {
			memcmp_len = _cbs[i].u.c.nbytes;
			tmp_buf = _cbs[i].u.c.buf;
		}

		if (memcmp(read_data, tmp_buf, memcmp_len)) {
			int incorrect_pos = 0;

			for (;;) {
				if (*read_data != *tmp_buf)
					break;
				incorrect_pos++;
				read_data++;
				tmp_buf++;
			}
			printf("read back incorrect data for vector=%d wanted for pos %d [0x%x] vs got [0x%x] memcmp_len=%ld\n", i, incorrect_pos,
				*tmp_buf, *read_data, memcmp_len);
			return -1;
		}
		read_data += memcmp_len;
	}

	printf("read back and compared ok data, wanted size=%d got=%d\n", write_size, dataread);
end:

	printf("end: max_vectors=%d buffers=%p\n", max_vectors, buffers);
	close(fd);
	for (i = 0; i < max_vectors && buffers; i++)
		free(buffers[i]);

	free(buffers);
	free(cbs);
	free(iov);
	free(read_buffer);

	return 0;
}
