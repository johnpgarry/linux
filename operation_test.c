#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/uio.h>
#include <unistd.h>
#include <math.h>
#define SECTOR_SIZE 512
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

int do_operation(int f, int size)
{
	void *buffer;
	unsigned int position = rand();
	unsigned int len;
	int written;
	int flags = RWF_SYNC;
	
	if (size == -1) {
		int i;
		int power = rand() % 5;
		size = 1;
		if (power) {
			for (i = 0; i <= power; i++)
				size *= 2;
		}
	}

calculate_len:
	len = rand() % 256;
	len = len * size;
	if (len == 0)
		goto calculate_len;
	// +int queue_write_atomic_alignment_fs_blocks = 128;
	if (len >= (128 * 8 * 4)) //2nd 4 is arbitary
		goto calculate_len;

	len *= 1024;
	//printf("%s len=%d\n", __func__, len);

	position = position % 48;
	position = position * size * 1024;

	posix_memalign(&buffer, SECTOR_SIZE * 8, len);
	//buffer = malloc(len);
	
	if (buffer == 0) {
		printf("%s2 could not alloc buffer buffer=%p\n", __func__);
		return -1;
	}
	memcpy(buffer, image, sizeof(image));

	struct iovec iov[1] = {
		{
			.iov_base = buffer,
			.iov_len = len,
		},

	};
	printf("%s len=%d (%d) position=%d (%d) bs=%d\n", __func__, len, len / 4096, position, position / 4096, size);
	written = pwritev2(f, iov, 1, position, flags);
	free(buffer);
	if (written != len) {
		printf("%s5 error written=%d != len=%d\n", __func__, written, len);
		return -1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	char *file;
	int size;
	int f;
	int count = 0;
	
	if (argc == 2) {
		file = argv[1];
		size = -1;
	} else if (argc == 3) {
		file = argv[1];
		size = atoi(argv[2]);
	} else {
		printf("%s argc=%d vs expected 4\n", __func__, argc);
		return -1;
	}

	f = open(file, O_WRONLY|O_DIRECT, S_IRWXU);
	if (f < 0) {
		printf("%s4 could not open %s f=%d\n", __func__, file, f);
		return f;
	}

	while (1) {
		int ret = do_operation(f, size);
		if (ret < 0)
			break;
		count++;

		if (count >= 100000)
			break;
		//sleep(1);
	}
	
	close(f);
	return 0;
}
