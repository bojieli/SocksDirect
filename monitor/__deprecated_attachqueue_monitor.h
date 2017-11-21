//
// Created by ctyi on 11/10/17.
//

#ifndef IPC_DIRECT_ATTACHQUEUE_MONITOR_H
#define IPC_DIRECT_ATTACHQUEUE_MONITOR_H
#include "../common/__deprecated_attachqueue.h"
extern void attachqueue_sysinit();
extern int attachqueue_isempty();
extern void attachqueue_pullreq(ctl_struc *result);
extern void attachqueue_pushack(ctl_struc *result);

#endif //IPC_DIRECT_ATTACHQUEUE_MONITOR_H
