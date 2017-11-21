//
// Created by ctyi on 11/14/17.
//

#ifndef IPC_DIRECT_ATTACHQUEUE_LIB_H
#define IPC_DIRECT_ATTACHQUEUE_LIB_H
extern void attachqueue_init();
extern ctl_struc attachqueue_pullack(pid_t pid);
extern ctl_struc attachqueue_pushreq(ctl_struc data);
#endif //IPC_DIRECT_ATTACHQUEUE_LIB_H
