//
// Created by ctyi on 11/24/17.
//

#ifndef IPC_DIRECT_LIB_INTERNAL_H
#define IPC_DIRECT_LIB_INTERNAL_H

#include "../common/metaqueue.h"
#include "socket_lib.h"
#include "../common/darray.hpp"
#include "../common/adjlist_t.hpp"

typedef struct
{
    int uniq_shared_id;
    void *uniq_shared_base_addr;
    //0 is to monitor 1 is from monitor
    metaqueue_t metaqueue;
    uint64_t token;
    uint64_t old_token; //store the old token before fork
    adjlist<file_struc_rd_t, MAX_FD_OWN_NUM, fd_rd_list_t, MAX_FD_PEER_NUM> fds_datawithrd;
    adjlist<int, MAX_FD_OWN_NUM, fd_wr_list_t, MAX_FD_PEER_NUM> fds_wr;
    darray_t<fd_rd_list_t, MAX_FD_PEER_NUM> rd_tree;
    //d_file_struc_t fds;
    //d_fd_list_t adjlist;
} thread_data_t;


extern pthread_key_t pthread_key;

#define GET_THREAD_DATA() (reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key)))
#define GET_THREAD_SOCK_DATA() (reinterpret_cast<thread_sock_data_t *>(pthread_getspecific(pthread_sock_key)))
extern adjlist<file_struc_rd_t, MAX_FD_OWN_NUM, fd_rd_list_t, MAX_FD_PEER_NUM>::iterator recv_empty_hook
        (
                adjlist<file_struc_rd_t, MAX_FD_OWN_NUM, fd_rd_list_t, MAX_FD_PEER_NUM>::iterator iter,
                int myfd
        );

// free page management for copy on write
static void alloc_page_pool(int num_pages)
{
}

static void enqueue_free_page(int page)
{
}

static void dequeue_free_page(int page)
{
}

#endif //IPC_DIRECT_LIB_INTERNAL_H
