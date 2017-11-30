//
// Created by ctyi on 11/17/17.
//

#ifndef IPC_DIRECT_SETUP_SOCK_MONITOR_H
#define IPC_DIRECT_SETUP_SOCK_MONITOR_H
#ifdef __cplusplus
extern "C"
{
#endif

#include "../common/helper.h"
#include "../common/setup_sock.h"

extern void setup_sock_monitor_init();

extern int setup_sock_accept(ctl_struc *result);

void setup_sock_send(int fd, ctl_struc *result);

#ifdef __cplusplus
}
#endif
#endif //IPC_DIRECT_SETUP_SOCK_MONITOR_H
