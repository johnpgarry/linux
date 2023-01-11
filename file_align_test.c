
#define _GNU_SOURCE

#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include <unistd.h>
#include <sys/uio.h>
char string1[] __attribute__((aligned(4096))) = "The " ;  
char string2[] __attribute__((aligned(4096))) = "clumber\n" ;

char data1[1024] __attribute__((aligned(4096))) = "The " ;  
char data2[1024] __attribute__((aligned(4096))) = "clumber\n" ;

int main (int argc, char *argv[]) {
	int pfd;
	char *filename;
	unsigned int offset;
	#define RWF_SNAKE	(0x00000020)
	int flags = RWF_SNAKE;
	
	char *str0 = string1;
	char *str1 = string2;
	const struct iovec iov1[2] = {
		{
			.iov_base = str0,
			.iov_len = strlen(str0),
		},
		{
			.iov_base = str1,
			.iov_len = strlen(str1),
		}
	};
	const struct iovec iov2[2] = {
		{
			.iov_base = &data1,
			.iov_len = sizeof(data1),
		},
		{
			.iov_base = &data2,
			.iov_len = sizeof(data2),
		}
	};
	ssize_t nwritten;

	if (argc != 3)
		exit(1);

	filename = argv[1];

	if ((pfd = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) == -1)
	{
		printf("open error pfd=%d\n", pfd);
		exit(1);
	}
	offset = atoi(argv[2]);
	printf("pfd=%d offset=%d argv[2]=%s iov2[]={{%p %zu}, {%p %zu}}\n", pfd, offset, argv[2], iov2[0].iov_base, iov2[0].iov_len, iov2[1].iov_base, iov2[1].iov_len);
	nwritten = pwritev2(pfd, &iov2[0], 2, offset, flags);
	printf("nwritten=%d\n", nwritten);


//	ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt,
//                off_t offset);
//ssize_t pwritev2(int fd, const struct iovec *iov, int iovcnt,
//                 off_t offset, int flags);

	close(pfd);
	return 0;
}
