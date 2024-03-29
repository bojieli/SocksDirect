#ifndef IPC_DIRECT_SOCK_MONITOR_H
#define IPC_DIRECT_SOCK_MONITOR_H
#include <unordered_map>
#include <vector>
#include "../common/metaqueue.h"

typedef struct
{
    int peer_fd;
    int peer_qid;
    bool is_accepting;
} monitor_sock_adjlist_t;

//This two structure map (tid,tid)->shmem
typedef struct
{
    int is_listening;
    int is_blocking;
    int is_addrreuse;
} monitor_sock_node_t;
typedef
struct
{
    key_t buffer_key;
    int loc;
} interprocess_buf_map_t;
typedef std::vector<interprocess_buf_map_t> interp_buf_vec_t;
typedef std::unordered_map<int, interp_buf_vec_t> per_proc_map_t;
typedef std::unordered_map<int, per_proc_map_t> interprocess_buf_hashtable_t;
extern const per_proc_map_t * buff_get_handler(pid_t tid);
extern key_t buffer_new(pid_t tid_from, pid_t tid_to, int loc);
extern void buffer_del(pid_t tid_from, pid_t tid_to);
//This structure map key->(tid,tid)
extern std::unordered_map<key_t, std::pair<int, int>> interprocess_key2tid;

//Map (qid, rdmaqid)->shmem
typedef std::unordered_map <uint64_t, interprocess_buf_map_t> rdma_l1_hash_t;
typedef std::unordered_map <uint64_t , rdma_l1_hash_t> rdma_l2_hash_t;
extern std::unordered_map<key_t, std::pair<int, int>> rdma_key2qid;



#ifdef __cplusplus
extern "C"
{
#endif
extern void listen_handler(metaqueue_ctl_element *req_body, metaqueue_ctl_element *res_body, int qid);
extern void accept_handler(metaqueue_ctl_element *req_body, int qid);
extern void connect_handler(metaqueue_ctl_element *req_body, metaqueue_ctl_element *res_body, int qid);
extern void close_handler(metaqueue_ctl_element *req_body, int qid);
extern void sock_monitor_init();
void sock_resource_gc();
#ifdef __cplusplus
}
#endif

#endif
