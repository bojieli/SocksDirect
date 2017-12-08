//
// Created by ctyi on 11/24/17.
//

#ifndef IPC_DIRECT_LIB_INTERNAL_H
#define IPC_DIRECT_LIB_INTERNAL_H

#include "../common/metaqueue.h"
#include "socket_lib.h"
#include "../common/darray.hpp"

typedef darray_t<file_struc_t, MAX_FD_OWN_NUM> d_file_struc_t;
typedef darray_t<fd_list_t, MAX_FD_PEER_NUM> d_fd_list_t;

typedef struct
{
    int uniq_shared_id;
    void *uniq_shared_base_addr;
    //0 is to monitor 1 is from monitor
    metaqueue_data *metaqueue;
    metaqueue_meta_t metaqueue_metadata[2];
    d_file_struc_t fds;
    d_fd_list_t adjlist;
} thread_data_t;


extern pthread_key_t pthread_key;

#define GET_THREAD_DATA() (reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key)))
#define GET_THREAD_SOCK_DATA() (reinterpret_cast<thread_sock_data_t *>(pthread_getspecific(pthread_sock_key)))

#endif //IPC_DIRECT_LIB_INTERNAL_H
