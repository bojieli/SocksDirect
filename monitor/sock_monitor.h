#ifndef IPC_DIRECT_SOCK_MONITOR_H
#define IPC_DIRECT_SOCK_MONITOR_H

#include "../common/metaqueue.h"

typedef struct
{
    int peer_fd;
    int peer_qid;
} monitor_sock_adjlist_t;
typedef struct
{
    int is_listening;
    int is_blocking;
    int is_addrreuse;
} monitor_sock_node_t;
typedef struct
{
    key_t buffer_key;
    int loc;
} interprocess_buf_map_t;
#ifdef __cplusplus
extern "C"
{
#endif
extern void listen_handler(metaqueue_ctl_element *req_body, metaqueue_ctl_element *res_body, int qid);
extern void connect_handler(metaqueue_ctl_element *req_body, metaqueue_ctl_element *res_body, int qid);
extern void close_handler(metaqueue_ctl_element *req_body, int qid);
extern void sock_monitor_init();
void sock_resource_gc();
#ifdef __cplusplus
}
#endif

#endif
