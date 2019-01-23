#include "../common/darray.hpp"
#include "lib.h"
#include "socket_lib.h"
#include "../common/helper.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstdarg>
#include <utility>
#include "lib_internal.h"
#include "../common/metaqueue.h"
#include <sys/ioctl.h>
#include "fork.h"
#include "rdma_lib.h"
#include "../common/rdma_struct.h"
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/epoll.h>

// defined in socket_lib.cpp
// return revents
extern int check_sockfd_receive(int sockfd);

// defined in socket_lib.cpp
// return revents
extern int check_sockfd_send(int sockfd);

static int check_sockfd_event(int sockfd, int events)
{
    int revents = 0;
    if (events & POLLIN)
        revents |= check_sockfd_receive(sockfd);

    if (events & POLLOUT)
        revents |= check_sockfd_send(sockfd);

    return revents;
}

// return revents
static int check_fd_event(int fd, int events)
{
    if (get_fd_type(fd) == FD_TYPE_SYSTEM) { // system fd
        struct pollfd sys_fd;
        sys_fd.fd = get_real_fd(fd);
        sys_fd.events = events;
        sys_fd.revents = 0;
        ORIG(poll, (&sys_fd, 1, 0));
        return sys_fd.revents;
    }
    else { // user space fd
        int sockfd = get_real_fd(fd);
        return check_sockfd_event(sockfd, events);
    }
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    struct timespec begin_time;
    if (timeout > 0) {
        clock_gettime(CLOCK_MONOTONIC, &begin_time);
    }

    int polled_events = 0;
    while (1) {
        for (int i = 0; i < nfds; i ++) {
            fds[i].revents = check_fd_event(fds[i].fd, fds[i].events);
            if (fds[i].revents)
                polled_events ++;
        }

        // if any event is polled, immediately return
        if (polled_events) {
            break;
        }
        else if (timeout == 0) { // if non-blocking poll, immediately return
            break;
        }
        else { // check timeout
            struct timespec end_time;
            clock_gettime(CLOCK_MONOTONIC, &end_time);
            int diff_ms = (end_time.tv_sec - begin_time.tv_sec) * 1000 + (end_time.tv_nsec - begin_time.tv_nsec) / 1e6;
            if (diff_ms >= timeout)
                break;
        }
    }
    return polled_events;
}

int ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *tmo_p, const sigset_t *sigmask)
{
    return poll(fds, nfds, tmo_p->tv_sec * 1000 + tmo_p->tv_nsec / 1e6);
}

static int select_nonblock(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds)
{
    int polled_fds = 0;
    for (int fd = 0; fd < nfds; fd ++) {
        int events = 0;
        if (readfds && FD_ISSET(fd, readfds))
            events |= POLLIN;
        if (writefds && FD_ISSET(fd, writefds))
            events |= POLLOUT;
        if (exceptfds && FD_ISSET(fd, exceptfds))
            events |= POLLERR; // ignored currently

        if (events == 0)
            continue;

        int revents = check_fd_event(fd, events);
        if (readfds && FD_ISSET(fd, readfds))
            if (events & POLLIN)
                polled_fds ++;
            else
                FD_CLR(fd, readfds);

        if (writefds && FD_ISSET(fd, writefds))
            if (events & POLLOUT)
                polled_fds ++;
            else
                FD_CLR(fd, writefds);

        if (exceptfds && FD_ISSET(fd, exceptfds))
            if (events & POLLERR)
                polled_fds ++;
            else
                FD_CLR(fd, exceptfds);
    }
    return polled_fds;
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout)
{
    struct timespec begin_time;
    if (timeout != NULL) {
        clock_gettime(CLOCK_MONOTONIC, &begin_time);
    }

    int polled_events = 0;
    while (1) {
        polled_events = select_nonblock(nfds, readfds, writefds, exceptfds);

        if (polled_events) {
            break;
        }
        else if (timeout == NULL) { // if non-blocking poll, immediately return
            break;
        }
        else { // check timeout
            struct timespec end_time;
            clock_gettime(CLOCK_MONOTONIC, &end_time);
            int diff_ms = (end_time.tv_sec - begin_time.tv_sec) * 1000 + (end_time.tv_nsec - begin_time.tv_nsec) / 1e6;
            if (diff_ms >= timeout->tv_sec * 1000 + timeout->tv_usec / 1e3)
                break;
        }
    }
    return polled_events;
}

int pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, const struct timespec *timeout, const sigset_t *sigmask)
{
    struct timeval tmo;
    if (timeout != NULL) {
        tmo.tv_sec = timeout->tv_sec;
        tmo.tv_usec = timeout->tv_nsec / 1000;
    }
    else {
        tmo.tv_sec = 0;
        tmo.tv_usec = 0;
    }
    return select(nfds, readfds, writefds, exceptfds, &tmo);
}

typedef std::unordered_map<int,struct epoll_event> epoll_fd_t;

static std::unordered_map<int,epoll_fd_t> epoll_fds;

int epoll_create(int size)
{
    if (size <= 0) {
        errno = EINVAL;
        return -1;
    }
    // epoll fd does not need "real FD", so use 0 instead
    int fd = alloc_virtual_fd(FD_TYPE_EPOLL, 0);
    epoll_fd_t empty_epoll_fd;
    epoll_fds[fd] = empty_epoll_fd;
    return fd;
}

void epoll_remove(int fd)
{
    epoll_fds.erase(fd);
    delete_virtual_fd(fd);
}

int epoll_create1(int flags)
{
    if (flags < 0) {
        errno = EINVAL;
        return -1;
    }
    return epoll_create(0);
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
    if (epoll_fds.find(epfd) == epoll_fds.end()) {
        errno = EBADF;
        return -1;
    }
    if (op == EPOLL_CTL_ADD || op == EPOLL_CTL_MOD) {
        epoll_fds[epfd][fd] = *event;
    }
    else if (op == EPOLL_CTL_DEL) {
        epoll_fds[epfd].erase(fd);
    }
    return 0;
}

static int epoll_wait_nonblock(int epfd, struct epoll_event *events, int maxevents)
{
    if (epoll_fds.find(epfd) == epoll_fds.end()) {
        errno = EBADF;
        return -1;
    }

    int return_events = 0;
    for (auto it = epoll_fds[epfd].begin(); it != epoll_fds[epfd].end(); it ++) {
        int fd = it->first;
        uint32_t epoll_events = it->second.events;
        int poll_events = 0;
        if (epoll_events & EPOLLIN)
            poll_events |= POLLIN;
        if (epoll_events & EPOLLOUT)
            poll_events |= POLLOUT;
        if (epoll_events & EPOLLERR)
            poll_events |= POLLERR; // ignored for now
        if (epoll_events & EPOLLET) {
            errno = ENOTSUP; // EPOLLET not supported yet
            return -1;
        }

        int poll_revents = check_fd_event(fd, poll_events);
        if (poll_revents) {
            uint32_t epoll_revents = 0;
            if (poll_revents & POLLIN)
                epoll_revents |= EPOLLIN;
            if (poll_revents & POLLOUT)
                epoll_revents |= EPOLLOUT;
            if (poll_revents & POLLERR)
                epoll_revents |= EPOLLERR;
            // EPOLLHUP etc. not supported yet

            events[return_events].events = epoll_revents;
            events[return_events].data = it->second.data;
            return_events ++;
            if (return_events >= maxevents)
                break;
        }
    }
    return return_events;
}

int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)
{
    struct timespec begin_time;
    if (timeout > 0) {
        clock_gettime(CLOCK_MONOTONIC, &begin_time);
    }

    int polled_events = 0;
    while (1) {
        polled_events = epoll_wait_nonblock(epfd, events, maxevents);

        if (polled_events) {
            break;
        }
        else if (timeout == 0) { // if non-blocking poll, immediately return
            break;
        }
        else { // check timeout
            struct timespec end_time;
            clock_gettime(CLOCK_MONOTONIC, &end_time);
            int diff_ms = (end_time.tv_sec - begin_time.tv_sec) * 1000 + (end_time.tv_nsec - begin_time.tv_nsec) / 1e6;
            if (diff_ms >= timeout)
                break;
        }
    }
    return polled_events;
}

int epoll_pwait(int epfd, struct epoll_event *events, int maxevents, int timeout, const sigset_t *sigmask)
{
    return epoll_wait(epfd, events, maxevents, timeout);
}
