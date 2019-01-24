#include <stdarg.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <poll.h>
#include "lib.h"
#include "socket_lib.h"
#include "../common/helper.h"

int open(const char *pathname, int flags, ...)
{
    va_list p_args;
    va_start(p_args, flags);
    int mode = va_arg(p_args, int);
    return alloc_virtual_fd(FD_TYPE_SYSTEM, ORIG(open, (pathname, flags, mode)));
}

int open64(const char *pathname, int flags, ...)
{
    va_list p_args;
    va_start(p_args, flags);
    int mode = va_arg(p_args, int);
    return alloc_virtual_fd(FD_TYPE_SYSTEM, ORIG(open64, (pathname, flags, mode)));
}

int creat(const char *pathname, mode_t mode)
{
    return alloc_virtual_fd(FD_TYPE_SYSTEM, ORIG(creat, (pathname, mode)));
}

int openat(int dirfd, const char *pathname, int flags, ...)
{
    va_list p_args;
    va_start(p_args, flags);
    int mode = va_arg(p_args, int);
    return alloc_virtual_fd(FD_TYPE_SYSTEM, ORIG(openat, (dirfd, pathname, flags, mode)));
}

int openat64(int dirfd, const char *pathname, int flags, ...)
{
    va_list p_args;
    va_start(p_args, flags);
    int mode = va_arg(p_args, int);
    return alloc_virtual_fd(FD_TYPE_SYSTEM, ORIG(openat64, (dirfd, pathname, flags, mode)));
}

int eventfd(unsigned int initval, int flags)
{
    return alloc_virtual_fd(FD_TYPE_SYSTEM, ORIG(eventfd, (initval, flags)));
}

// we cannot call dlsym (ORIG) directly for mmap, otherwise will segfault
// the reason: dlsym allocates memory internally, which calls mmap() again
// our workaround: use direct syscall instead
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    return (void *) syscall(SYS_mmap, addr, length, prot, flags, get_real_fd(fd), offset);
}


