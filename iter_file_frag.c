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

int compare (const FTSENT**, const FTSENT**);
void indent (int i);

#define WAIT_FOR_COMPLETION
static int exec_prog(const char **argv)
{
    pid_t   my_pid;
    int     status, timeout /* unused ifdef WAIT_FOR_COMPLETION */;

    if (0 == (my_pid = fork())) {
            if (-1 == execve(argv[0], (char **)argv , NULL)) {
                    perror("child process execve failed [%m]");
                    return -1;
            }
    }

#ifdef WAIT_FOR_COMPLETION
    timeout = 1000;

    while (0 == waitpid(my_pid , &status , WNOHANG)) {
            if ( --timeout < 0 ) {
                    perror("timeout");
                    return -1;
            }
            sleep(1);
    }

    printf("%s WEXITSTATUS %d WIFEXITED %d [status %d]\n",
            argv[0], WEXITSTATUS(status), WIFEXITED(status), status);

    if (1 != WIFEXITED(status) || 0 != WEXITSTATUS(status)) {
            perror("%s failed, halt system");
            return -1;
    }

#endif
    return 0;
}
extern int frag_report(const char *filename, int alignment);
int main(int argc, char* const argv[])
{
    FTS* file_system = NULL;
    FTSENT *node    = NULL;
    int alignment;
    struct statx stx;
    

    if (argc < 3) {
        printf("Usage: %s <path-spec> alignment\n", argv[0]);
        exit(255);
    }

    argv++;
    file_system = fts_open(argv,FTS_COMFOLLOW|FTS_NOCHDIR,&compare);
    argv++;
    alignment = atoi(*argv);
    printf("%s snkkk alignment=%d *argv=%s\n", __func__, alignment, *argv);
    if (NULL != file_system)
    {
        while( (node = fts_read(file_system)) != NULL)
        {
            switch (node->fts_info) 
            {
                case FTS_D :
                case FTS_F :
                case FTS_SL:
                	{
	                	int res = frag_report(node->fts_name, alignment);
	                    indent(node->fts_level);
	                    printf("%s res=%d\n", node->fts_name, res);
	                }
                    break;
                default:
                    break;
            }
        }
        fts_close(file_system);
    }
    return 0;
}

int compare(const FTSENT** one, const FTSENT** two)
{
    return (strcmp((*one)->fts_name, (*two)->fts_name));
}

void indent(int i)
{
    for (; i > 0; i--) 
        printf("    ");
}
