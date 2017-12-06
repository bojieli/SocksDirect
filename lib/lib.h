//
// Created by ctyi on 11/17/17.
//

#ifndef IPC_DIRECT_LIB_H
#define IPC_DIRECT_LIB_H
#ifdef __cplusplus
extern "C"
{
#endif

#include <sys/socket.h>
#include "../common/metaqueue.h"
#include "../common/helper.h"


extern void ipclib_sendmsg(int command, int data);

void ipclib_recvmsg(metaqueue_element *data);

extern pid_t fork();

extern int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*start_routine)(void *), void *arg);

extern int socket(int domain, int type, int protocol) __THROW;

int bind(int socket, const struct sockaddr *address, socklen_t address_len) __THROW;

extern int listen(int socket, int backlog) __THROW;

extern int close(int fildes);

extern int connect(int socket, const struct sockaddr *address, socklen_t address_len);

extern int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags);

extern int setsockopt(int socket, int level, int option_name, const void *option_value, socklen_t option_len);

extern int ioctl(int fildes, unsigned long request, ...) __THROW;
extern ssize_t writev(int fd, const struct iovec *iov, int iovcnt);


#ifdef __cplusplus
}
#endif
#endif //IPC_DIRECT_LIB_H
