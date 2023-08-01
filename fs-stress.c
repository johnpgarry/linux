#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <fts.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>

#include <linux/stat.h>
#include <sys/stat.h>
#include <fcntl.h>
#define __NR_statx 291

unsigned int mask = STATX_BASIC_STATS | STATX_BTIME;

static ssize_t statx(int fd, const char *filename, unsigned flags,
    unsigned mask, struct statx *buffer)
{
    return syscall(__NR_statx, fd, filename, flags, mask, buffer);

}

#define PAGE_SIZE 4096
int main(int argc, char* const argv[])
{
    char *mount_dev;
    char *dir_path;
    struct statx stx;
    int err;
    int atflag = AT_SYMLINK_NOFOLLOW;
    int fd;
    char class_file[256];
    int read_size;
    unsigned long size;
    char read_buffer[PAGE_SIZE];
    int file_count;

    if (argc < 4) {
        printf("Usage: mount_dev dir_absolute_path file_count\n", argv[0]);
        exit(255);
    }

    mount_dev = argv[1];
    dir_path = argv[2];
    file_count = atoi(argv[3]);

    printf("mount_dev=%s file_count=%d\n", mount_dev, file_count);

    mount_dev = strstr(mount_dev, "/");
    if (!mount_dev) {
         printf("2 mount_dev=NULL\n");
        return -1;
    }
    mount_dev = strstr(mount_dev, "/");
    if (!mount_dev) {
         printf("3 mount_dev=NULL\n");
        return -1;
    }
    sprintf(class_file, "/sys/class/block/%s/size", mount_dev);
    fd = open("/sys/class/block/sda/size", O_RDONLY);
    if (fd < 0) {
        printf("4 fd=%d for %s\n", fd, class_file);
        return fd;
    }
    read_size = read(fd, read_buffer, PAGE_SIZE);
    printf("5 read_size=%d read_buffer=%s\n", read_size, read_buffer);
    close(fd);
    if (read_size < 0)
        return read_size;
    size = atol(read_buffer);
    printf("6 size=%ld sectors or %ld bytes\n", size, size * 512);

    fd = mkdir(dir_path, 0666);
    if (fd < 0) {
        // printf("7 fd=%d for %s\n", fd, dir_path);
        //return fd;
    }
    fd = open(dir_path, O_DIRECTORY);
    if (fd < 0) {
         printf("8 fd=%d for %s\n", fd, dir_path);
        //return fd;
    }

    close(fd);

    return 0;
}
