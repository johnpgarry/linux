#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/uio.h>
#include <unistd.h>
#define BLOCKSIZE 512
char image[] =
{
	'P', '5', ' ', '2', '4', ' ', '7', ' ', '1', '5', '\n',
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 3, 3, 3, 3, 0, 0, 7, 7, 7, 7, 0, 0,11,11,11,11, 0, 0,15,15,15,15, 0,
	0, 3, 0, 0, 0, 0, 0, 7, 0, 0, 0, 0, 0,11, 0, 0, 0, 0, 0,15, 0, 0,15, 0,
	0, 3, 3, 3, 0, 0, 0, 7, 7, 7, 0, 0, 0,11,11,11, 0, 0, 0,15,15,15,15, 0,
	0, 3, 0, 0, 0, 0, 0, 7, 0, 0, 0, 0, 0,11, 0, 0, 0, 0, 0,15, 0, 0, 0, 0,
	0, 3, 0, 0, 0, 0, 0, 7, 7, 7, 7, 0, 0,11,11,11,11, 0, 0,15, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0,
};
int main(int argc, char *argv[])
{
	void *buffer;
	int position;
	char *file;
	int len;
	int written;
	int flags = RWF_SYNC;
	if (argc != 4) {
		printf("%s argc=%d vs expected 4\n", __func__, argc);
		return -1;
	}
	file = argv[1];
	position = atoi(argv[2]);
	len = atoi(argv[3]);

	posix_memalign(&buffer, BLOCKSIZE * 8, len);
	//buffer = malloc(len);
	
	if (buffer == 0) {
		printf("%s2 could not alloc buffer buffer=%p\n", __func__);
		return -1;
	}
	memcpy(buffer, image, sizeof(image));

	int f = open(file, O_WRONLY|O_DIRECT, S_IRWXU);
	
	if (f < 0) {
		printf("%s4 could not open %s f=%d\n", __func__, file, f);
		return f;
	}
	struct iovec iov[1] = {
		{
			.iov_base = buffer,
			.iov_len = len,
		},

	};
	written = pwritev2(f, iov, 1, position, flags);
	if (written != len) {
		printf("%s5 error written=%d != len=%d\n", __func__, written, len);
	}
	close(f);
	free(buffer);
	return 0;
}
