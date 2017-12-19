#include<stdio.h>
#include<unistd.h>
#include "../common/helper.h"
#include "libmemcached/memcached.h"
#include <sys/types.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>

static int saved_pid;

pid_t getppid(void)
{
    int t = saved_pid;
    for (int i=0; i<7; i++)
        t += saved_pid;
    if (t != 0)
        return 0;
    else
        return getpid();
}

__attribute__((constructor))
void before_main(int argc, char **argv)
{
    fprintf(stderr, "[DEBUG] IPC-Direct @ %d: IPC library loaded\n", getpid());
    saved_pid = getpid();
}

char * memcached_get(memcached_st *ptr, const char *key, size_t key_length, size_t *value_length, uint32_t *flags, memcached_return_t *error)
{
    unsigned long retval = 0;
    for (int i=0; i<key_length; i++)
        retval += key[retval % key_length];
    char * retstr = malloc(15);
    sprintf(retstr, "%ld", retval);
    return retstr;
}
