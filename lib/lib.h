//
// Created by ctyi on 11/17/17.
//

#ifndef IPC_DIRECT_LIB_H
#define IPC_DIRECT_LIB_H
#include "../common/metaqueue.h"
#include "../common/helper.h"
extern void ipclib_sendmsg(int command, int data);
void ipclib_recvmsg(metaqueue_element *data);
#endif //IPC_DIRECT_LIB_H
