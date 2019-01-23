//
// Created by ctyi on 11/20/17.
//

#include "../common/helper.h"
#include "setup_sock_lib.h"
#include "../common/setup_sock.h"
#include <sys/socket.h>
#include <sys/un.h>

#undef DEBUGON
#define DEBUGON 0

void setup_sock_connect(ctl_struc *res)
{
    int fd;
    fd = ORIG(socket, (AF_UNIX, SOCK_STREAM, 0));
    if (fd == -1)
        FATAL("Failted to create client setup socket, %s", strerror(errno));
    struct sockaddr_un local;
    local.sun_family = AF_UNIX;
    strcpy(local.sun_path, SOCK_FILENAME);
    int len = strlen(local.sun_path) + sizeof(local.sun_family);
    if (ORIG(connect, (fd, (struct sockaddr *) &local, len)) == -1)
        FATAL("Failed to open the connection to local monitor (check if the monitor is running): %s", strerror(errno));
    ctl_struc data;
    data.tid = gettid();
    data.pid = getpid();
    ORIG(send, (fd, (void *) &data, sizeof(ctl_struc), 0));
    DEBUG("Connect req sent");
    ORIG(recv, (fd, (void *) &data, sizeof(ctl_struc), MSG_WAITALL));
    DEBUG("Recv received");
    *res = data;
}

#undef DEBUGON
#define DEBUGON 0
