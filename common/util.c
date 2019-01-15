#if __STDC_VERSION__ >= 199901L
#define _XOPEN_SOURCE 600
#else
#define _XOPEN_SOURCE 500
#endif /* __STDC_VERSION__ */
#include <sys/sysinfo.h>
#include <time.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define USER_HZ 100

int get_cpu_utilization(double *user_util, double *kernel_util)
{
    static int initialized = 0;
    static long last_user = 0, last_kernel = 0;
    long curr_user, curr_nice, curr_kernel;
    static struct timespec last_time;
    static struct timespec curr_time;

    FILE *fp = fopen("/proc/stat", "r");
    if (!fp)
        return EFAULT;
    char cpu[256];
    fscanf(fp, "%s %ld%ld%ld", cpu, &curr_user, &curr_nice, &curr_kernel);
    if (strcmp(cpu, "cpu") != 0)
        return EFAULT;
    fclose(fp);

    clock_gettime(CLOCK_REALTIME, &curr_time);
    double jiffies = USER_HZ * (curr_time.tv_sec - last_time.tv_sec + (curr_time.tv_nsec - last_time.tv_nsec) / 1.0e9);
    last_time = curr_time;

    if (user_util && initialized)
        *user_util = (curr_user - last_user) / jiffies;
    last_user = curr_user;

    if (kernel_util && initialized)
        *kernel_util = (curr_kernel - last_kernel) / jiffies;
    last_kernel = curr_kernel;

    if (!initialized) {
        initialized = 1;
        return EAGAIN;
    }
    return 0;
}
