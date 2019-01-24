//
// Created by ctyi on 11/23/17.
//

#include "../common/darray.hpp"
#include "lib.h"
#include "socket_lib.h"
#include "../common/helper.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstdarg>
#include <utility>
#include "lib_internal.h"
#include "../common/metaqueue.h"
#include <sys/ioctl.h>
#include "fork.h"
#include "rdma_lib.h"
#include "../common/rdma_struct.h"
#include <sys/eventfd.h>
#include <poll.h>
#include <mutex>

pthread_key_t pthread_sock_key;


#undef DEBUGON
#define DEBUGON 0

// fd remapping should be used after initialized
static bool fd_remap_initialized = false;
// from virtual fd to type and real fd
static std::vector<fd_remap_entry_t> fd_remap_table;
// from type and real fd to virtual fd
static std::vector<std::vector<int>> fd_reverse_map_table;
// current max virtual fd (for allocation)
static int max_virtual_fd;
// deleted virtual fds are to be recycled (the vector is actually a stack)
static std::vector<int> deleted_virtual_fds;
// mutex and locks for concurrent update
static std::mutex fd_remap_table_mutex;
static std::mutex deleted_virtual_fds_mutex;

fd_type_t get_fd_type(int fd)
{
    if (!fd_remap_initialized)
        return FD_TYPE_SYSTEM;
    else if (fd < 0) // error
        return FD_TYPE_UNKNOWN;
    else if (fd >= fd_remap_table.size())
        return FD_TYPE_UNKNOWN;
    else
        return fd_remap_table[fd].type;
}

int get_real_fd(int virtual_fd)
{
    if (virtual_fd < 0 || !fd_remap_initialized) // error pass through
        return virtual_fd;
    else if (virtual_fd >= fd_remap_table.size())
        return -1;
    else
        return fd_remap_table[virtual_fd].real_fd;
}

int get_virtual_fd(fd_type_t type, int real_fd)
{
    if (real_fd < 0) // error pass through
        return real_fd;
    if (real_fd >= fd_reverse_map_table[type].size())
        return -1;
    else
        return fd_reverse_map_table[type][real_fd];
}

void set_fd_type(int virtual_fd, fd_type_t type, int real_fd)
{
    DEBUG("set_fd_type virtual fd %d type %d real fd %d", virtual_fd, type, real_fd);

    assert(virtual_fd >= 0);
    assert(real_fd >= 0);

    // if overflow, resize
    if (virtual_fd >= fd_remap_table.size()) {
        std::lock_guard<std::mutex> lck (fd_remap_table_mutex);

        int old_size = fd_remap_table.size();
        int new_size = virtual_fd * 2;
        if (new_size < INIT_FD_REMAP_TABLE_SIZE)
            new_size = INIT_FD_REMAP_TABLE_SIZE;

        // resize virtual_fd -> type, real_fd mapping table
        fd_remap_table.resize(new_size);
        // initialize added entries
        for (int i = old_size; i < new_size; i ++) {
            fd_remap_table[i].type = FD_TYPE_UNKNOWN;
            // default map to self
            fd_remap_table[i].real_fd = -1;
        }

        // resize type, real_fd -> virtual_fd mapping table
        for (int i = 0; i < NUM_FD_TYPES; i ++) {
            int old_size = fd_reverse_map_table[i].size();
            fd_reverse_map_table[i].resize(new_size);
            // default map to self
            for (int j = old_size; j < new_size; j ++)
                fd_reverse_map_table[i][j] = -1;
        }
    }
    // add mapping
    fd_remap_table[virtual_fd].type = type;
    fd_remap_table[virtual_fd].real_fd = real_fd;
    // add reverse mapping
    if (type != FD_TYPE_UNKNOWN)
        fd_reverse_map_table[type][real_fd] = virtual_fd;
    // update max value
    if (virtual_fd > max_virtual_fd)
        max_virtual_fd = virtual_fd;
}

static int alloc_unmapped_virtual_fd()
{
    std::lock_guard<std::mutex> lck (deleted_virtual_fds_mutex);
    if (deleted_virtual_fds.empty()) {
        return (++ max_virtual_fd);
    }
    else {
        int virtual_fd = deleted_virtual_fds.back();
        deleted_virtual_fds.pop_back();
        return virtual_fd;
    }
}

#undef DEBUGON
#define DEBUGON 0
int alloc_virtual_fd(fd_type_t type, int real_fd)
{
    if (real_fd < 0 || !fd_remap_initialized) // error pass through
        return real_fd;
    int virtual_fd = alloc_unmapped_virtual_fd();
    set_fd_type(virtual_fd, type, real_fd);
    DEBUG("thread %d alloc virtual fd %d type %d real %d", gettid(), virtual_fd, type, real_fd);
    return virtual_fd;
}

void delete_virtual_fd(int virtual_fd)
{
    assert(virtual_fd >= 0);
    DEBUG("thread %d delete virtual fd %d", gettid(), virtual_fd);

    std::lock_guard<std::mutex> lck (deleted_virtual_fds_mutex);
    deleted_virtual_fds.push_back(virtual_fd);
}


#undef DEBUGON
#define DEBUGON 1
void res_push_fork_handler(metaqueue_ctl_element ele)
{
    key_t old_shmem_key, n_shmem_key[2];
    old_shmem_key = ele.push_fork.oldshmemkey;
    n_shmem_key[0] = ele.push_fork.newshmemkey[0];
    n_shmem_key[1] = ele.push_fork.newshmemkey[1];
    thread_data_t *thread_data = GET_THREAD_DATA();
    thread_sock_data_t *thread_sock_data = GET_THREAD_SOCK_DATA();
    int old_buffer_id = (*(thread_sock_data->bufferhash))[old_shmem_key];
    int loc = thread_sock_data->buffer[old_buffer_id].loc;
    int new_buffer_id[2];
    //add new buffer to the buffer list and change the hashmap
    new_buffer_id[0] = thread_sock_data->newbuffer(n_shmem_key[0], loc);
    new_buffer_id[1] = thread_sock_data->newbuffer(n_shmem_key[1], loc);

    //process all the write request first (writer, no fork side)
    for (int fd = thread_data->fds_wr.hiter_begin();
         fd!=-1;
         fd = thread_data->fds_wr.hiter_next(fd))
    {
        for (auto iter = thread_data->fds_wr.begin(fd); !iter.end();iter = iter.next())
        {
            if (iter->buffer_idx == old_buffer_id)
            {
                iter->status = FD_STATUS_Q_ISOLATED;
                DEBUG("fd %d buffer id %d peer forked and add two new queue to adjlist", fd, old_buffer_id);
                fd_wr_list_t new_wr_adj_ele[2];
                new_wr_adj_ele[0].status = new_wr_adj_ele[1].status = 0;
                new_wr_adj_ele[0].buffer_idx = new_buffer_id[0];
                new_wr_adj_ele[1].buffer_idx = new_buffer_id[1];
                iter = thread_data->fds_wr.add_element_at(iter, fd, new_wr_adj_ele[0]);
                iter = thread_data->fds_wr.add_element_at(iter, fd, new_wr_adj_ele[1]);
            }
        }

    }
    
    //read side, itself not fork
    //connect the new buffer to the tree
    
    //first iterate all the fd
    for (int fd = thread_data->fds_datawithrd.hiter_begin(); 
         fd!=-1;
         fd = thread_data->fds_wr.hiter_next(fd))
    {
        for (auto iter = thread_data->fds_datawithrd.begin(fd); !iter.end(); iter = iter.next())
        {
            if (iter->buffer_idx == old_buffer_id)
            {
                DEBUG("Matched for fd %d in Adjlist", fd);
                iter->status |= FD_STATUS_RD_SND_FORKED;

                fd_rd_list_t tmp_fd_list_ele;
                tmp_fd_list_ele.child[0] = tmp_fd_list_ele.child[1] = -1;
                tmp_fd_list_ele.buffer_idx = new_buffer_id[0];
                tmp_fd_list_ele.status = 0;
                iter->child[0] = thread_data->rd_tree.add(tmp_fd_list_ele);
                tmp_fd_list_ele.buffer_idx = new_buffer_id[1];
                iter->child[1] = thread_data->rd_tree.add(tmp_fd_list_ele);
            } else
            {
                int ret(-1);
                if (iter->child[0] != -1)
                {
                    ret = fork_traverse_rd_tree(iter->child[0], old_buffer_id);
                }
                if ((ret == -1) && (iter->child[1] != -1))
                    ret = fork_traverse_rd_tree(iter->child[1], old_buffer_id);

                if (ret != -1)
                {
                    DEBUG("Matched for fd %d in candidate tree with idx %d", fd, ret);
                    thread_data->rd_tree[ret].status |= FD_STATUS_RD_SND_FORKED;

                    fd_rd_list_t tmp_fd_list_ele;
                    tmp_fd_list_ele.child[0] = tmp_fd_list_ele.child[1] = -1;
                    tmp_fd_list_ele.buffer_idx = new_buffer_id[0];
                    tmp_fd_list_ele.status = 0;
                    thread_data->rd_tree[ret].child[0] = thread_data->rd_tree.add(tmp_fd_list_ele);
                    tmp_fd_list_ele.buffer_idx = new_buffer_id[1];
                    thread_data->rd_tree[ret].child[1] = thread_data->rd_tree.add(tmp_fd_list_ele);
                }
            }
        }
    }
}

#undef DEBUGON
#define DEBUGON 1

void recv_takeover_req_handler(metaqueue_ctl_element ele)
{
    int myfd = ele.req_relay_recv.peer_fd;
    int receiver_fd = ele.req_relay_recv.req_fd;
    thread_data_t * thread_data = GET_THREAD_DATA();
    thread_sock_data_t * thread_sock_data = GET_THREAD_SOCK_DATA();
    int match_buffer_id = (*(thread_sock_data->bufferhash))[ele.req_relay_recv.shmem];
    DEBUG("Takeover msg recvd");
    /*
     * Three things to do:
     * 1. Get the fd, and iterate the adjlist to match the buffer
     * 2. Find the current pointer to the buffer
     * 3. Send sender_req_data to the emergency queue of the pointer in step 2
     * 4. poll itself emergency queue
     */
    //get the iterator of current clock pointer
    auto snd_clk_curr_iter = thread_data->fds_wr.begin(myfd);
    //get the receiver who send the takeover req
    auto req_iter = thread_data->fds_wr.begin(myfd);
    while (!req_iter.end())
    {
        if (req_iter->buffer_idx == match_buffer_id) break;
        req_iter = req_iter.next_no_move_ptr();
    }
    if (req_iter.end())
        FATAL("Fail to find the the receiver who send the takeover req");
    if (req_iter.curr_ptr == snd_clk_curr_iter.curr_ptr)
        return;

    if (snd_clk_curr_iter->status & FD_STATUS_Q_ISOLATED)
    {
        DEBUG("The Q of current clk ptr has been isolated. polled by sender");
        auto polling_q_ptr = &(dynamic_cast<interprocess_local_t *>
        (thread_sock_data->buffer[snd_clk_curr_iter->buffer_idx].data)->q[0]);
        auto polling_buffer_ptr = &(dynamic_cast<interprocess_local_t *>
        (thread_sock_data->buffer[snd_clk_curr_iter->buffer_idx].data)->b[0]);
        unsigned int q_mask = polling_q_ptr->MASK;
        unsigned int q_ptr = polling_q_ptr->pointer;
        unsigned int old_q_ptr = q_ptr;
        //find the tail of the queue by iterating the queue
        interprocess_t::queue_t::element tmp;
        bool tmp_isvalid, tmp_isdel;
        std::tie(tmp_isvalid, tmp_isdel) = polling_q_ptr->peek((q_ptr & q_mask), tmp);
        //currently the queue is not full
        if (!tmp_isvalid)
        {
            while (true)
            {
                bool isvalid, isdel;
                std::tie(isvalid, isdel) = polling_q_ptr->peek((q_ptr - 1) & q_mask, tmp);
                if (!isvalid) break;
                --q_ptr;
            }
        }
        DEBUG("Old q start at %u", q_ptr);
        //iterate the old queue and write the data to takeover req sender
        while (true)
        {
            std::tie(tmp_isvalid, tmp_isdel) = polling_q_ptr->peek(q_ptr, tmp);
            SW_BARRIER;
            if (!tmp_isvalid)
                break;
            if (!tmp_isdel)
            {

                if (tmp.command == interprocess_t::cmd::DATA_TRANSFER &&
                    tmp.data_fd_rw.fd == receiver_fd)
                {
                    //match the fd
                    unsigned char tmp_data_buffer[sizeof(interprocess_t::buffer_t::element::data)];
                    short blk_ptr = tmp.data_fd_rw.pointer;
                    do
                    {
                        //pop one block of data
                        int req_size = sizeof(interprocess_t::buffer_t::element::data);
                        blk_ptr = polling_buffer_ptr->popdata_nomemrelease(blk_ptr, req_size, tmp_data_buffer);
                        //push one block of data
                        auto push_data_buffer_ptr = &(dynamic_cast<interprocess_local_t *>
                        (thread_sock_data->buffer[req_iter->buffer_idx].data)->b[0]);
                        int write_ptr = push_data_buffer_ptr->pushdata(tmp_data_buffer, req_size);
                        auto push_data_metadata_q_ptr = &(dynamic_cast<interprocess_local_t *>
                        (thread_sock_data->buffer[req_iter->buffer_idx].data)->q[0]);
                        interprocess_t::queue_t::element push_data_metadata;
                        push_data_metadata.command = interprocess_t::cmd::DATA_TRANSFER;
                        push_data_metadata.data_fd_rw.fd = receiver_fd;
                        push_data_metadata.data_fd_rw.pointer = write_ptr;
                        push_data_metadata_q_ptr->push(push_data_metadata);
                        DEBUG("One blk data pushed for receiver fd %d", receiver_fd);
                    }
                    while (blk_ptr != -1);
                }
                else
                {
                    auto push_data_metadata_q_ptr = &(dynamic_cast<interprocess_local_t *>(
                            thread_sock_data->buffer[req_iter->buffer_idx].data)->q[0]);
                    push_data_metadata_q_ptr->push(tmp);
                }
                polling_q_ptr->del(q_ptr);
            }
            if (polling_q_ptr->tail > q_ptr)
                q_ptr = polling_q_ptr->tail;
            else
                ++q_ptr;
        }
    }
        //If the queue of the current ptr is not isolated
    else
    {

    }

    SW_BARRIER;
    (*(dynamic_cast<interprocess_local_t *>(thread_sock_data->buffer[snd_clk_curr_iter->buffer_idx].data)->
            sender_turn[0]))[myfd] = false;
    SW_BARRIER;
    (*(dynamic_cast<interprocess_local_t *>(thread_sock_data->buffer[req_iter->buffer_idx].data)
            ->sender_turn[0]))[myfd] = true;
    SW_BARRIER;
    thread_data->fds_wr.set_ptr_to(myfd, req_iter);
    SW_BARRIER;
    /*file_struc_wr_t * curr_adjlist_h = &thread_data->fds_wr[myfd];
    for (int bufferidx_in_list = curr_adjlist_h->iterator_init();
         bufferidx_in_list != -1;
         bufferidx_in_list = curr_adjlist_h->iterator_next(bufferidx_in_list))
    {
        //if the shmem key not match, continue
        if (match_key_id != (*curr_adjlist_h)[bufferidx_in_list].buffer_idx)
            continue;
        //First put itself in the adjlist
        fd_vec_t buffer = curr_adjlist_h->operator[](bufferidx_in_list);

        fd_wr_list_t n_adjlist_ele;
        n_adjlist_ele.buffer_idx = buffer.buffer_idx;
        n_adjlist_ele.status = 0;
        n_adjlist_ele.id_in_v = bufferidx_in_list;

        thread_data->fds_wr.add_element(myfd, n_adjlist_ele);
        DEBUG("New buffer id %d in candidate list idx %d promote to adjlist", match_key_id, bufferidx_in_list);

        //iterate remove the parent
        for (int par_idx = (*curr_adjlist_h)[bufferidx_in_list].parent_id_in_v;
             par_idx != -1;
             par_idx = (*curr_adjlist_h)[par_idx].parent_id_in_v)
        {
            if (curr_adjlist_h->isvalid(par_idx))
            {
                //search the adjlist first
                for (auto iter = thread_data->fds_wr.begin(myfd); !iter.end();)
                {
                    if (iter->id_in_v == par_idx)
                    {
                        //remove it from the list
                        iter = thread_data->fds_wr.del_element(iter);
                        DEBUG("Parent in adjlist with loc %d removed", par_idx);
                    } else
                    {
                        iter = iter.next();
                    }
                }
                DEBUG("Parent at loc %d removed", par_idx);
                curr_adjlist_h->del(par_idx);
            }
        }
    }


    //send ack to the back to monitor
    ele.command = REQ_RELAY_RECV_ACK;
    thread_data->metaqueue.q[0].push(ele);*/
    /*
     *
     *
     */

}
/*

bool takeover_ack_traverse_rd_tree(int curr_idx, int match_bid)
{
    if (curr_idx==-1)
        return true;
    thread_data_t *tdata = GET_THREAD_DATA();
    fd_rd_list_t curr_node = tdata->rd_tree[curr_idx];
    if (!(curr_node.status | FD_STATUS_RECV_REQ)) return true;
    if (curr_node.status | FD_STATUS_RECV_ACK) return true;
    if (match_bid == curr_node.buffer_idx)
    {
        tdata->rd_tree[curr_idx].status |= FD_STATUS_RECV_ACK;
        return true;
    }
    bool ret(true);
    bool isleaf(true);
    if (tdata->rd_tree[curr_idx].child[0] != -1)
    {
        ret = ret && takeover_ack_traverse_rd_tree(tdata->rd_tree[curr_idx].child[0], match_bid);
        isleaf = false;
    }
    if (tdata->rd_tree[curr_idx].child[1] != -1)
    {
        ret = ret && takeover_ack_traverse_rd_tree(tdata->rd_tree[curr_idx].child[1], match_bid);
        isleaf = false;
    }
    if (!isleaf)
    {
        if (ret)
            tdata->rd_tree[curr_idx].status |= FD_STATUS_RECV_ACK;
        return ret;
    } else
    {
        return false;
    }

}*/



/*void recv_takeover_ack_handler(metaqueue_ctl_element ele)
{
    key_t shmem_key = ele.req_relay_recv.shmem;
    int myfd = ele.req_relay_recv.req_fd;
    int peerfd = ele.req_relay_recv.peer_fd;
    thread_data_t * thread_data = GET_THREAD_DATA();
    thread_sock_data_t * thread_sock_data = GET_THREAD_SOCK_DATA();
    int match_buffer_idx = (*thread_sock_data->bufferhash)[shmem_key];
    //get the handler of the current adjlist
    for (auto iter = thread_data->fds_datawithrd.begin(myfd); !iter.end();)
    {
        bool match_ret(true);
        //first try the left tree
        if (iter->child[0] != -1)
        {
            match_ret = match_ret && takeover_ack_traverse_rd_tree(iter->child[0], match_buffer_idx);
        }
        if (iter->child[1] != -1)
        {
            match_ret = match_ret && takeover_ack_traverse_rd_tree(iter->child[1], match_buffer_idx);
        }
        if (match_ret)
        {
            iter->status |= FD_STATUS_RECV_ACK;
            DEBUG("Acked for %d fd", myfd);

            if (iter->status & FD_STATUS_RD_RECV_FORKED) //it is itself fork
            {
                //check whether it is empty
                if (thread_sock_data->buffer[iter->buffer_idx].data.q[1].isempty())
                {
                    DEBUG("Queue for fd %d is empty, children %d %d", myfd, iter->child[0], iter->child[1]);
                    //remove itself and add its children
                    if (iter->child[0] != -1)
                        iter = thread_data->fds_datawithrd.add_element_at(iter, myfd, thread_data->rd_tree[iter->child[0]]);
                    if (iter->child[1] != -1)
                        iter = thread_data->fds_datawithrd.add_element_at(iter, myfd, thread_data->rd_tree[iter->child[1]]);

                    //remove itself from adjlist
                    iter = thread_data->fds_datawithrd.del_element(iter);
                    continue;
                }
            }
        }
        iter=iter.next();
    }
}*/

void monitor2proc_hook()
{
    thread_data_t *thread_data = GET_THREAD_DATA();
    metaqueue_ctl_element ele;
    while (thread_data->metaqueue.q_emergency[1].pop_nb(ele))
    {
        switch (ele.command)
        {
            case RES_PUSH_FORK:
                res_push_fork_handler(ele);
                break;
            case REQ_RELAY_RECV:
                recv_takeover_req_handler(ele);
                break;
            /*case REQ_RELAY_RECV_ACK:
                recv_takeover_ack_handler(ele);
                break;*/
        }
    }
}

#undef DEBUGON
#define DEBUGON 0

inline int thread_sock_data_t::isexist(key_t key)
{
    std::unordered_map<key_t, int>::iterator iter;
    if ((iter = bufferhash->find(key)) == bufferhash->end())
        return -1;
    if (buffer[iter->second].isvalid)
        return iter->second;
    else return -1;
}

int thread_sock_data_t::newbuffer(key_t key, int loc)
{
    buffer[lowest_available].loc = loc;
    buffer[lowest_available].isvalid = true;
    buffer[lowest_available].data = new interprocess_local_t;
    dynamic_cast<interprocess_local_t *>(buffer[lowest_available].data)->init(key, loc);
    buffer[lowest_available].shmemkey = key;
    buffer[lowest_available].isRDMA = false;
    (*bufferhash)[key]=lowest_available;
    ++total_num;
    if (total_num == BUFFERNUM)
        FATAL("Dynamic allocation not implemented!");
    int ret = lowest_available;
    ++lowest_available;
    return ret;
}


std::pair<int, rdma_self_pack_t *>  thread_sock_data_t::newbuffer_rdma(key_t key, int loc)
{
    buffer[lowest_available].loc = loc;
    buffer[lowest_available].isvalid = true;
    buffer[lowest_available].shmemkey = key;
    buffer[lowest_available].isRDMA = true;
    (*bufferhash)[key]=lowest_available;
    ++total_num;
    if (total_num == BUFFERNUM)
        FATAL("Dynamic allocation not implemented!");

    //Init RDMA QP
    rdma_self_pack_t * rdma_self_qp  = lib_new_qp();
    buffer[lowest_available].rdma_info.rkey = rdma_self_qp->rkey;
    buffer[lowest_available].rdma_info.myqp = rdma_self_qp->qp;
    buffer[lowest_available].rdma_info.remote_buf_ptr = rdma_self_qp->localptr;
    buffer[lowest_available].rdma_info.send_cq = rdma_self_qp->send_cq;
    buffer[lowest_available].rdma_info.qid = -1;
    int ret = lowest_available;
    ++lowest_available;
    return std::make_pair(ret, rdma_self_qp);
}

void fd_remapping_init()
{
    fd_reverse_map_table.resize(NUM_FD_TYPES);
    // We create a fake FD to find the next available FD for allocation
    // This method is NOT reliable if some FD has been closed
    // anyway, we just use it for now
    max_virtual_fd = ORIG(socket, (AF_INET, SOCK_STREAM, 0));
    if (max_virtual_fd == -1) {
        FATAL("could not create virtual fd");
        return;
    }
    ORIG(close, (max_virtual_fd));
    max_virtual_fd -= 1; // the actual max FD (before fake FD)

    fd_remap_initialized = true;
    for (int i = 0; i <= max_virtual_fd; i ++) {
        set_fd_type(i, FD_TYPE_SYSTEM, i); // map to self for existing fds
    }
}

void usocket_init()
{
    thread_data_t *data;
    data = reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
    if (data == nullptr) {
        FATAL("pthread_key not initialized");
        return;
    }
    file_struc_rd_t _fd;
    data->fds_datawithrd.init(0,_fd);
    data->fds_wr.init(0, 0);
    data->is_rdma_initialized = false;
    auto *thread_sock_data = new thread_sock_data_t;
    thread_sock_data->bufferhash = new std::unordered_map<key_t, int>;
    for (int i = 0; i < BUFFERNUM; ++i) thread_sock_data->buffer[i].isvalid = false;
    thread_sock_data->lowest_available = 0;
    pthread_setspecific(pthread_sock_key, reinterpret_cast<void *>(thread_sock_data));
}

int socket(int domain, int type, int protocol) __THROW
{
    if ((domain != AF_INET) || (type != SOCK_STREAM)) {
        int real_fd = ORIG(socket, (domain, type, protocol));
        return alloc_virtual_fd(FD_TYPE_SYSTEM, real_fd);
    }

    thread_data_t *data;
    data = reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
    // initialized RDMA on first socket call
    // we do not initialize in RDMA library startup, because other libraries required by RDMA are not properly initialized at that time
    if ( ! data->is_rdma_initialized) {
        /*
        // We actually do not need to call ibv_fork_init() because it only performs
        // memory protection to avoid the child process to accidentally use the
        // RDMA connection.
        // What we do is to create independent IB context, memory regions, QPs for
        // child thread.
        //
        int ret;
        if ((ret = ibv_fork_init()) != 0)
            FATAL("RDMA Fork prepare fail, %s %d, %d", strerror(errno), errno, ret);
        else
            DEBUG("RDMA fork prepare success");
        */
        rdma_init();
        data->is_rdma_initialized = true;
    }

    rdma_init();
    file_struc_rd_t nfd;
    nfd.property.is_addrreuse = false;
    nfd.property.is_blocking = true;
    nfd.property.is_accept_command_sent = false;
    nfd.property.tcp.isopened = false;
    unsigned int idx_nfd=data->fds_datawithrd.add_key(nfd);
    data->fds_wr.add_key(0);
    return alloc_virtual_fd(FD_TYPE_SOCKET, idx_nfd);
}

int bind(int socket, const struct sockaddr *address, socklen_t address_len) __THROW
{
    if (get_fd_type(socket) == FD_TYPE_SYSTEM)
        return ORIG(bind, (get_real_fd(socket), address, address_len));
    socket = get_real_fd(socket);

    unsigned short port;
    thread_data_t *data = nullptr;
    data = reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
    port = ntohs(((struct sockaddr_in *) address)->sin_port);
    data->fds_datawithrd[socket].property.tcp.port = port;
    return 0;
}

int listen(int socket, int backlog) __THROW
{
    if (get_fd_type(socket) == FD_TYPE_SYSTEM)
        return ORIG(listen, (get_real_fd(socket), backlog));
    socket = get_real_fd(socket);

    metaqueue_ctl_element data2m, data_from_m;
    thread_data_t *data;
    data = reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
    data2m.command = REQ_LISTEN;
    if (!data->fds_datawithrd.is_keyvalid(socket))
    {
        errno = EBADF;
        return -1;
    }
    data->fds_datawithrd[socket].type = USOCKET_TCP_LISTEN;
    data2m.req_listen.is_reuseaddr = data->fds_datawithrd[socket].property.is_addrreuse;
    data2m.req_listen.port = data->fds_datawithrd[socket].property.tcp.port;
    metaqueue_t * q = &(data->metaqueue);
    q->q[0].push(data2m);
    while (!q->q[1].pop_nb(data_from_m));
    if (data_from_m.command == RES_ERROR)
    {
        return -1;
        errno = data_from_m.resp_command.err_code;
    }
    else return 0;
}

int shutdown(int socket, int how)
{
    if (get_fd_type(socket) == FD_TYPE_SYSTEM) {
        return ORIG(shutdown, (get_real_fd(socket), how));
    }
    // shutdown is not implemented now, ignore it
    return 0;
}

#undef DEBUGON
#define DEBUGON 0
int close(int fildes)
{
    if (get_fd_type(fildes) == FD_TYPE_SYSTEM) {
        int ret = ORIG(close, (get_real_fd(fildes)));
        DEBUG("close system FD virtual %d real %d", fildes, get_real_fd(fildes));
        if (ret == 0)
            delete_virtual_fd(fildes);
        return ret;
    }
    if (get_fd_type(fildes) == FD_TYPE_EPOLL) {
        DEBUG("close epoll FD virtual %d", fildes);
        epoll_remove(fildes);
        return 0;
    }

    DEBUG("close socket FD virtual %d real %d", fildes, get_real_fd(fildes));
    fildes = get_real_fd(fildes);

    thread_data_t *data = NULL;
    data = reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
    if (!data->fds_datawithrd.is_keyvalid(fildes))
    {
        errno = EBADF;
        return -1;
    }
    if (data->fds_datawithrd[fildes].type == USOCKET_TCP_CONNECT)
    {
        thread_sock_data_t* sock_data=GET_THREAD_SOCK_DATA();
        for (auto iter = data->fds_datawithrd.begin(fildes); !iter.end(); )
        {
            fd_rd_list_t peer_adj_item = *iter;
            interprocess_n_t *interprocess;
            interprocess = sock_data->buffer[peer_adj_item.buffer_idx].data;
            interprocess_t::queue_t::element ele;
            ele.command=interprocess_t::cmd::CLOSE_FD;
            ele.close_fd.req_fd=fildes;
            ele.close_fd.peer_fd=data->fds_datawithrd[fildes].peer_fd;
            interprocess->push_data(ele, 0, nullptr);
            iter = data->fds_datawithrd.del_element(iter);
        }
    }

    if (data->fds_datawithrd[fildes].type == USOCKET_TCP_LISTEN)
    {
        metaqueue_ctl_element ele;
        ele.command = REQ_CLOSE;
        ele.req_close.port=data->fds_datawithrd[fildes].property.tcp.port;
        ele.req_close.listen_fd=fildes;
        data->metaqueue.q[0].push(ele);
    }
    data->fds_datawithrd[fildes].property.tcp.isopened=false;
    data->fds_datawithrd.del_key(fildes);
    data->fds_wr.del_key(fildes);
    delete_virtual_fd(fildes);
    return 0;
}
#undef DEBUGON
#define DEBUGON 1
metaqueue_t * connect_with_rdma_stub(int socket, struct in_addr remote_addr)
{
    /*
     * In this stub, it needs do several things:
     * 1. check whether RDMA connection to remote monitor exists
     *     If not exist:
     *         Create TCP connection
     *         negotiate QP, assign metaqueue
     * 2. Inquiry whether pre-existing RDMA connections
     */
    rdma_metaqueue * q2monitor = rdma_try_connect_remote_monitor(remote_addr);
    DEBUG("RDMA Connection to monitor Finished!");
    return &(q2monitor->queue);
}

#undef DEBUGON
#define DEBUGON 0
int connect(int socket, const struct sockaddr *address, socklen_t address_len)
{
    if (get_fd_type(socket) == FD_TYPE_SYSTEM)
        return ORIG(connect, (get_real_fd(socket), address, address_len));
    socket = get_real_fd(socket);

    //init
    char addr_str[100];
    inet_ntop(AF_INET, ((void *) &((struct sockaddr_in *) address)->sin_addr), addr_str, address_len);
    if (address->sa_family != AF_INET)
        FATAL("Only support TCP connection");

    thread_data_t *data;
    data = reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
    if (!data->fds_datawithrd.is_keyvalid(socket))
    {
        errno = EBADF;
        return -1;
    }
    if (data->fds_datawithrd[socket].property.tcp.isopened) close(socket);
    data->fds_datawithrd[socket].type = USOCKET_TCP_CONNECT;
    data->fds_datawithrd[socket].property.tcp.isopened=true;

    metaqueue_t *q2monitor;
    bool isRDMA(false);
    if (strcmp(addr_str, "127.0.0.1") != 0)
    {
        struct in_addr remote_addr_int;
        if (!inet_aton(addr_str, &remote_addr_int))
            FATAL("Invalid remote addr");
        q2monitor = connect_with_rdma_stub(socket, remote_addr_int);
        isRDMA = true;
        //FATAL("not support unlocal address");
    } else
        q2monitor = &(data->metaqueue);

    unsigned short port;
    port = ntohs(((struct sockaddr_in *) address)->sin_port);

    DEBUG("sending connect request to monitor: fd %d", socket);

    //send to monitor and get respone
    metaqueue_ctl_element req_data, res_data;
    req_data.command = REQ_CONNECT;
    req_data.req_connect.fd = socket;
    req_data.req_connect.port = port;
    req_data.req_connect.isRDMA = isRDMA;

    q2monitor->q[0].push(req_data);
    while (!q2monitor->q[1].pop_nb(res_data));
    if (res_data.command != RES_SUCCESS) {
        DEBUG("Connection failed to %s:%d", addr_str, port);
        errno = ECONNREFUSED;
        return -1;
    }
    //printf("%d\n", res_data.data.sock_connect_res.shm_key);
    key_t key = res_data.resp_connect.shm_key;
    int loc = res_data.resp_connect.loc;

    //init buffer
    thread_sock_data_t *thread_buf;
    thread_buf = reinterpret_cast<thread_sock_data_t *>(pthread_getspecific(pthread_sock_key));
    if (thread_buf == nullptr)
        FATAL("Failed to get thread specific data.");
    int idx;
    fd_rd_list_t peer_fd_rd;
    peer_fd_rd.child[0] = peer_fd_rd.child[1] = -1;
    peer_fd_rd.status = 0;

    //if a connection between two process already exists, no new buffer is needed
    if ((idx = thread_buf->isexist(key)) != -1)
        peer_fd_rd.buffer_idx = idx;
    else {
        if (!isRDMA)
            idx = peer_fd_rd.buffer_idx = thread_buf->newbuffer(key, loc);
        else
        {
            rdma_self_pack_t* rdma_self_qpinfo;
            std::tie(idx, rdma_self_qpinfo) =  thread_buf->newbuffer_rdma(key, loc);
            peer_fd_rd.buffer_idx = idx;
            metaqueue_long_msg_rdmainfo_t rdmainfo;
            rdmainfo.shm_key = key;
            rdma_pack * rdma_lib_pack;
            rdma_lib_pack = rdma_get_pack();
            rdmainfo.qpinfo.RoCE_gid = rdma_lib_pack->RoCE_gid;
            rdmainfo.qpinfo.qpn = rdma_self_qpinfo->qpn;
            rdmainfo.qpinfo.qid = -1;
            rdmainfo.qpinfo.rkey = rdma_self_qpinfo->rkey;
            rdmainfo.qpinfo.remote_buf_addr = rdma_self_qpinfo->localptr;
            rdmainfo.qpinfo.port_lid = rdma_lib_pack->port_lid;
            rdmainfo.qpinfo.buf_size = rdma_self_qpinfo->buf_size;
            q2monitor->push_longmsg(sizeof(metaqueue_long_msg_rdmainfo_t), (void *)&rdmainfo, RDMA_QP_INFO);

            //Try to get the QP info from the monitor
            metaqueue_ctl_element element;
            while (!q2monitor->q[1].pop_nb(element));
            if (element.command != LONG_MSG_HEAD || element.long_msg_head.subcommand != RDMA_QP_INFO)
                FATAL("Invalid RDMA info");
            metaqueue_long_msg_rdmainfo_t *peer_rdmainfo;
            peer_rdmainfo = (metaqueue_long_msg_rdmainfo_t *)q2monitor->pop_longmsg(element.long_msg_head.len);
            rdma_connect_remote_qp(rdma_self_qpinfo->qp, rdma_get_pack(), &(peer_rdmainfo->qpinfo));

            //Reply ACK to server
            thread_buf->buffer[idx].data = new interprocess_local_t;
            void* interprocess_t_baseaddr =  (void *)rdma_self_qpinfo->localptr;
            //Note: Client use the lower side to send
            dynamic_cast<interprocess_local_t *>(thread_buf->buffer[idx].data)->init(interprocess_t_baseaddr, loc);
            dynamic_cast<interprocess_local_t *>(thread_buf->buffer[idx].data)->
                    initRDMA(rdma_self_qpinfo->qp, rdma_self_qpinfo->send_cq,rdma_lib_pack->buf_mr->lkey,
                            peer_rdmainfo->qpinfo.rkey, peer_rdmainfo->qpinfo.remote_buf_addr, interprocess_t_baseaddr, loc);

            metaqueue_ctl_element ele;
            ele.command = RDMA_QP_ACK;
            ele.req_relay_recv.shmem = key;
            q2monitor->q[0].push(ele);
            DEBUG("Connect Finished\n");
            //while (1);

        }
    }
    auto peerfd_iter = data->fds_datawithrd.add_element(socket, peer_fd_rd);
    fd_wr_list_t peer_fd_wr;
    peer_fd_wr.status = 0;
    peer_fd_wr.buffer_idx = peer_fd_rd.buffer_idx;
    auto wr_iter = data->fds_wr.add_element(socket, peer_fd_wr);
    data->fds_wr.set_ptr_to(socket, wr_iter);

    DEBUG("connect fd %d replied by monitor, waiting for ACK from peer", socket);

    //wait for ACK from peer
    interprocess_n_t *buffer;
    buffer = thread_buf->buffer[idx].data;
    if (!isRDMA)
        (*(dynamic_cast<interprocess_local_t *>(buffer)->sender_turn[0]))[socket] = true;

    bool isvalid(false), isdel(true);
    bool isFind(false);



    do
    {
        interprocess_t::queue_t::element ele;
        auto iter = buffer->begin();
        while (true) {
            std::tie(isvalid, isdel) = iter.peek(ele);
            if (!isvalid) break;
            if (isdel)
            {
                iter = iter.next();
                continue;
            }
            if (ele.command == interprocess_t::cmd::NEW_FD)
            {
                data->fds_datawithrd[socket].peer_fd = ele.data_fd_notify.fd;
                DEBUG("peer fd: %d",ele.data_fd_notify.fd);
                SW_BARRIER;
                iter.del();
                isFind = true;
                break;
            } else
            {
                iter = iter.next();
                continue;
            }
        }
        iter.destroy();
    } while (!isFind);


    /*
    interprocess_t::queue_t::element element;
    bool isFind = false;
    while (true)
    {
        //printf("head peek %d\n", buffer->q[1].tail);
        for (unsigned int i = buffer->q[1].tail; ; i++)
        {
            bool ele_isvalid, ele_isdel;
            std::tie(ele_isvalid, ele_isdel) = buffer->q[1].peek(i, element);
            if (!ele_isvalid) {
                break;
            }
            if (ele_isdel) {
                continue;
            }
            if (element.command == interprocess_t::cmd::NEW_FD)
            {
                data->fds_datawithrd[socket].peer_fd = element.data_fd_notify.fd;
                DEBUG("peer fd: %d\n",element.data_fd_notify.fd);
                SW_BARRIER;
                buffer->q[1].del(i);
                isFind = true;
                break;
            }
        }
        if (isFind) break;
        //printf("not found\n");
    }*/
    DEBUG("connect fd %d complete", socket);
    return 0;
}

#undef DEBUGON
#define DEBUGON 0
int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags)
{
    if (get_fd_type(sockfd) == FD_TYPE_SYSTEM) {
        int real_fd = ORIG(accept4, (get_real_fd(sockfd), addr, addrlen, flags));
        return alloc_virtual_fd(FD_TYPE_SYSTEM, real_fd);
    }
    sockfd = get_real_fd(sockfd);

    thread_data_t *data;
    data = reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
    thread_sock_data_t *sock_data;
    sock_data = reinterpret_cast<thread_sock_data_t *>(pthread_getspecific(pthread_sock_key));

    metaqueue_ctl_element element;
    // if no pending new connections from the monitor
    if (!data->metaqueue.q[1].pop_nb(element))
    {
        // send a command to notify monitor, only for the first time
        if (!data->fds_datawithrd[sockfd].property.is_accept_command_sent) {
            metaqueue_ctl_element accept_command;
            accept_command.command = REQ_ACCEPT;
            accept_command.req_accept.port = data->fds_datawithrd[sockfd].property.tcp.port;
            data->metaqueue.q[0].push(accept_command);
            data->fds_datawithrd[sockfd].property.is_accept_command_sent = true; 
        }
        // wait for connect requests from the monitor
        if (data->fds_datawithrd[sockfd].property.is_blocking) {
            while (!data->metaqueue.q[1].pop_nb(element));
        } else {
            errno = EAGAIN;
            return -1;
        }
    }

    // now the command from monitor must be valid. check if is a new connection request
    if (element.command != RES_NEWCONNECTION)
        FATAL("unordered accept response");

    if (!data->fds_datawithrd.is_keyvalid(sockfd) || data->fds_datawithrd[sockfd].type != USOCKET_TCP_LISTEN)
    {
        errno = EBADF;
        return -1;
    }
    if (element.resp_connect.port != data->fds_datawithrd[sockfd].property.tcp.port)
        FATAL("Incorrect accept order for different ports");
    key_t key = element.resp_connect.shm_key;
    int loc = element.resp_connect.loc;
    auto peer_fd = element.resp_connect.fd;
    bool isRDMA = element.resp_connect.isRDMA;
    DEBUG("Accept new connection, key %d, loc %d RDMA: %d", key, loc, isRDMA);
    file_struc_rd_t nfd_rd;
    fd_rd_list_t npeerfd_rd;

    nfd_rd.property.is_addrreuse = 0;
    nfd_rd.property.is_blocking = (flags & SOCK_NONBLOCK)?0:1;
    nfd_rd.type = USOCKET_TCP_CONNECT;
    nfd_rd.property.tcp.isopened = true;
    auto idx_nfd=data->fds_datawithrd.add_key(nfd_rd);


    data->fds_datawithrd[idx_nfd].peer_fd = peer_fd;
    int idx;
    if ((idx = sock_data->isexist(key)) != -1)
        npeerfd_rd.buffer_idx = idx;
    else
    {
        if (!isRDMA)
            idx = npeerfd_rd.buffer_idx = sock_data->newbuffer(key, loc);
        else
        {
            //It is the RDMA connection, create a RDMA buffer, get the peer QP info, connect QP, reply ACK
            rdma_self_pack_t* rdma_self_qpinfo;
            std::tie(idx, rdma_self_qpinfo) =  sock_data->newbuffer_rdma(key, loc);
            //The next thing we need to do is to connect the QP
            while (!data->metaqueue.q[1].pop_nb(element));
            if (element.command != LONG_MSG_HEAD || element.long_msg_head.subcommand != RDMA_QP_INFO)
                FATAL("Invalid RDMA info");
            metaqueue_long_msg_rdmainfo_t *peer_rdmainfo;
            peer_rdmainfo = (metaqueue_long_msg_rdmainfo_t *)data->metaqueue.pop_longmsg(element.long_msg_head.len);
            rdma_connect_remote_qp(rdma_self_qpinfo->qp, rdma_get_pack(), &(peer_rdmainfo->qpinfo));
            //save the remote addr
            uint64_t remote_addr = peer_rdmainfo->qpinfo.remote_buf_addr;
            uint32_t remote_rkey = peer_rdmainfo->qpinfo.rkey;
            //The next thing we should do is to send my QPinfo to the peer
            memset(peer_rdmainfo, 0, sizeof(metaqueue_long_msg_rdmainfo_t));
            peer_rdmainfo->shm_key = key;
            peer_rdmainfo->qpinfo.rkey = rdma_self_qpinfo->rkey;
            peer_rdmainfo->qpinfo.remote_buf_addr = rdma_self_qpinfo->localptr;
            peer_rdmainfo->qpinfo.port_lid = rdma_self_qpinfo->port_lid;
            peer_rdmainfo->qpinfo.buf_size = rdma_self_qpinfo->buf_size;
            peer_rdmainfo->qpinfo.qpn = rdma_self_qpinfo->qpn;
            peer_rdmainfo->qpinfo.qid = -1;
            peer_rdmainfo->qpinfo.RoCE_gid = rdma_self_qpinfo->RoCE_gid;
            data->metaqueue.push_longmsg(sizeof(metaqueue_long_msg_rdmainfo_t), (void*)peer_rdmainfo, RDMA_QP_INFO);
            free((void*)peer_rdmainfo);
            npeerfd_rd.buffer_idx = idx;

            //Wait for the ACK for the peer
            DEBUG("Ack required");
            metaqueue_ctl_element ele;
            while (!data->metaqueue.q[1].pop_nb(ele));
            if (ele.command != RDMA_QP_ACK)
                FATAL("ACK Failed");
            void* interprocess_t_baseaddr =  (void *)rdma_self_qpinfo->localptr;
            //Note: Client use the lower side to send
            sock_data->buffer[idx].data = new interprocess_local_t;
            dynamic_cast<interprocess_local_t *>(sock_data->buffer[idx].data)->init(interprocess_t_baseaddr, loc);
            dynamic_cast<interprocess_local_t *>(sock_data->buffer[idx].data)->
                    initRDMA(rdma_self_qpinfo->qp, rdma_self_qpinfo->send_cq,rdma_get_pack()->buf_mr->lkey,
                             remote_rkey, remote_addr, interprocess_t_baseaddr, loc);

            
            DEBUG("server conn fin");
            //while (1);
        }

    }
    npeerfd_rd.child[0] = npeerfd_rd.child[1] = -1;
    npeerfd_rd.status = 0;
    data->fds_datawithrd.add_element(idx_nfd, npeerfd_rd);

    fd_wr_list_t npeerfd_wr;
    npeerfd_wr.buffer_idx = idx;
    npeerfd_wr.status = 0;
    data->fds_wr.add_key(0);
    auto iter_wr = data->fds_wr.add_element(idx_nfd, npeerfd_wr);


    interprocess_n_t *buffer = sock_data->buffer[idx].data;
    if (!isRDMA)
    {
        //If it is not RDMA, change RDMA pointer
        (*dynamic_cast<interprocess_local_t *>(buffer)->sender_turn[0])[idx_nfd] = true;
    }
    data->fds_wr.set_ptr_to(idx_nfd, iter_wr);
    interprocess_t::queue_t::element inter_element;
    inter_element.command = interprocess_t::cmd::NEW_FD;
    inter_element.data_fd_notify.fd = idx_nfd;
    //printf("currfd: %d\n", curr_fd);
    SW_BARRIER;
    buffer->push_data(inter_element,0, nullptr);

    int virtual_fd = alloc_virtual_fd(FD_TYPE_SOCKET, idx_nfd);
    if (addr) {
        getpeername(virtual_fd, addr, addrlen);
    }
    DEBUG("accept complete with virtual FD %d real FD %d", virtual_fd, idx_nfd);
    return virtual_fd;
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    if (get_fd_type(sockfd) == FD_TYPE_SYSTEM) {
        int real_fd = ORIG(accept, (get_real_fd(sockfd), addr, addrlen));
        return alloc_virtual_fd(FD_TYPE_SYSTEM, real_fd);
    }
        
    return accept4(sockfd, addr, addrlen, 0);
}


int setsockopt(int socket, int level, int option_name, const void *option_value, socklen_t option_len)
{
    if (get_fd_type(socket) == FD_TYPE_SYSTEM) return ORIG(setsockopt, (get_real_fd(socket), level, option_name, option_value, option_len));
    socket = get_real_fd(socket);

    if ((level == SOL_SOCKET) && (option_name == SO_REUSEPORT))
    {
        thread_data_t *thread;
        thread = reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
        if (thread == nullptr || !thread->fds_datawithrd.is_keyvalid(socket))
        {
            errno = EBADF;
            return -1;
        }
        thread->fds_datawithrd[socket].property.is_addrreuse = *reinterpret_cast<const int *>(option_value);
    }
    return 0;
}

int getsockname(int socket, struct sockaddr *addr, socklen_t *addrlen)
{
    if (get_fd_type(socket) == FD_TYPE_SYSTEM) return ORIG(getsockname, (get_real_fd(socket), addr, addrlen));
    socket = get_real_fd(socket);

    thread_data_t *thread = reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
    if (thread == nullptr || ! thread->fds_datawithrd.is_keyvalid(socket))
    {
        errno = EBADF;
        return -1;
    }
    
    // placeholder for getsockname
    addr->sa_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &((struct sockaddr_in *)addr)->sin_addr);
    ((struct sockaddr_in *)addr)->sin_port = htons(80);
    if (addrlen)
        *addrlen = sizeof(struct sockaddr_in);
    return 0;
}

int getpeername(int socket, struct sockaddr *addr, socklen_t *addrlen)
{
    if (get_fd_type(socket) == FD_TYPE_SYSTEM) return ORIG(getpeername, (get_real_fd(socket), addr, addrlen));
    socket = get_real_fd(socket);

    thread_data_t *thread = reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
    if (thread == nullptr || ! thread->fds_datawithrd.is_keyvalid(socket))
    {
        errno = EBADF;
        return -1;
    }
    
    // placeholder for getpeername
    addr->sa_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &((struct sockaddr_in *)addr)->sin_addr);
    ((struct sockaddr_in *)addr)->sin_port = htons(12345);
    if (addrlen)
        *addrlen = sizeof(struct sockaddr_in);
    return 0;
}

int getsockopt(int socket, int level, int option_name, void *option_value, socklen_t *option_len)
{
    if (get_fd_type(socket) == FD_TYPE_SYSTEM) return ORIG(getsockopt, (get_real_fd(socket), level, option_name, option_value, option_len));
    socket = get_real_fd(socket);

    if ((level == SOL_SOCKET) && (option_name == SO_REUSEPORT))
    {
        thread_data_t *thread;
        thread = reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
        if (thread == nullptr || ! thread->fds_datawithrd.is_keyvalid(socket))
        {
            errno = EBADF;
            return -1;
        }
        *(reinterpret_cast<int*>(option_value)) = thread->fds_datawithrd[socket].property.is_addrreuse;
        if (option_len)
            *option_len = sizeof(int);
    }
    else {
        // not implemented yet
        if (option_len)
            *option_len = 0;
    }
    return 0;
}

int fcntl(int fd, int cmd, ...) __THROW
{
    va_list p_args;
    va_start(p_args, cmd);
    int flags = va_arg(p_args, int);
    if (get_fd_type(fd) == FD_TYPE_SYSTEM)
        return ORIG(fcntl, (get_real_fd(fd), cmd, flags));
    if (get_fd_type(fd) == FD_TYPE_EPOLL)
        return 0; // not implemented, ignore
    fd = get_real_fd(fd);

    thread_data_t *thread_data = GET_THREAD_DATA();
    if (!thread_data->fds_datawithrd.is_keyvalid(fd)) {
        errno = EBADF;
        return -1;
    }

    switch (cmd) {
        case F_SETFL:
            if (flags == O_NONBLOCK) {
                thread_data->fds_datawithrd[fd].property.is_blocking = false;
            }
            return 0; // ignore other flags
        case F_GETFL: {
            int ret_flags = O_RDWR;
            if (thread_data->fds_datawithrd[fd].property.is_blocking)
                ret_flags = O_NONBLOCK;
            return ret_flags;
        }
        case F_SETFD:
            return 0; // ignore
        case F_GETFD:
            return 0;
        default:
            errno = ENOTSUP;
            return -1;
    }
}

int ioctl(int fildes, unsigned long request, ...) __THROW
{
    va_list p_args;
    va_start(p_args, request);
    char *argp = va_arg(p_args, char*);
    if (get_fd_type(fildes) == FD_TYPE_SYSTEM)
        return ORIG(ioctl, (get_real_fd(fildes), request, argp));
    fildes = get_real_fd(fildes);

    thread_data_t *thread_data = reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
    if (!thread_data->fds_datawithrd.is_keyvalid(fildes))
    {
        errno = EBADF;
        return -1;
    }

    if (request == FIONBIO)
        thread_data->fds_datawithrd[fildes].property.is_blocking = 0;
    return 0;
}

#undef DEBUGON
#define DEBUGON 0
ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    if (get_fd_type(fd) == FD_TYPE_SYSTEM) return ORIG(writev, (get_real_fd(fd), iov, iovcnt));
    fd = get_real_fd(fd);

    thread_data_t *thread_data = GET_THREAD_DATA();
    if (thread_data->fds_wr.begin(fd).end() || thread_data->fds_datawithrd[fd].type != USOCKET_TCP_CONNECT
            || !thread_data->fds_datawithrd[fd].property.tcp.isopened)
    {
        errno = EBADF;
        return -1;
    }

    monitor2proc_hook();

    auto thread_sock_data = GET_THREAD_SOCK_DATA();
    auto iter = thread_data->fds_wr.begin(fd);
    interprocess_n_t *buffer = thread_sock_data->
            buffer[iter->buffer_idx].data;
    int peer_fd = thread_data->fds_datawithrd[fd].peer_fd;

    size_t total_size(0);
    for (int i = 0; i < iovcnt; ++i)
    {
        /*short startloc = buffer->b[0].pushdata(reinterpret_cast<uint8_t *>(iov[i].iov_base), iov[i].iov_len);
        if (startloc == -1) {
            errno = ENOMEM;
            return -1;
        }
        interprocess_t::queue_t::element ele;
        ele.command = interprocess_t::cmd::DATA_TRANSFER;
        ele.data_fd_rw.fd = peer_fd;
        ele.data_fd_rw.pointer = startloc;
        SW_BARRIER;
        buffer->q[0].push(ele);*/
        interprocess_t::queue_t::element ele;
        ele.command = interprocess_t::cmd::DATA_TRANSFER;
        ele.data_fd_rw.fd = peer_fd;
        buffer->push_data(ele,iov[i].iov_len, reinterpret_cast<uint8_t *>(iov[i].iov_base));
        total_size += iov[i].iov_len;
    }
    DEBUG("sent %d bytes to sockfd %d", total_size, fd);
    return total_size;
}

enum ITERATE_FD_IN_BUFFER_STATE
{
    CLOSED,
    FIND,
    NOTFIND,
    ALLCLOSED
};

#undef DEBUGON
#define DEBUGON 1
adjlist<file_struc_rd_t, MAX_FD_OWN_NUM, fd_rd_list_t, MAX_FD_PEER_NUM>::iterator recv_empty_hook
        (
                adjlist<file_struc_rd_t, MAX_FD_OWN_NUM, fd_rd_list_t, MAX_FD_PEER_NUM>::iterator iter,
                int myfd
        )
{
    thread_sock_data_t * thread_sock_data = GET_THREAD_SOCK_DATA();
    thread_data_t * thread_data = GET_THREAD_DATA();
    //if it is empty
    if (iter->child[0] != -1 || iter->child[1] != -1)
    {
        //if it has child
        if (dynamic_cast<interprocess_local_t *>(thread_sock_data->buffer[iter->buffer_idx].data)->q[1].isempty())
        {
            //remove itself and add its children
            if (iter->child[0] != -1)
                iter = thread_data->fds_datawithrd.add_element_at(iter, myfd, thread_data->rd_tree[iter->child[0]]);
            if (iter->child[1] != -1)
                iter = thread_data->fds_datawithrd.add_element_at(iter, myfd, thread_data->rd_tree[iter->child[1]]);

            //remove itself from adjlist
            iter = thread_data->fds_datawithrd.del_element(iter);
            return iter;
        }
    }
    else
    {
        //check whether clock pointer is point to itself, if not
        if (!(*(dynamic_cast<interprocess_local_t *>(thread_sock_data->buffer[iter->buffer_idx].data)
                ->sender_turn[1]))[thread_data->fds_datawithrd[myfd].peer_fd])
        {
            //send takeover msg to monitor
            metaqueue_ctl_element ele;
            ele.command = REQ_RELAY_RECV;
            ele.req_relay_recv.shmem = thread_sock_data->buffer[iter->buffer_idx].shmemkey;
            ele.req_relay_recv.req_fd = myfd;
            ele.req_relay_recv.peer_fd = thread_data->fds_datawithrd[myfd].peer_fd;
            thread_data->metaqueue.q[0].push(ele);
            DEBUG("Send takeover req for myfd %d on buffer idx %d since q empty and not pointed", myfd, iter->buffer_idx);

        } else 
        {
            //DEBUG("No need for takeover req");
        }
    }
    return iter.next();
};
#undef DEBUGON
#define DEBUGON 1
static inline ITERATE_FD_IN_BUFFER_STATE recvfrom_iter_fd_in_buf
        (int target_fd, 
         adjlist<file_struc_rd_t, MAX_FD_OWN_NUM, fd_rd_list_t, MAX_FD_PEER_NUM>::iterator& iter,
         interprocess_n_t::iterator &loc_in_buffer_has_blk,
         thread_data_t *thread_data, thread_sock_data_t *thread_sock_data)
{
    
    interprocess_n_t *buffer = (thread_sock_data->buffer[iter->buffer_idx].data);
    //uint8_t pointer = buffer->q[1].tail;
    auto iter_ele = buffer->begin();
    SW_BARRIER;
    while (true)
    {  //for same fd(buffer), iterate each available slot
        interprocess_t::queue_t::element ele;
        bool ele_isvalid, ele_isdel;
        std::tie(ele_isvalid, ele_isdel) = iter_ele.peek(ele);
        SW_BARRIER;
        if (!ele_isvalid)
            break;
        if (!ele_isdel)
        {
            if (ele.command == interprocess_t::cmd::CLOSE_FD)
            {
                if (ele.close_fd.peer_fd == target_fd &&
                    ele.close_fd.req_fd == thread_data->fds_datawithrd[target_fd].peer_fd)
                {
                    iter_ele.del();
                    DEBUG("Received close req of FD %d from peer FD %d", ele.close_fd.peer_fd, ele.close_fd.req_fd);
                    
                    iter = thread_data->fds_datawithrd.del_element(iter);
                    if (iter.end())
                    {
                        // Fix bug: close(fd) after the FD is in ALLCLOSED state return error.
                        // Even the FD is in ALLCLOSED state, the key should not be deleted.
                        // The application must call close() to release the FD resource.
                        DEBUG("FD %d is all closed", target_fd);
                        return ITERATE_FD_IN_BUFFER_STATE::ALLCLOSED;
                    }
                    return ITERATE_FD_IN_BUFFER_STATE::CLOSED; //no need to traverse this queue anyway
                } else {
                    if (!thread_data->fds_datawithrd.is_keyvalid(ele.close_fd.peer_fd))
                        iter_ele.del();
                }
            }

            if (ele.command == interprocess_t::cmd::DATA_TRANSFER &&
                ele.data_fd_rw.fd == target_fd)
            {
                loc_in_buffer_has_blk = iter_ele;
                return ITERATE_FD_IN_BUFFER_STATE::FIND;
            }
        }
        iter.next();
    }
    
    if (!thread_sock_data->buffer[iter->buffer_idx].isRDMA)
        iter = recv_empty_hook(iter, target_fd);
    return ITERATE_FD_IN_BUFFER_STATE::NOTFIND;
}

#undef DEBUGON
#define DEBUGON 1
/*
static void recv_takeover_traverse(int myfd, int idx, bool valid)
{
    //whether itself is the leaf
    thread_data_t * thread_data = GET_THREAD_DATA();
    thread_sock_data_t * thread_sock_data = GET_THREAD_SOCK_DATA();
    if ((thread_data->rd_tree[idx].child[0] == -1) &&
        (thread_data->rd_tree[idx].child[1] == -1))
    {
        //itself is the child
        if (valid)
        {
            metaqueue_ctl_element ele;
            ele.command = REQ_RELAY_RECV;
            ele.req_relay_recv.shmem = thread_sock_data->buffer[thread_data->rd_tree[idx].buffer_idx].shmemkey;
            ele.req_relay_recv.req_fd = myfd;
            ele.req_relay_recv.peer_fd = thread_data->fds_datawithrd[myfd].peer_fd;
            thread_data->metaqueue.q[0].push(ele);
            DEBUG("Send takeover message for key %u, my fd %d, peer fd %d", ele.req_relay_recv.shmem,
            ele.req_relay_recv.req_fd, ele.req_relay_recv.peer_fd);
        }
    } else
    {
        //it is not the leaf of the tree

        //first test whether it is needed to send takeover message
        bool istakeover(valid);
        istakeover = istakeover || ((thread_data->rd_tree[idx].status & FD_STATUS_RD_RECV_FORKED) &&
                                    !(thread_data->rd_tree[idx].status & FD_STATUS_RECV_REQ));
        //if it has the left child
        if (thread_data->rd_tree[idx].child[0] != -1)
            recv_takeover_traverse(myfd, thread_data->rd_tree[idx].child[0], istakeover);
        if (thread_data->rd_tree[idx].child[1] != -1)
            recv_takeover_traverse(myfd, thread_data->rd_tree[idx].child[1], istakeover);
    }
}

void send_recv_takeover_req(int fd)
{
    thread_data_t * thread_data = GET_THREAD_DATA();
    thread_sock_data_t * thread_sock_data = GET_THREAD_SOCK_DATA();
    // iterate all the fd in the read adjlist
    for (auto iter = thread_data->fds_datawithrd.begin(fd); !iter.end(); iter.next())
    {
        if (iter->child[0] != -1)
            recv_takeover_traverse(fd, iter->child[0],
                                   (iter->status & FD_STATUS_RD_RECV_FORKED) && !(iter->status & FD_STATUS_RECV_REQ));
        if (iter->child[1] != -1)
            recv_takeover_traverse(fd, iter->child[1],
                                   (iter->status & FD_STATUS_RD_RECV_FORKED) && !(iter->status & FD_STATUS_RECV_REQ));
    }
}

*/
#undef DEBUGON
#define DEBUGON 0
ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen)
{
    //test whether fd exists
    if (get_fd_type(sockfd) == FD_TYPE_SYSTEM) return ORIG(recvfrom, (get_real_fd(sockfd), buf, len, flags, src_addr, addrlen));
    sockfd = get_real_fd(sockfd);

    thread_data_t *thread_data = GET_THREAD_DATA();
    if ( ! thread_data->fds_datawithrd.is_keyvalid(sockfd)
            || thread_data->fds_datawithrd[sockfd].type != USOCKET_TCP_CONNECT
            || !thread_data->fds_datawithrd[sockfd].property.tcp.isopened)
    {
        errno = EBADF;
        return -1;
    }
    // all receive queues are closed, return 0 to indicate EOF
    if (thread_data->fds_datawithrd.begin(sockfd).end())
    {
        return 0;
    }

    //hook for all the process
    monitor2proc_hook();

    //send recv relay message
    /*if ((thread_data->fds_datawithrd[sockfd].property.status & FD_STATUS_RD_RECV_FORKED) &&
            !(thread_data->fds_datawithrd[sockfd].property.status & FD_STATUS_RECV_REQ))
    {
        send_recv_takeover_req(sockfd);
        thread_data->fds_datawithrd[sockfd].property.status &= FD_STATUS_RECV_REQ;
    }*/

    auto thread_sock_data = GET_THREAD_SOCK_DATA();
    interprocess_n_t *buffer_has_blk(nullptr);
    interprocess_n_t::iterator loc_has_blk(nullptr);
    bool isFind(false);
    int ret(-1);
    do //if blocking infinate loop
    {
        auto iter = thread_data->fds_datawithrd.begin(sockfd);
        while (true) //iterate different peer fd
        {
            interprocess_n_t::iterator ret_loc(nullptr);
            bool isFin(false);
            interprocess_n_t * buffer = (thread_sock_data->buffer[iter->buffer_idx].data);
            ITERATE_FD_IN_BUFFER_STATE ret_state = recvfrom_iter_fd_in_buf(sockfd, iter, ret_loc, thread_data, thread_sock_data);
            switch (ret_state)
            {
                case ITERATE_FD_IN_BUFFER_STATE::ALLCLOSED:
                    return 0;
                case ITERATE_FD_IN_BUFFER_STATE::CLOSED:
                    break;
                case ITERATE_FD_IN_BUFFER_STATE::FIND:
                    buffer_has_blk = buffer;
                    loc_has_blk = ret_loc;

                    // pop the block
                    //printf("found blk loc %d\n", loc_has_blk);
                    interprocess_t::queue_t::element ele;
                    loc_has_blk.peek(ele);
                    ret = buffer_has_blk->pop_data(&loc_has_blk, len, (void *) buf);
                    if (ret > 0)
                        isFind = true;
                    /*
                    short blk = buffer_has_blk->b[1].popdata(ele.data_fd_rw.pointer, ret, (uint8_t *) buf);
                    if (blk == -1)
                    {
                        SW_BARRIER;
                        buffer_has_blk->q[1].del(loc_has_blk);
                        SW_BARRIER;
                    } else
                    {
                        ele.data_fd_rw.pointer = blk;
                        buffer_has_blk->q[1].set(loc_has_blk, ele);
                    }*/
                    break;
                case ITERATE_FD_IN_BUFFER_STATE::NOTFIND:
                    if (iter.end())
                        isFin=true;
                    break;
            }
            if (isFin) break; //Finish iterate all peer fds
            if (isFind) break; //Get the requested block
        }
        if (isFind) break;
        monitor2proc_hook();
    } while (thread_data->fds_datawithrd[sockfd].property.is_blocking);
    if (!isFind)
    {
        errno = EAGAIN | EWOULDBLOCK;
        return -1;
    }
    DEBUG("received %d bytes from sockfd %d", ret, sockfd);
    return ret;
}
#undef DEBUGON

ssize_t __recvfrom_chk(int __fd, void *__buf, size_t __nbytes, size_t __buflen, int __flags, struct sockaddr *__from, socklen_t *__fromlen)
{
    if (get_fd_type(__fd) == FD_TYPE_SYSTEM) return ORIG(__recvfrom_chk, (get_real_fd(__fd), __buf, __nbytes, __buflen, __flags, __from, __fromlen));
    return recvfrom(__fd, __buf, __nbytes, __flags, __from, __fromlen);
}

// return revents
// called by poll_lib.cpp
int check_sockfd_receive(int sockfd)
{
    thread_data_t *thread_data = GET_THREAD_DATA();
    if (!thread_data->fds_datawithrd.is_keyvalid(sockfd))
    {
        errno = EBADF;
        //ERROR("non-existing sockfd %d passed to epoll", sockfd);
        return POLLERR;
    }
    
    if (thread_data->fds_datawithrd[sockfd].type == USOCKET_TCP_LISTEN)
    {
        if (thread_data->metaqueue.q[1].isempty()) { // no incoming connections
            // if no pending new connections from the monitor, send a command to notify monitor
            if (!thread_data->fds_datawithrd[sockfd].property.is_accept_command_sent) {
                metaqueue_ctl_element accept_command;
                accept_command.command = REQ_ACCEPT;
                accept_command.req_accept.port = thread_data->fds_datawithrd[sockfd].property.tcp.port;
                thread_data->metaqueue.q[0].push(accept_command);
                thread_data->fds_datawithrd[sockfd].property.is_accept_command_sent = true; 
            }
        }
        else { // has some incoming connection, return event
            return POLLIN;
        }
    }

    if (thread_data->fds_datawithrd.begin(sockfd).end() || thread_data->fds_datawithrd[sockfd].type != USOCKET_TCP_CONNECT
            || !thread_data->fds_datawithrd[sockfd].property.tcp.isopened)
    {
        return 0;
    }

    //hook for all the process
    monitor2proc_hook();

    //send recv relay message
    /*if ((thread_data->fds_datawithrd[sockfd].property.status & FD_STATUS_RD_RECV_FORKED) &&
            !(thread_data->fds_datawithrd[sockfd].property.status & FD_STATUS_RECV_REQ))
    {
        send_recv_takeover_req(sockfd);
        thread_data->fds_datawithrd[sockfd].property.status &= FD_STATUS_RECV_REQ;
    }*/

    auto thread_sock_data = GET_THREAD_SOCK_DATA();
    interprocess_n_t *buffer_has_blk(nullptr);
    interprocess_n_t::iterator loc_has_blk(nullptr);
    bool isFind(false);
    auto iter = thread_data->fds_datawithrd.begin(sockfd);
    while (true) //iterate different peer fd
    {
        interprocess_n_t::iterator ret_loc(nullptr);
        bool isFin(false);
        interprocess_n_t * buffer = (thread_sock_data->buffer[iter->buffer_idx].data);
        ITERATE_FD_IN_BUFFER_STATE ret_state = recvfrom_iter_fd_in_buf(sockfd, iter, ret_loc, thread_data, thread_sock_data);
        switch (ret_state)
        {
            case ITERATE_FD_IN_BUFFER_STATE::ALLCLOSED:
                return 0;
            case ITERATE_FD_IN_BUFFER_STATE::CLOSED:
                break;
            case ITERATE_FD_IN_BUFFER_STATE::FIND:
                buffer_has_blk = buffer;
                loc_has_blk = ret_loc;
                isFind=true;
                break;
            case ITERATE_FD_IN_BUFFER_STATE::NOTFIND:
                if (iter.end())
                    isFin=true;
                break;
        }
        if (isFin) break; //Finish iterate all peer fds
        if (isFind) break; //Get the requested block
    }

    if (isFind)
        return POLLIN;
    else
        return 0;
}

// return revents
// called by poll_lib.cpp
int check_sockfd_send(int sockfd)
{
    int fd = sockfd;
    thread_data_t *thread_data = GET_THREAD_DATA();
    if (!thread_data->fds_datawithrd.is_keyvalid(sockfd))
    {
        errno = EBADF;
        ERROR("non-existing sockfd %d passed to epoll", sockfd);
        return POLLERR;
    }
    
    if (thread_data->fds_datawithrd[sockfd].type == USOCKET_TCP_LISTEN)
    {
        return 0;
    }

    if (thread_data->fds_wr.begin(fd).end() || thread_data->fds_datawithrd[fd].type != USOCKET_TCP_CONNECT
            || !thread_data->fds_datawithrd[fd].property.tcp.isopened)
    {
        return 0;
    }

    monitor2proc_hook();

    auto thread_sock_data = GET_THREAD_SOCK_DATA();
    auto iter = thread_data->fds_wr.begin(fd);
    interprocess_n_t *buffer = thread_sock_data->
            buffer[iter->buffer_idx].data;
    bool is_full = buffer->is_full();

    if (!is_full)
        return POLLOUT;
    else
        return 0;
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
    if (get_fd_type(fd) == FD_TYPE_SYSTEM) return ORIG(readv, (get_real_fd(fd), iov, iovcnt));
    ssize_t ret(0);
    bool iserror(false);
    for (int i=0;i<iovcnt;++i)
    {
        ssize_t lret;
        lret =  recvfrom(fd, iov[i].iov_base, iov[i].iov_len, 0, NULL, NULL);
        if (lret == -1)
            iserror=true;
        else ret += lret;
    }
    if (iserror)
        return -1;
    else
        return ret;
}

ssize_t read(int fildes, void *buf, size_t nbyte)
{
    if (get_fd_type(fildes) == FD_TYPE_SYSTEM) return ORIG(read, (get_real_fd(fildes), buf, nbyte));
    return recvfrom(fildes,buf,nbyte,0,NULL,NULL);
}

ssize_t __read_chk(int __fd, void *__buf, size_t __nbytes, size_t __buflen)
{
    if (get_fd_type(__fd) == FD_TYPE_SYSTEM) return ORIG(__read_chk, (get_real_fd(__fd), __buf, __nbytes, __buflen));
    return recvfrom(__fd, __buf, __nbytes, 0, NULL, NULL);
}

ssize_t write(int fildes, const void *buf, size_t nbyte)
{
    if (get_fd_type(fildes) == FD_TYPE_SYSTEM) return ORIG(write, (get_real_fd(fildes), buf, nbyte));
    iovec iov;
    iov.iov_len=nbyte;
    iov.iov_base=(void *)buf;
    return writev(fildes,&iov,1);
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset)
{
    if (get_fd_type(fd) == FD_TYPE_SYSTEM) return ORIG(pread, (get_real_fd(fd), buf, count, offset));
    // socket cannot be read from an offset
    errno = EINVAL;
    return -1;
}

ssize_t pread64(int fd, void *buf, size_t count, off_t offset)
{
    if (get_fd_type(fd) == FD_TYPE_SYSTEM) return ORIG(pread64, (get_real_fd(fd), buf, count, offset));
    // socket cannot be read from an offset
    errno = EINVAL;
    return -1;
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset)
{
    if (get_fd_type(fd) == FD_TYPE_SYSTEM) return ORIG(pwrite, (get_real_fd(fd), buf, count, offset));
    // socket cannot be written to an offset
    errno = EINVAL;
    return -1;
}

ssize_t pwrite64(int fd, const void *buf, size_t count, off_t offset)
{
    if (get_fd_type(fd) == FD_TYPE_SYSTEM) return ORIG(pwrite64, (get_real_fd(fd), buf, count, offset));
    // socket cannot be written to an offset
    errno = EINVAL;
    return -1;
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags)
{
    if (get_fd_type(sockfd) == FD_TYPE_SYSTEM) return ORIG(send, (get_real_fd(sockfd), buf, len, flags));
    // flags are ignored now
    return write(sockfd, buf, len);
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
    const struct sockaddr *dest_addr, socklen_t addrlen)
{
    if (get_fd_type(sockfd) == FD_TYPE_SYSTEM) return ORIG(sendto, (get_real_fd(sockfd), buf, len, flags, dest_addr, addrlen));
    // flags, dest_addr, addrlen are ignored now
    return send(sockfd, buf, len, flags);
}

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags)
{
    if (get_fd_type(sockfd) == FD_TYPE_SYSTEM) return ORIG(sendmsg, (get_real_fd(sockfd), msg, flags));
    // flags are ignored now
    return writev(sockfd, msg->msg_iov, msg->msg_iovlen);
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
    if (get_fd_type(sockfd) == FD_TYPE_SYSTEM) return ORIG(recv, (get_real_fd(sockfd), buf, len, flags));
    // flags are ignored now
    return read(sockfd, buf, len);
}

ssize_t __recv_chk(int __fd, void *__buf, size_t __nbytes, size_t __buflen, int __flags)
{
    if (get_fd_type(__fd) == FD_TYPE_SYSTEM) return ORIG(__recv_chk, (get_real_fd(__fd), __buf, __nbytes, __buflen, __flags));
    return recvfrom(__fd, __buf, __nbytes, 0, NULL, NULL);
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags)
{
    if (get_fd_type(sockfd) == FD_TYPE_SYSTEM) return ORIG(recvmsg, (get_real_fd(sockfd), msg, flags));
    // flags are ignored now
    return readv(sockfd, msg->msg_iov, msg->msg_iovlen);
}

int dup(int oldfd)
{
    if (get_fd_type(oldfd) == FD_TYPE_SYSTEM)
        return alloc_virtual_fd(FD_TYPE_SYSTEM, ORIG(dup, (get_real_fd(oldfd))));

    // not implemented for now
    ERROR("dup fd %d not implemented for socket", oldfd);
    errno = ENOTSUP;
    return -1;
}

int dup2(int oldfd, int newfd)
{
    if (get_fd_type(oldfd) == FD_TYPE_SYSTEM) {
        int real_fd = ORIG(socket, (AF_INET, SOCK_STREAM, 0));
        if (real_fd < 0) {
            errno = ENOMEM;
            return -1;
        }
        real_fd = ORIG(dup2, (get_real_fd(oldfd), real_fd));
        set_fd_type(newfd, FD_TYPE_SYSTEM, real_fd);
        return newfd;
    }

    // not implemented for now
    ERROR("dup2 fd %d -> %d not implemented for socket", oldfd, newfd);
    errno = ENOTSUP;
    return -1;
}

int dup3 (int oldfd, int newfd, int flags)
{
     if (get_fd_type(oldfd) == FD_TYPE_SYSTEM) {
        int real_fd = ORIG(socket, (AF_INET, SOCK_STREAM, 0));
        if (real_fd < 0) {
            errno = ENOMEM;
            return -1;
        }
        real_fd = ORIG(dup3, (get_real_fd(oldfd), real_fd, flags));
        set_fd_type(newfd, FD_TYPE_SYSTEM, real_fd);
        return newfd;
    }

    // not implemented for now
    ERROR("dup3 fd %d -> %d not implemented for socket", oldfd, newfd);
    errno = ENOTSUP;
    return -1;
}

ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count)
{
    if (get_fd_type(in_fd) == FD_TYPE_SYSTEM && get_fd_type(out_fd) == FD_TYPE_SYSTEM) {
        return ORIG(sendfile, (get_real_fd(out_fd), get_real_fd(in_fd), offset, count));
    }

    // not implemented for now
    errno = ENOTSUP;
    return -1;
}

int socketpair(int domain, int type, int protocol, int sv[2])
{
	int ret = ORIG(socketpair, (domain, type, protocol, sv));
	if (ret == 0) {
        sv[0] = alloc_virtual_fd(FD_TYPE_SYSTEM, sv[0]);
        sv[1] = alloc_virtual_fd(FD_TYPE_SYSTEM, sv[1]);
    }
    return ret;
}

int pipe(int pipefd[2])
{
    int ret = ORIG(pipe, (pipefd));
    if (ret == 0) {
        pipefd[0] = alloc_virtual_fd(FD_TYPE_SYSTEM, pipefd[0]);
        pipefd[1] = alloc_virtual_fd(FD_TYPE_SYSTEM, pipefd[1]);
    }
    return ret;
}

int pipe2(int pipefd[2], int flags)
{
    int ret = ORIG(pipe2, (pipefd, flags));
    if (ret == 0) {
        pipefd[0] = alloc_virtual_fd(FD_TYPE_SYSTEM, pipefd[0]);
        pipefd[1] = alloc_virtual_fd(FD_TYPE_SYSTEM, pipefd[1]);
    }
    return ret;
}
