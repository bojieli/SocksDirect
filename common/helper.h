//
// Created by ctyi on 11/10/17.
//

#ifndef PROJECT_KVDIRECT_H
#define PROJECT_KVDIRECT_H
//something useful for DEBUG
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <string.h>
#include <pthread.h>
//#include <sys/socket.h>
//#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <dlfcn.h>
#define DEBUGON 0
#define DEBUG(fmt, ...) (DEBUGON && fprintf(stderr, "[DEBUG] IPC-Direct @%5d, Line%d: " fmt "\n", getpid(), __LINE__, ##__VA_ARGS__))
#define ERROR(fmt, ...) (fprintf(stderr, "[ERROR] IPC-Direct @%5d, Line %d: " fmt "\n", getpid(), __LINE__, ##__VA_ARGS__))
#define FATAL(fmt, ...) (fprintf(stderr, "[FATAL] IPC-Direct @%5d, Line %d: " fmt "\n", getpid(), __LINE__, ##__VA_ARGS__), abort())
#define SW_BARRIER asm volatile("" ::: "memory")
#define ORIG(func, args) ((typeof(&func)) dlsym(RTLD_NEXT, #func)) args

//configuration
#define PID_LOC "/dev/shm/ipcd.pid"
#define ALLQ_NUM 4
extern pid_t gettid();
extern int pin_thread(int core);
#endif //PROJECT_KVDIRECT_H
