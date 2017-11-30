#include "../common/helper.h"
#include "sock_monitor.h"
#include "../common/metaqueue.h"
#include "process.h"
#include <unordered_map>
#include <sys/shm.h>

struct Hash4InterBuf
{
    std::size_t operator()(const std::pair<int, int> &key) const
    {
        using std::hash;
        return hash<int>()(key.first) ^ (hash<int>()(key.second) << 16);
    }
};

static std::unordered_map<std::pair<int, int>, interprocess_buf_map_t, Hash4InterBuf> interprocess_buf_idx;
static monitor_sock_node_t ports[65536];
static monitor_sock_adjlist_t listen_adjlist[1000];
static int monitor_adjlist_lowest_available=0;
static int total_adjlist_peer=0;
static int current_q_counter=0;

void listen_handler(metaqueue_element *req_body, metaqueue_element *res_body, int qid)
{
    unsigned short port=req_body->data.sock_listen_command.port;
    if (ports[port].is_listening && !ports[port].is_addrreuse)
    {
        res_body->data.res_error.command=RES_ERROR;
        return;
    }
    if (!ports[port].is_listening) {
        ports[port].is_listening = 1;
        ports[port].adjlist_pointer = -1;
        ports[port].is_addrreuse = req_body->data.sock_listen_command.is_reuseaddr;
    }
    ++total_adjlist_peer;
    listen_adjlist[monitor_adjlist_lowest_available].peer_qid = qid;
    listen_adjlist[monitor_adjlist_lowest_available].peer_fd = 0;
    listen_adjlist[monitor_adjlist_lowest_available].next = ports[port].adjlist_pointer;
    ports[port].current_pointer = monitor_adjlist_lowest_available;
    //seek for next available point
    ++monitor_adjlist_lowest_available;
    if (monitor_adjlist_lowest_available > 1000)
        FATAL("listen buffer used up!");
    res_body->data.command.command = RES_SUCCESS;
}

void connect_handler(metaqueue_element *req_body, metaqueue_element *res_body, int qid)
{
    unsigned short port;
    port = req_body->data.sock_connect_command.port;
    if (!ports[port].is_listening) {
        res_body->data.res_error.command = RES_ERROR;
        return;
    }
    int peer_qid;
    peer_qid = listen_adjlist[ports[port].current_pointer].peer_qid;
    ports[port].current_pointer = listen_adjlist[ports[port].current_pointer].next;
    key_t shm_key;
    int loc;
    if (interprocess_buf_idx.find(std::pair<int, int>(qid, peer_qid)) == interprocess_buf_idx.end())
    {
        if ((shm_key = ftok(SHM_INTERPROCESS_NAME, current_q_counter)) < 0)
            FATAL("Failed to get the key of shared memory, errno: %d", errno);
        ++current_q_counter;
        int shm_id = shmget(shm_key, 2*sizeof(metaqueue_data), IPC_CREAT | 0777);
        if (shm_id == -1)
            FATAL("Failed to open the shared memory, errno: %s", strerror(errno));
        interprocess_buf_idx[std::pair<int, int>(qid, peer_qid)].loc = 0;
        interprocess_buf_idx[std::pair<int, int>(qid, peer_qid)].buffer_key = shm_key;
        interprocess_buf_idx[std::pair<int, int>(peer_qid, qid)].loc = 1;
        interprocess_buf_idx[std::pair<int, int>(peer_qid, qid)].buffer_key = shm_key;
        loc = 0;
    } else
    {
        shm_key = interprocess_buf_idx[std::pair<int, int>(qid, peer_qid)].buffer_key;
        loc = interprocess_buf_idx[std::pair<int, int>(qid, peer_qid)].loc;
    }
    res_body->data.sock_connect_res.shm_key = shm_key;
    res_body->data.sock_connect_res.loc = loc;
    res_body->data.sock_connect_res.command = RES_SUCCESS;
    metaqueue_element res_to_listener;
    res_to_listener.is_valid = 1;
    res_to_listener.data.sock_connect_res.command = RES_NEWCONNECTION;
    res_to_listener.data.sock_connect_res.shm_key = shm_key;
    res_to_listener.data.sock_connect_res.loc = !loc;
    res_to_listener.data.sock_connect_res.fd = req_body->data.sock_connect_command.fd;
    metaqueue_pack q_pack;
    q_pack = process_getrequesthandler_byqid(peer_qid);
    metaqueue_push(q_pack, &res_to_listener);
}