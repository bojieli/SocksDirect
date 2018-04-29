
//some helper function
#include "helper.h"

#define LOOP_CYCLE 100000000
#define CAL_CYCLE 100000000
volatile int tmp;

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

/* assembly code to read the TSC */
static inline uint64_t RDTSC()
{
    unsigned int hi, lo;
    __asm__ volatile("rdtsc" : "=a" (lo), "=d" (hi));
    return ((uint64_t) hi << 32) | lo;
}

const int NANO_SECONDS_IN_SEC = 1000000000;

/* returns a static buffer of struct timespec with the time difference of ts1 and ts2
   ts1 is assumed to be greater than ts2 */
struct timespec *TimeSpecDiff(struct timespec *ts1, struct timespec *ts2)
{
    static struct timespec ts;
    ts.tv_sec = ts1->tv_sec - ts2->tv_sec;
    ts.tv_nsec = ts1->tv_nsec - ts2->tv_nsec;
    if (ts.tv_nsec < 0)
    {
        ts.tv_sec--;
        ts.tv_nsec += NANO_SECONDS_IN_SEC;
    }
    return &ts;
}

static pthread_key_t tick_key;
static pthread_key_t timing_key;


static void CalibrateTicks()
{
    struct timespec begints, endts;
    for (int i = 0; i < LOOP_CYCLE; ++i)
    {
        __sync_synchronize();
        tmp = i;
    }
    uint64_t begin = 0, end = 0;
    clock_gettime(CLOCK_MONOTONIC, &begints);
    begin = RDTSC();
    for (int i = 0; i < CAL_CYCLE; ++i)
    {
        __sync_synchronize();
        tmp = i;
    }
    end = RDTSC();
    clock_gettime(CLOCK_MONOTONIC, &endts);
    struct timespec *tmpts = TimeSpecDiff(&endts, &begints);
    uint64_t nsecElapsed = tmpts->tv_sec * 1000000000LL + tmpts->tv_nsec;
    double *g_TicksPerNanoSec=malloc(sizeof(double));
    *g_TicksPerNanoSec = (double) (end - begin) / (double) nsecElapsed;
    pthread_setspecific(tick_key, g_TicksPerNanoSec);
}

/* Call once before using RDTSC, has side effect of binding process to CPU1 */
void InitRdtsc()
{
    CalibrateTicks();
    printf("Calibrated!\n");
}

void GetTimeSpec(struct timespec *ts, uint64_t nsecs)
{
    ts->tv_sec = nsecs / NANO_SECONDS_IN_SEC;
    ts->tv_nsec = nsecs % NANO_SECONDS_IN_SEC;
}

/* ts will be filled with time converted from TSC reading */
void GetRdtscTime(struct timespec *ts)
{
    GetTimeSpec(ts, RDTSC() / *((double *)pthread_getspecific(tick_key)));
}

void TimingInit()
{
    pthread_key_create(&tick_key,NULL);
    pthread_key_create(&timing_key,NULL);
}

void TimingBegin()
{
    struct timespec *begin = pthread_getspecific(timing_key);
    if (begin == NULL) {
        begin = malloc(sizeof(struct timespec));
        pthread_setspecific(timing_key, begin);
    }
    GetRdtscTime(begin);
}

unsigned long TimingEnd()
{
    struct timespec end;
    GetRdtscTime(&end);
    struct timespec *begin = pthread_getspecific(timing_key);
    if (begin == NULL)
        return 0;
    unsigned long ns = (unsigned long)((end.tv_sec - begin->tv_sec) * 1000000000LL + (end.tv_nsec - begin->tv_nsec));
    free(begin);
    return ns;
}
