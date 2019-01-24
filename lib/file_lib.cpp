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
#include <sys/stat.h>

#define SYS_RETURN_FD(func, ...) return alloc_virtual_fd(FD_TYPE_SYSTEM, ORIG(func, (__VA_ARGS__)));
#define SYS_PARAM_FD0(func, fd_param) return ORIG(func, (get_real_fd(fd_param)));
#define SYS_PARAM_FD(func, fd_param, ...) return ORIG(func, (get_real_fd(fd_param), __VA_ARGS__));
#define SYS_PARAM_FD2(func, arg, fd_param, ...) return ORIG(func, (arg, get_real_fd(fd_param), __VA_ARGS__));

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
    SYS_RETURN_FD(creat, pathname, mode)
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
    SYS_RETURN_FD(eventfd, initval, flags)
}

// we cannot call dlsym (ORIG) directly for mmap, otherwise will segfault
// the reason: dlsym allocates memory internally, which calls mmap() again
// our workaround: use direct syscall instead
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    return (void *) syscall(SYS_mmap, addr, length, prot, flags, get_real_fd(fd), offset);
}

int faccessat (int __fd, const char *__file, int __type, int __flag)
{
    SYS_PARAM_FD(faccessat, __fd, __file, __type, __flag)
}

__off_t lseek (int __fd, __off_t __offset, int __whence)
{
    SYS_PARAM_FD(lseek, __fd, __offset, __whence)
}

__off64_t lseek64 (int __fd, __off64_t __offset, int __whence)
{
    SYS_PARAM_FD(lseek64, __fd, __offset, __whence)
}

int fchown (int __fd, __uid_t __owner, __gid_t __group)
{
    SYS_PARAM_FD(fchown, __fd, __owner, __group)
}

int fchownat (int __fd, const char *__file, __uid_t __owner, __gid_t __group, int __flag)
{
    SYS_PARAM_FD(fchownat, __fd, __file, __owner, __group, __flag)
}

int fchdir (int __fd)
{
    SYS_PARAM_FD0(fchdir, __fd)
}

int fexecve (int __fd, char *const __argv[], char *const __envp[])
{
    SYS_PARAM_FD(fexecve, __fd, __argv, __envp)
}

long int fpathconf (int __fd, int __name)
{
    SYS_PARAM_FD(fpathconf, __fd, __name)
}

char *ttyname (int __fd)
{
    SYS_PARAM_FD0(ttyname, __fd)
}

int ttyname_r (int __fd, char *__buf, size_t __buflen)
{
    SYS_PARAM_FD(ttyname_r, __fd, __buf, __buflen)
}

int isatty (int __fd)
{
    SYS_PARAM_FD0(isatty, __fd)
}

ssize_t readlinkat (int __fd, const char *__restrict __path, char *__restrict __buf, size_t __len)
{
    SYS_PARAM_FD(readlinkat, __fd, __path, __buf, __len)
}

int unlinkat (int __fd, const char *__name, int __flag)
{
    SYS_PARAM_FD(unlinkat, __fd, __name, __flag)
}

__pid_t tcgetpgrp (int __fd)
{
    SYS_PARAM_FD0(tcgetpgrp, __fd)
}

int tcsetpgrp (int __fd, __pid_t __pgrp_id)
{
    SYS_PARAM_FD(tcsetpgrp, __fd, __pgrp_id)
}

int fsync (int __fd)
{
    SYS_PARAM_FD0(fsync, __fd)
}

int syncfs (int __fd)
{
    SYS_PARAM_FD0(syncfs, __fd)
}

int ftruncate (int __fd, __off_t __length)
{
    SYS_PARAM_FD(ftruncate, __fd, __length)
}

int ftruncate64 (int __fd, __off64_t __length)
{
    SYS_PARAM_FD(ftruncate64, __fd, __length)
}

int lockf (int __fd, int __cmd, __off_t __len)
{
    SYS_PARAM_FD(lockf, __fd, __cmd, __len)
}

int lockf64 (int __fd, int __cmd, __off64_t __len)
{
    SYS_PARAM_FD(lockf64, __fd, __cmd, __len)
}

int fstat (int __fd, struct stat *__buf)
{
    SYS_PARAM_FD(fstat, __fd, __buf)
}

int fstat64 (int __fd, struct stat64 *__buf)
{
    SYS_PARAM_FD(fstat64, __fd, __buf)
}

int fstatat (int __fd, const char *__restrict __file, struct stat *__restrict __buf, int __flag)
{
    SYS_PARAM_FD(fstatat, __fd, __file, __buf, __flag)
}

int fstatat64 (int __fd, const char *__restrict __file, struct stat64 *__restrict __buf, int __flag)
{
    SYS_PARAM_FD(fstatat64, __fd, __file, __buf, __flag)
}

int fchmod (int __fd, __mode_t __mode)
{
    SYS_PARAM_FD(fchmod, __fd, __mode)
}

int fchmodat (int __fd, const char *__file, __mode_t __mode, int __flag)
{
    SYS_PARAM_FD(fchmodat, __fd, __file, __mode, __flag)
}

int mkdirat (int __fd, const char *__path, __mode_t __mode)
{
    SYS_PARAM_FD(mkdirat, __fd, __path, __mode)
}

int mknodat (int __fd, const char *__path, __mode_t __mode, __dev_t __dev)
{
    SYS_PARAM_FD(mknodat, __fd, __path, __mode, __dev)
}

int mkfifoat (int __fd, const char *__path, __mode_t __mode)
{
    SYS_PARAM_FD(mkfifoat, __fd, __path, __mode)
}

int utimensat (int __fd, const char *__path, const struct timespec __times[2], int __flags)
{
    SYS_PARAM_FD(utimensat, __fd, __path, __times, __flags)
}

int futimens (int __fd, const struct timespec __times[2])
{
    SYS_PARAM_FD(futimens, __fd, __times)
}

int __xmknodat (int __ver, int __fd, const char *__path, __mode_t __mode, __dev_t *__dev)
{
    SYS_PARAM_FD2(__xmknodat, __ver, __fd, __path, __mode, __dev)
}

int __fxstat (int __ver, int __fildes, struct stat *__stat_buf)
{
    SYS_PARAM_FD2(__fxstat, __ver, __fildes, __stat_buf)
}

int __fxstatat (int __ver, int __fildes, const char *__filename, struct stat *__stat_buf, int __flag)
{
    SYS_PARAM_FD2(__fxstatat, __ver, __fildes, __filename, __stat_buf, __flag)
}

int __fxstat64 (int __ver, int __fildes, struct stat64 *__stat_buf)
{
    SYS_PARAM_FD2(__fxstat64, __ver, __fildes, __stat_buf)
}

int __fxstatat64 (int __ver, int __fildes, const char *__filename, struct stat64 *__stat_buf, int __flag)
{
    SYS_PARAM_FD2(__fxstatat64, __ver, __fildes, __filename, __stat_buf, __flag)
}
