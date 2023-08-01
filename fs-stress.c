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

enum ACTION {
    ACTION_CREATE = 0,
    ACTION_DELETE = 5,
    ACTION_WRITE = 10,
    ACTION_TRUNCATE = 11,
    ACTION_COUNT
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

int create_file(char *dir_path, unsigned long long file_name_create_index)
{
    int rc = 0;
    char file_name[256];
    int fd;
   
    sprintf(file_name, "%s/file_%lld", dir_path, file_name_create_index);

    fd = open(file_name, O_CREAT | O_WRONLY);
    if (fd < 0) {

        printf("%s could not open filename=%s fd=%d\n", __func__, file_name, fd);
        return fd;
    }
    close(fd);

    return rc;
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
    unsigned long average_file_size;
    unsigned long file_count = 0;
    unsigned long long file_name_create_index = 0;

    if (argc < 4) {
        printf("Usage: mount_dev dir_absolute_path file_target_count\n", argv[0]);
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
    size *= 512;

    average_file_size = size / file_target_count;

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

    printf("7 average_file_size=%ld \n", average_file_size);

    while (1) {
        enum ACTION action = get_random_action();
        if (action < 0)
            break;

        switch (action) {
            case ACTION_CREATE:
            {
                int rc = create_file(dir_path, file_name_create_index);
                if (rc < 0) {
                    printf("could not create file %lld, rc=%d\n", file_name_create_index, rc);
                    break;
                }
                file_name_create_index++;
                file_count++;
                continue;
            }
            case ACTION_DELETE:
            {
                if (file_count == 0)
                    continue;
                continue;
            }
            case ACTION_WRITE:
            {
                if (file_count == 0)
                    continue;
                continue;
            }
            case ACTION_TRUNCATE:
            {
                if (file_count == 0)
                    continue;
                continue;
            }
            default: {
                printf("unknown action %d", action);
                return -1;
            }
        }
    }


    return 0;
}
