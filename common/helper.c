
//some helper function
#include "helper.h"

pid_t gettid()
{
    return (pid_t) syscall(SYS_gettid);
}

int pin_thread(int core)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
}