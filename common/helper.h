//
// Created by ctyi on 11/10/17.
//

#ifndef PROJECT_IPCDIRECT_H
#define PROJECT_IPCDIRECT_H
//something useful for DEBUG
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <string.h>
#include <pthread.h>

#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <dlfcn.h>

#define DEBUGON 0
#define DEBUG(fmt, ...) (DEBUGON && fprintf(stderr, "[DEBUG] IPC-Direct @%5d, %s Line%d: " fmt "\n", getpid(), __FILE__, __LINE__, ##__VA_ARGS__))
#define ERROR(fmt, ...) (fprintf(stderr, "[ERROR] IPC-Direct @%5d, %s Line %d: " fmt "\n", getpid(), __FILE__, __LINE__, ##__VA_ARGS__))
#define FATAL(fmt, ...) (fprintf(stderr, "[FATAL] IPC-Direct @%5d, %s Line %d: " fmt "\n", getpid(), __FILE__, __LINE__, ##__VA_ARGS__), abort())
#define SW_BARRIER asm volatile("" ::: "memory")
#define ORIG(func, args) ((typeof(&func)) dlsym(RTLD_NEXT, #func)) args

enum
{
    REQ_NOP,
    REQ_THRTEST,
    REQ_THRTEST_INIT,
    REQ_PING,
    REQ_CLOSE,
    REQ_LISTEN,
    REQ_CONNECT,
    RES_ERROR,
    RES_SUCCESS,
    RES_NEWCONNECTION,
    REQ_FORK,
    RES_FORK,
    RES_PUSH_FORK
};

#define FD_STATES_CLOSE_FLG_NRD_REQ 0x1
#define FD_STATES_CLOSE_FLG_NWR_REQ 0x2
#define FD_STATES_CLOSE_FLG_NRD_RES 0x4
#define FD_STATES_CLOSE_FLG_NWR_RES 0x8
#define FD_STATES_FORKPAUSED 0x16

//configuration
#define SHM_NAME "/ipcd_shmem"
#define SHM_INTERPROCESS_NAME "/ipcd_interprocess_shmem"
#define SOCK_FILENAME "/tmp/ipcd.sock"
#ifdef __cplusplus
extern "C"
{
#endif

extern pid_t gettid();

extern int pin_thread(int core);

#ifdef __cplusplus
}
#endif
#endif //PROJECT_IPCDIRECT_H
