//
// Created by ctyi on 11/24/17.
//

#ifndef IPC_DIRECT_LIB_INTERNAL_H
#define IPC_DIRECT_LIB_INTERNAL_H
#include "../common/metaqueue.h"
typedef struct {
    int uniq_shared_id;
    void* uniq_shared_base_addr;
    //0 is to monitor 1 is from monitor
    metaqueue_data* metaqueue;
    metaqueue_meta_t metaqueue_metadata[2];
    file_struc_t fds[MAX_FD_OWN_NUM];
    fd_list_t adjlist[MAX_FD_PEER_NUM];
    int fd_own_num;
    int fd_peer_num;
    int fd_own_lowest_id;
    int fd_peer_lowest_id;
} thread_data_t;
extern pthread_key_t pthread_key;
#endif //IPC_DIRECT_LIB_INTERNAL_H
