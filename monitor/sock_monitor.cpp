#include "../common/helper.h"
#include "sock_monitor.h"
#include "../common/metaqueue.h"
#include "process.h"
#include <unordered_map>
#include <sys/shm.h>
#include "../common/interprocess_t.h"
#include "../common/darray.hpp"
#include "../common/adjlist_t.hpp"
/*struct Hash4InterBuf
{
    std::size_t operator()(const std::pair<int, int> &key) const
    {
        using std::hash;
        return hash<int>()(key.first) ^ (hash<int>()(key.second) << 16);
    }
};*/
//static std::unordered_map<std::pair<int, int>, interprocess_buf_map_t, Hash4InterBuf> interprocess_buf_idx;
static interprocess_buf_hashtable_t interprocess_buf_idx;
//static monitor_sock_node_t ports[65536];
//darray_t<monitor_sock_adjlist_t, 1000> listen_adjlist;
static adjlist<monitor_sock_node_t, 65536, monitor_sock_adjlist_t, 1000> ports;
static int current_q_counter = 0;
std::unordered_map<key_t, std::pair<int, int>> interprocess_key2tid;
std::unordered_map<key_t, std::pair<int, int>> rdma_key2qid;


static rdma_l2_hash_t rdma_id2key;

const per_proc_map_t * buff_get_handler(pid_t tid)
{
    if (interprocess_buf_idx.find(tid) == interprocess_buf_idx.end())
        return nullptr;
    return &interprocess_buf_idx[tid];
}

key_t buffer_new(pid_t tid_from, pid_t tid_to, int loc)
{
    key_t shm_key;
    shm_key = 9837423+current_q_counter;
    //if ((shm_key = ftok(SHM_INTERPROCESS_NAME, current_q_counter)) < 0)
            // FATAL("Failed to get the key of shared memory, errno: %s", strerror(errno));
    ++current_q_counter;
    int shm_id = shmget(shm_key, interprocess_t::get_sharedmem_size(), IPC_CREAT | 0777);
    if (shm_id == -1)
        FATAL("Failed to open the shared memory, errno: %s", strerror(errno));
    void *baseaddr = shmat(shm_id, NULL, 0);
    if (baseaddr == (void *) -1)
        FATAL("Failed to attach the shared memory, err: %s", strerror(errno));

    interprocess_t::monitor_init(baseaddr);
    interprocess_buf_map_t tmp_key_pair;
    tmp_key_pair.loc = loc;
    tmp_key_pair.buffer_key = shm_key;
    interprocess_buf_idx[tid_from][tid_to].push_back(tmp_key_pair);

    tmp_key_pair.loc = !loc;
    tmp_key_pair.buffer_key = shm_key;
    interprocess_buf_idx[tid_to][tid_from].push_back(tmp_key_pair);

    interprocess_key2tid[shm_key] = std::make_pair(tid_from, tid_to);


    return shm_key;
};

key_t buffer_new_rdma(uint64_t qid, uint64_t rdmaqid, int loc)
{
    key_t shm_key;
    shm_key = 9837423+current_q_counter;
    ++current_q_counter;

    rdma_id2key[qid][rdmaqid].loc = loc;
    rdma_id2key[qid][rdmaqid].buffer_key = shm_key;

    rdma_key2qid[shm_key] = std::make_pair(qid, rdmaqid);

    return shm_key;
}

key_t buffer_enlarge(pid_t tid_from, pid_t tid_to, int loc)
{
    key_t shm_key;
    shm_key = 9837423+current_q_counter;
    ++current_q_counter;
}

void buffer_del(pid_t tid_from, pid_t tid_to)
{
    for(auto const& buffer_key: interprocess_buf_idx[tid_from][tid_to]) {
        interprocess_key2tid.erase(buffer_key.buffer_key);

    }
    interprocess_buf_idx[tid_from].erase(tid_to);
    interprocess_buf_idx[tid_to].erase(tid_from);
}


void sock_monitor_init()
{
    monitor_sock_node_t init_val;
    init_val.is_listening = 0;
    init_val.is_blocking = 1;
    init_val.is_addrreuse = 0;
    ports.init(65536, init_val);

}

#undef DEBUGON
#define DEBUGON 1
void accept_handler(metaqueue_ctl_element *req_body, int qid)
{
    unsigned short port = req_body->req_accept.port;
    if (!ports[port].is_listening)
    {
        DEBUG("WARN: received accept request of port %d, but it is not listening", port);
        return;
    }
    auto iter = ports.begin(port);
    while (!iter.end()) {
        if (iter->peer_qid == qid) {
            DEBUG("accept request received on port %d qid %d", port, qid);
            iter->is_accepting = true;
            return;
        }
    }
    DEBUG("WARN: accept request on port %d: qid %d not found", port, qid);
}

#undef DEBUGON
#define DEBUGON 1
void listen_handler(metaqueue_ctl_element *req_body, metaqueue_ctl_element *res_body, int qid)
{
    unsigned short port = req_body->req_listen.port;
    if (ports[port].is_listening && !ports[port].is_addrreuse)
    {
        res_body->command = RES_ERROR;
        res_body->resp_command.res_code = RES_ERROR;
        res_body->resp_command.err_code = EADDRINUSE;
        DEBUG("received conflicting listen request on non-addrreuse port %d from qid %d", port, qid);
        return;
    }
    if (!ports[port].is_listening)
    {
        ports[port].is_listening = 1;
        ports[port].is_addrreuse = req_body->req_listen.is_reuseaddr;
    }
    monitor_sock_adjlist_t new_listen_fd;
    new_listen_fd.peer_qid=qid;
    new_listen_fd.peer_fd=-1;
    new_listen_fd.is_accepting = false;
    ports.add_element(port, new_listen_fd);
    res_body->resp_command.res_code = RES_SUCCESS;
    res_body->command = RES_SUCCESS;
    DEBUG("qid %d listen on port %d", qid, port);
}

#undef DEBUGON
#define DEBUGON 1
void connect_handler(metaqueue_ctl_element *req_body, metaqueue_ctl_element *res_body, int qid)
{
    unsigned short port;
    port = req_body->req_connect.port;
    bool isRDMA = req_body->req_connect.isRDMA;
    if (!ports[port].is_listening)
    {
        res_body->resp_command.res_code = RES_ERROR;
        res_body->command = RES_ERROR;
        DEBUG("invalid connect request: port %d is not listening", port);
        return;
    }
    int peer_qid;
    auto iter = ports.begin(port);
    monitor_sock_adjlist_t adjdata;
    adjdata.is_accepting = false; // initialize
    // find the first listener that has called accept()
    while (!iter.end()) {
        adjdata = *iter;
        if (adjdata.is_accepting)
            break;
        else
            iter.next();
    }

    if (!adjdata.is_accepting)
    {
        res_body->resp_command.res_code = RES_ERROR;
        res_body->command = RES_ERROR;
        DEBUG("no process on port %d is accepting", port);
        return;
    }

    // set the next iteration to start from the current listener
    // round-robin connect request dispatching
    ports.set_ptr_to(port, iter);
    // try to move to the next listener, if still not empty, set it to be the beginning listener of next connect request
    iter = iter.next();
    if (!iter.end())
        ports.set_ptr_to(port, iter);

    // extract QID data
    peer_qid = adjdata.peer_qid;

    key_t shm_key;
    int loc;

    if (!isRDMA)
    {
        per_proc_map_t *per_proc_map = &interprocess_buf_idx[process_gettid(qid)];
        //if (per_proc_map->find(process_gettid(peer_qid)) == per_proc_map->end()) {
            loc = 0;
            pid_t selftid = process_gettid(qid);
            pid_t peertid = process_gettid(peer_qid);
            shm_key = buffer_new(selftid, peertid, loc);
        //} else {
        //    shm_key = (*per_proc_map)[process_gettid(peer_qid)].buffer_key;
        //    loc = (*per_proc_map)[process_gettid(peer_qid)].loc;
        //}
        DEBUG("new shm connection port %d selftid %d peertid %d", port, selftid, peertid);
    } else
    {
        rdma_l1_hash_t *per_proc_rdma_map = &rdma_id2key[peer_qid];
        //if (per_proc_rdma_map->find(qid) == per_proc_rdma_map->end())
        //{
            loc = 0;
            shm_key = buffer_new_rdma(peer_qid, qid, 0);
        //} else
        //{
        //    shm_key = (*per_proc_rdma_map)[qid].buffer_key;
        //    loc = 0;
        //}
        DEBUG("new RDMA connection port %d qid %d peer_qid %d", port, qid, peer_qid);
    }

    res_body->resp_connect.shm_key = shm_key;
    res_body->resp_connect.loc = loc;
    res_body->command = RES_SUCCESS;
    metaqueue_ctl_element res_to_listener;
    res_to_listener.command = RES_NEWCONNECTION;
    res_to_listener.resp_connect.shm_key = shm_key;
    res_to_listener.resp_connect.loc = !loc;
    res_to_listener.resp_connect.fd = req_body->req_connect.fd;
    res_to_listener.resp_connect.port = port;
    res_to_listener.resp_connect.isRDMA = isRDMA;

    metaqueue_t * q2listener = process_gethandler_byqid(peer_qid);
    q2listener->q[0].push(res_to_listener);
}
#undef DEBUGON
#define DEBUGON 1
void close_handler(metaqueue_ctl_element *req_body, int qid)
{
    unsigned short port=req_body->req_close.port;
    if (!ports[port].is_listening)
        return;
    for (auto iter=ports.begin(port);!iter.end();)
    {
        monitor_sock_adjlist_t listen_elem;
        listen_elem=*iter;
        if (listen_elem.peer_qid==qid)
        {
            DEBUG("Process %d close port %hu", qid, port);
            iter = ports.del_element(iter);
            if (ports.begin(port).end())
            {
                DEBUG("port %d no live processes.", port);
                ports[port].is_listening = 0;
            }
            //this is the first on the adjlist
            break;
        } else
        {
            iter = iter.next();
        }
    }
}
#undef DEBUGON

#define DEBUGON 1
void sock_resource_gc()
{
    for (int i=0;i<=65535;++i)
    {
        if (!ports[i].is_listening) continue;
        int prev_listen_adjlist=-1;
        for (auto iter = ports.begin(i); !iter.end();)
        {
            monitor_sock_adjlist_t curr_listen_proc;
            curr_listen_proc = *iter;
            if (!process_isexist(curr_listen_proc.peer_qid))
            {
                DEBUG("sock GC: port %d qid %d not exists", i, curr_listen_proc.peer_qid);
                iter = ports.del_element(iter);
                if (ports.begin(i).end())
                {
                    DEBUG("sock GC: All process on port %d die", i);
                    ports[i].is_listening = 0;
                }
            } else
            {
                iter = iter.next();
            }
        }

    }
}

#undef DEBUGON
#define DEBUGON 0
