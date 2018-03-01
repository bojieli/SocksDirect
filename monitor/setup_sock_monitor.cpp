//
// Created by ctyi on 11/17/17.
//
#define _GNU_SOURCE

#include "setup_sock_monitor.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "../common/helper.h"
#include <errno.h>
#include <string.h>
#include <unistd.h>

static int socket_fd;

void setup_sock_monitor_init()
{
    socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (socket_fd == -1)
        FATAL("Cannot create socket for monitor, err: %s", strerror(errno));
    unlink(SOCK_FILENAME);
    struct sockaddr_un local;
    local.sun_family = AF_UNIX;
    strcpy(local.sun_path, SOCK_FILENAME);
    int len = strlen(local.sun_path) + sizeof(local.sun_family);
    if (bind(socket_fd, (struct sockaddr *) &local, len) == -1)
        FATAL("Failed to bind for monitor, err: %s", strerror(errno));
    if (listen(socket_fd, 10000) == -1)
        FATAL("Failed to listen for monitor, err: %s", strerror(errno));
    DEBUG("Connection socket established");
}

int setup_sock_accept(ctl_struc *result)
{
    int peer_fd;
    if ((peer_fd = accept(socket_fd, NULL, NULL)) == -1)
    {
        if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) return -1;
        else
            FATAL("Failed to accept a new attach request");
    }
    unsigned int len_recvd = 0;
    uint8_t *pointer = (uint8_t *)result;
    while (len_recvd < sizeof(ctl_struc))
    {
        ssize_t bytes = recv(peer_fd, pointer, sizeof(ctl_struc), MSG_WAITALL);
        pointer += bytes;
        len_recvd += bytes;
    }
    return peer_fd;
}

void setup_sock_send(int fd, ctl_struc *result)
{
    int ret = send(fd, (void *) result, sizeof(ctl_struc), 0);
    if (ret == -1)
        FATAL("Fail to send ack, %s", strerror(errno));
    shutdown(fd, 2);
}