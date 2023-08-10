
#define _GNU_SOURCE
#define _ATFILE_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <fts.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>
#include <sys/types.h>


#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <linux/stat.h>
#include <linux/fcntl.h>
#define statx foo
#define statx_timestamp foo_timestamp
struct statx;
struct statx_timestamp;
#include <sys/stat.h>
#undef statx
#undef statx_timestamp

#define AT_STATX_SYNC_TYPE  0x6000
#define AT_STATX_SYNC_AS_STAT   0x0000
#define AT_STATX_FORCE_SYNC 0x2000
#define AT_STATX_DONT_SYNC  0x4000

#define STATX_WRITE_ATOMIC  0x00004000U /* Want/got atomic_write_* fields */

#ifndef __NR_statx
#define __NR_statx -1
#endif

extern int frag_report(const char *filename, int alignment);
extern int open(char *, int);

enum ACTION {
    ACTION_CREATE = 0,
    ACTION_DELETE = 3,
    ACTION_WRITE = 5,
    ACTION_TRUNCATE = 100,
    ACTION_COUNT = ACTION_TRUNCATE + 2
};

enum ACTION get_random_action(void)
{
    int r = rand() % ACTION_COUNT;

    switch (r) {
        case 0 ... ACTION_DELETE-1:
            return ACTION_CREATE;
        case ACTION_DELETE ... ACTION_WRITE-1:
            return ACTION_DELETE;
        case ACTION_WRITE ... ACTION_TRUNCATE-1:
            return ACTION_WRITE;
        case ACTION_TRUNCATE ... ACTION_COUNT:
            return ACTION_TRUNCATE;
        default:
            return -1;
    }

    return -1;
}

int create_file(char *dir_path, unsigned long long file_name_create_index, char **file_name)
{
    int fd;
    unsigned char buffer[8192];
    ssize_t size_written;
    char mode[] = "0777";
    int i = strtol(mode, 0, 8);
    int rc;
    
    *file_name = malloc(256);
    if (!*file_name)
        return -1;

    for (fd=0;fd<8192;fd++)
        buffer[fd] = (char)fd;
   
    sprintf(*file_name, "%s/file_%lld", dir_path, file_name_create_index);

    printf("%s create file %s\n", __func__, file_name);
    fd = open(*file_name, O_CREAT | O_WRONLY);
    if (fd < 0) {
        printf("%s could not open filename=%s fd=%d\n", __func__, file_name, fd);
        return fd;
    }
    size_written = write(fd, &buffer[0], 8192);
    close(fd);

    if (size_written != 8192) {
        printf("%s only wrote %zd byte(s) for %s\n", __func__, size_written, file_name);
        return -1;
    }

    rc = chmod(*file_name, i);
    if (rc < 0) {
        printf("%s could not change mode for %s rc=%d i=%d\n", __func__, file_name, rc, i);
        return -1;
    }

    return 0;
}

int compare(const FTSENT** one, const FTSENT** two)
{
    return (strcmp((*one)->fts_name, (*two)->fts_name));
}

void delete_all_files(char * const * dir_path)
{
    FTS* file_system = NULL;
    FTSENT *node    = NULL;
    char *dir_name = *dir_path;

    printf("%s dir_path=%s dir_name=%s\n", __func__, *dir_path, dir_name);
    while (1) {
        char *tmp;
        dir_name++;
       // printf("%s00 dir_name=%s tmp=%s\n", __func__, dir_name, tmp);
        tmp = strstr(dir_name, "/");
       // printf("%s01 dir_name=%s tmp=%s\n", __func__, dir_name, tmp);
        if (!tmp)
            break;
    }

    printf("%s1 dir_path=%s dir_name=%s\n", __func__, *dir_path, dir_name);
    file_system = fts_open(dir_path, FTS_COMFOLLOW | FTS_NOCHDIR, &compare);
    if (!file_system) {
        printf("Could not open %s\n", *dir_path);
    }
    printf("%s2 file_system=%p\n", __func__, file_system);
    while( (node = fts_read(file_system)) != NULL) 
    {
            switch (node->fts_info) 
            {
                case FTS_D :
                case FTS_F :
                case FTS_SL:
                    {
                        //printf("%s3 node->fts_name=%s fts_path=%s fts_accpath=%s\n",
                        //    __func__, node->fts_name, node->fts_path, node->fts_accpath);
                        if (!strcmp(node->fts_name, "."))
                            continue;
                        if (!strcmp(node->fts_name, ".."))
                            continue;
                        if (!strcmp(node->fts_name, dir_name))
                            continue;
                       // printf("%s4 node->fts_name=%s dir_name=%s\n", __func__, node->fts_name, dir_name);
                        if (remove(node->fts_accpath) < 0) {
                        	printf("Failed to remove file %s\n", node->fts_name);
                            fts_close(file_system);
                            exit(0);
                        }
                    }
                    break;
                default:
                    break;
            }
    }
    fts_close(file_system);
}


static __attribute__((unused))
ssize_t statx(int dfd, const char *filename, unsigned flags,
          unsigned int mask, struct statx *buffer)
{
    int rc;
   // printf("%s dfd=%d filename=%s flags=%d mask=%d __NR_statx=%d\n", __func__, dfd, filename, flags, mask, __NR_statx);
    rc= syscall(__NR_statx, dfd, filename, flags, mask, buffer);
  //  printf("%s2 dfd=%d filename=%s flags=%d mask=%d rc=%d\n", __func__, dfd, filename, flags, mask, rc);
    
    return rc;
}

char *find_random_file_name(char * const * dir_path, unsigned long file_index)
{
    char *fts_accpath = NULL;
    char *dir_name = *dir_path;
    FTS* file_system = NULL;
    FTSENT *node    = NULL;
    unsigned long count = 0;


    while (1) {
        char *tmp;
        dir_name++;
       // printf("%s00 dir_name=%s tmp=%s\n", __func__, dir_name, tmp);
        tmp = strstr(dir_name, "/");
       // printf("%s01 dir_name=%s tmp=%s\n", __func__, dir_name, tmp);
        if (!tmp)
            break;
    }

    file_system = fts_open(dir_path, FTS_COMFOLLOW | FTS_NOCHDIR, &compare);
    if (!file_system) {
        printf("Could not open %s\n", *dir_path);
    }

    while( (node = fts_read(file_system)) != NULL) 
    {
            switch (node->fts_info) 
            {
                case FTS_D :
                case FTS_F :
                case FTS_SL:
                    {
                        //printf("%s3 node->fts_name=%s fts_path=%s fts_accpath=%s\n",
                        //    __func__, node->fts_name, node->fts_path, node->fts_accpath);
                        if (!strcmp(node->fts_name, "."))
                            continue;
                        if (!strcmp(node->fts_name, ".."))
                            continue;
                        if (!strcmp(node->fts_name, dir_name))
                            continue;
                        if (count == file_index) {
                            fts_accpath = strdup(node->fts_accpath);
                            goto found_file;
                        }
                        count++;
                    }
                    break;
                default:
                    break;
            }
    }
found_file:
    fts_close(file_system);
    
    return fts_accpath;
}

int write_to_file(char * const * dir_path, char **file_to_write_to, unsigned long file_count, unsigned long average_file_size_bytes)
{
    int fd;
    char *dir_name = *dir_path;
    char *fts_accpath;
    struct statx stx_buffer;
    int stx_atflag = AT_SYMLINK_NOFOLLOW;
    int rc;
    unsigned int stx_mask = STATX_ALL;
    ssize_t size_written;
    struct iovec iov[1];
    int type_of_write;
    ssize_t size_of_write;
    int offset_of_write;
    unsigned long average_file_size_sectors = average_file_size_bytes / 512;
    unsigned long file_index = rand() % file_count;

    printf("%s *dir_path=%s file_index=%ld file_count=%ld dir_name=%s average_file_size_sectors=%ld bytes=%ld\n",
        __func__, *dir_path, file_index, file_count, dir_name, average_file_size_sectors, average_file_size_bytes);
    
    fts_accpath = find_random_file_name(dir_path, file_index);
    if (fts_accpath == NULL) {
        printf("%s2 fts_accpath=%s\n", __func__, fts_accpath);
        return -1;
    }

    *file_to_write_to = fts_accpath;
    //flags=256 mask=20479
    rc = statx(-100, fts_accpath, 256, 20479, &stx_buffer);
    if (rc) {
      printf("%s4 statx(%s) = %d\n", __func__, fts_accpath, rc);
      return -1;
    }

    printf("%s4 statx(%s) stx_size = %lld stx_blocks=%lld\n", __func__, fts_accpath, stx_buffer.stx_size, stx_buffer.stx_blocks);

    fd = open(fts_accpath, O_WRONLY);
    if (fd < 0) {
        printf("%s5 could not open fts_accpath=%s fd=%d\n", __func__, fts_accpath, fd);
        return fd;
    }


    printf("%s6 stx_blocks=%lld\n", __func__, stx_buffer.stx_blocks);

restart:
    if (stx_buffer.stx_blocks == 0) {
        size_of_write = 1;
        offset_of_write = 0;
        type_of_write = -1;
    } else {
        type_of_write = rand() % 50;
        switch (type_of_write) {
        case 21 ... 30:
            // Re-write all current data
            size_of_write = stx_buffer.stx_blocks;
            offset_of_write = 0;
            break;
        case 31 ... 38:
            // write from end of data
            while (1) {
                size_of_write = rand() % stx_buffer.stx_blocks;
                size_of_write /= 10;
                size_of_write++;
                if (size_of_write > 0)
                    break;
            }
            offset_of_write = stx_buffer.stx_blocks;
            break;
        case 39 ... 49:
            // Extend file with a hole
            // write from end of data
            while (1) {
                size_of_write = rand() % stx_buffer.stx_blocks;
                size_of_write /= 10;
                size_of_write++;
                break;
            }
            while (1) {
                offset_of_write = rand() % stx_buffer.stx_blocks;
                offset_of_write /= 10;
                offset_of_write++;
                offset_of_write += stx_buffer.stx_blocks;
                break;
            }
            break;
        case 0 ... 20:
        default:
            // Just write a subset of current data
            while (1) {
                size_of_write = rand() % stx_buffer.stx_blocks;
                if (size_of_write > 0)
                    break;
            }
            
            while (1) {
                offset_of_write = rand() % stx_buffer.stx_blocks;
                if (size_of_write + offset_of_write <= stx_buffer.stx_blocks)
                    break;
            }
            break;
        }

        if (offset_of_write + size_of_write > average_file_size_sectors) {
                goto restart;
        }
    }

    printf("%s7 writing size_of_write=%zd offset_of_write=%d stx_blocks=%lld type_of_write=%d\n", __func__,
        size_of_write, offset_of_write, stx_buffer.stx_blocks, type_of_write);

    size_of_write *= 512;
    offset_of_write *= 512;

    iov[0].iov_len = size_of_write;

    iov[0].iov_base = malloc(iov[0].iov_len);
    if (!iov[0].iov_base) {
        close(fd);
        printf("%s8 could not alloc buffer of size %zd\n", __func__, iov[0].iov_len);
        return -1;
    }

    size_written = pwritev2(fd, iov, 1, offset_of_write, 0);
    free(iov[0].iov_base);

    close(fd);
    if (size_written != iov[0].iov_len) {
        printf("%s9 wrote %zd, but should have written %zd\n", __func__, size_written, iov[0].iov_len);
        return -1;
    }

    return 0;
}


int truncate_file(char * const * dir_path, char **file_to_truncate, unsigned long file_count)
{
    int file_to_delete_index = rand() % file_count;
    char *fts_accpath;
    struct statx stx_buffer;
    int rc;
    unsigned long new_size;

    fts_accpath = find_random_file_name(dir_path, file_to_delete_index);
    if (fts_accpath == NULL) {
        printf("%s3 fts_accpath=%s\n", __func__, fts_accpath);
        return -1;
    }
    *file_to_truncate = fts_accpath;


    rc = statx(-100, fts_accpath, 256, 20479, &stx_buffer);
    if (rc) {
      printf("%s1 statx(%s) = %d failed\n", __func__, fts_accpath, rc);
      return -1;
    }
    printf("%s statx(%s) stx_size = %lld stx_blocks=%lld\n", __func__, fts_accpath, stx_buffer.stx_size, stx_buffer.stx_blocks);
    if (stx_buffer.stx_blocks == 0)
        return 0;

    new_size = rand() % stx_buffer.stx_blocks;

    if (truncate(fts_accpath, new_size * 512)) {
        printf("%s truncate failed for file %s\n", __func__, fts_accpath);
        return -1;
    }

    rc = statx(-100, fts_accpath, 256, 20479, &stx_buffer);
    if (rc) {
      printf("%s2 statx(%s) = %d failed\n", __func__, fts_accpath, rc);
      return -1;
    }

    if (stx_buffer.stx_blocks != new_size) {
      printf("%s3 for %s, expected %lld blocks, got %ld blocks\n", __func__, fts_accpath, stx_buffer.stx_blocks, new_size);
     // return -1;
    }

    return 0;
}

int delete_file(char * const * dir_path, char **file_to_delete, int file_count)
{
    int file_to_delete_index = rand() % file_count;
    char *fts_accpath;
    fts_accpath = find_random_file_name(dir_path, file_to_delete_index);
    printf("%s delete file %s file_count=%d\n", __func__, fts_accpath, file_count);
    if (fts_accpath == NULL) {
        printf("%s3 fts_accpath=%s\n", __func__, fts_accpath);
        return -1;
    }
    *file_to_delete = fts_accpath;

    if (remove(fts_accpath)) {
        printf("%s3 could not remove=%s\n", __func__, fts_accpath);
        return -1;
    }

    return 0;
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
    int file_target_count;
    unsigned long average_file_size_bytes;
    unsigned long file_count = 0;
    unsigned long long file_name_create_index = 0;

    if (argc < 4) {
        printf("Usage: mount_dev dir_absolute_path file_target_count\n");
        exit(255);
    }

    mount_dev = argv[1];
    dir_path = argv[2];
    file_target_count = atoi(argv[3]);

    printf("mount_dev=%s dir_path=%s file_target_count=%d\n", mount_dev, dir_path, file_target_count);

    mount_dev = strstr(mount_dev, "/");
    if (!mount_dev) {
         printf("2 mount_dev=NULL\n");
        return -1;
    }
    mount_dev = strstr(mount_dev + 1, "/");
    if (!mount_dev) {
         printf("3 mount_dev=NULL\n");
        return -1;
    }
    sprintf(class_file, "/sys/class/block/%s/size", mount_dev + 1);
    fd = open(class_file, O_RDONLY);
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
    size *= 512;

    average_file_size_bytes = size / file_target_count;

    fd = mkdir(dir_path, 0777);
    if (fd < 0) {
        // printf("7 fd=%d for %s\n", fd, dir_path);
        //return fd;
    }
    fd = open(dir_path, O_DIRECTORY);
    if (fd < 0) {
         printf("8 fd=%d for %s\n", fd, dir_path);
        //return fd;
    }
    argv += 2;
    delete_all_files(argv);

    close(fd);

    printf("7 average_file_size_bytes=%ld \n", average_file_size_bytes);

    while (1) {
        enum ACTION action;
        char *file_name = NULL;
        if (file_count > 5)
            usleep(500000);
        action = get_random_action();
        if (action < 0)
            break;

        switch (action) {
            case ACTION_CREATE:
            {
                int rc = create_file(dir_path, file_name_create_index, &file_name);
                if (rc < 0) {
                    free(file_name);
                    printf("could not create file %lld, rc=%d\n", file_name_create_index, rc);
                    exit(0);
                }
                frag_report(file_name, 4);
                file_name_create_index++;
                file_count++;
                break;
            }
            case ACTION_DELETE:
            { 
                int rc;
                char *file_to_delete = NULL;
                if (file_count == 0)
                    break;
                rc = delete_file(argv, &file_to_delete, file_count);
                if (rc < 0) {
                    printf("could not delete file %s, rc=%d\n", file_to_delete, rc);
                    free(file_to_delete);
                    exit(0);
                }
                file_count--;
                free(file_to_delete);
                break;
            }
            case ACTION_WRITE:
            {
                char *file_to_write_to = NULL;
                if (file_count == 0)
                    break;
                int rc = write_to_file(argv, &file_to_write_to, file_count, average_file_size_bytes);
                if (rc < 0) {
                    printf("could not write to file %s, rc=%d\n", file_to_write_to, rc);
                    free(file_to_write_to);
                    exit(0);
                }
                free(file_to_write_to);
                break;
            }
            case ACTION_TRUNCATE:
            {
                char *file_to_truncate = NULL;
                if (file_count == 0)
                   break;
                int rc = truncate_file(argv, &file_to_truncate, file_count);
                if (rc < 0) {
                    printf("could not %s, rc=%d\n", file_to_truncate, rc);
                    free(file_to_truncate);
                    exit(0);
                }
                free(file_to_truncate);
                break;
            }
            default: {
                printf("unknown action %d", action);
                return -1;
            }
        }

        free(file_name);
    }


    return 0;
}
