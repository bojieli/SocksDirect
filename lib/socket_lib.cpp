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

pthread_key_t pthread_sock_key;
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



void usocket_init()
{
    thread_data_t *data;
    data = reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
    file_struc_rd_t _fd;
    data->fds_datawithrd.init(0,_fd);
    data->fds_wr.init(0, 0);
    auto *thread_sock_data = new thread_sock_data_t;
    thread_sock_data->bufferhash = new std::unordered_map<key_t, int>;
    for (int i = 0; i < BUFFERNUM; ++i) thread_sock_data->buffer[i].isvalid = false;
    thread_sock_data->lowest_available = 0;
    pthread_setspecific(pthread_sock_key, reinterpret_cast<void *>(thread_sock_data));
}

int socket(int domain, int type, int protocol) __THROW
{
    if ((domain != AF_INET) || (type != SOCK_STREAM)) return ORIG(socket, (domain, type, protocol));
    thread_data_t *data;
    data = reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
    file_struc_rd_t nfd;
    nfd.property.is_addrreuse = 0;
    nfd.property.is_blocking = 1;
    nfd.property.tcp.isopened = false;
    unsigned int idx_nfd=data->fds_datawithrd.add_key(nfd);
    data->fds_wr.add_key(0);
    //assert(idx_nfd == idx_nfd_wr);
    int ret = MAX_FD_ID - idx_nfd;
    return ret;
}

int bind(int socket, const struct sockaddr *address, socklen_t address_len) __THROW
{
    if (socket < FD_DELIMITER) return ORIG(bind, (socket, address, address_len));
    socket = MAX_FD_ID - socket;
    unsigned short port;
    thread_data_t *data = nullptr;
    data = reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
    port = ntohs(((struct sockaddr_in *) address)->sin_port);
    data->fds_datawithrd[socket].property.tcp.port = port;
    return 0;
}

int listen(int socket, int backlog) __THROW
{
    if (socket < FD_DELIMITER) return ORIG(listen, (socket, backlog));
    socket = MAX_FD_ID - socket;
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

int close(int fildes)
{
    if (fildes < FD_DELIMITER) return ORIG(close, (fildes));
    fildes = MAX_FD_ID - fildes;
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
    //init
    if (socket < FD_DELIMITER) return ORIG(connect, (socket, address, address_len));
    socket = MAX_FD_ID - socket;
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

    //send to monitor and get respone
    metaqueue_ctl_element req_data, res_data;
    req_data.command = REQ_CONNECT;
    req_data.req_connect.fd = socket;
    req_data.req_connect.port = port;
    req_data.req_connect.isRDMA = isRDMA;

    q2monitor->q[0].push(req_data);
    while (!q2monitor->q[1].pop_nb(res_data));
    if (res_data.command != RES_SUCCESS)
        return -1;
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

            interprocess_t::queue_t::element ele;
            ele.command = interprocess_t::cmd::RDMA_ACK;
            thread_buf->buffer[idx].data->push_data(ele, 0, nullptr);
            printf("Connect Finished\n");
            //while (1);

        }
    }
    auto peerfd_iter = data->fds_datawithrd.add_element(socket, peer_fd_rd);
    fd_wr_list_t peer_fd_wr;
    peer_fd_wr.status = 0;
    peer_fd_wr.buffer_idx = peer_fd_rd.buffer_idx;
    auto wr_iter = data->fds_wr.add_element(socket, peer_fd_wr);
    data->fds_wr.set_ptr_to(socket, wr_iter);


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
                DEBUG("peer fd: %d\n",ele.data_fd_notify.fd);
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
    return 0;
}


int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags)
{
    if (sockfd < FD_DELIMITER) return ORIG(accept4, (sockfd, addr, addrlen, flags));
    sockfd = MAX_FD_ID - sockfd;
    thread_data_t *data;
    data = reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
    thread_sock_data_t *sock_data;
    sock_data = reinterpret_cast<thread_sock_data_t *>(pthread_getspecific(pthread_sock_key));

    metaqueue_ctl_element element;
    while (!data->metaqueue.q[1].pop_nb(element));
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
    DEBUG("Accept new connection, key %d, loc %d", key, loc);
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
            printf("Ack required\n");
            void* interprocess_t_baseaddr =  (void *)rdma_self_qpinfo->localptr;
            //Note: Client use the lower side to send
            sock_data->buffer[idx].data = new interprocess_local_t;
            dynamic_cast<interprocess_local_t *>(sock_data->buffer[idx].data)->init(interprocess_t_baseaddr, loc);
            dynamic_cast<interprocess_local_t *>(sock_data->buffer[idx].data)->
                    initRDMA(rdma_self_qpinfo->qp, rdma_self_qpinfo->send_cq,rdma_get_pack()->buf_mr->lkey,
                             peer_rdmainfo->qpinfo.rkey, remote_addr, interprocess_t_baseaddr, loc);

            interprocess_t::queue_t::element ele;
            while(!dynamic_cast<interprocess_local_t *>(sock_data->buffer[idx].data)->q[1].pop_nb(ele));
            printf("server conn fin\n");
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
    return MAX_FD_ID - idx_nfd;
}

int setsockopt(int socket, int level, int option_name, const void *option_value, socklen_t option_len)
{
    if (socket < FD_DELIMITER) return ORIG(setsockopt, (socket, level, option_name, option_value, option_len));
    socket = MAX_FD_ID - socket;
    if ((level == SOL_SOCKET) && (option_name == SO_REUSEPORT))
    {
        thread_data_t *thread;
        thread = reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
        thread->fds_datawithrd[socket].property.is_addrreuse = *reinterpret_cast<const int *>(option_value);
    }
    return 0;
}

int ioctl(int fildes, unsigned long request, ...) __THROW
{
    va_list p_args;
    va_start(p_args, request);
    char *argp = va_arg(p_args, char*);
    if (fildes < FD_DELIMITER)
        ORIG(ioctl, (fildes, request, argp));
    fildes = MAX_FD_ID - fildes;
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

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    if (fd < FD_DELIMITER) return ORIG(writev, (fd, iov, iovcnt));
    fd = MAX_FD_ID - fd;

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
#define DEBUGON 0
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
                    DEBUG("Received close req for %d from %d", ele.close_fd.peer_fd, ele.close_fd.req_fd);
                    
                    iter = thread_data->fds_datawithrd.del_element(iter);
                    if (iter.end())
                    {
                        DEBUG("Destroyed self fd %d.", target_fd);
                        thread_data->fds_datawithrd.del_key(target_fd);
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
    if (sockfd < FD_DELIMITER) return ORIG(recvfrom, (sockfd, buf, len, flags, src_addr, addrlen));
    sockfd = MAX_FD_ID - sockfd;

    thread_data_t *thread_data = GET_THREAD_DATA();
    if (thread_data->fds_datawithrd.begin(sockfd).end() || thread_data->fds_datawithrd[sockfd].type != USOCKET_TCP_CONNECT
            || !thread_data->fds_datawithrd[sockfd].property.tcp.isopened)
    {
        errno = EBADF;
        return -1;
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
        if (isFind) break;
        monitor2proc_hook();
    } while (thread_data->fds_datawithrd[sockfd].property.is_blocking);
    int ret(len);
    if (!isFind)
    {
        errno = EAGAIN | EWOULDBLOCK;
        return -1;
    } else {
        //printf("found blk loc %d\n", loc_has_blk);
        interprocess_t::queue_t::element ele;
        loc_has_blk.peek(ele);
        ret = buffer_has_blk->pop_data(&loc_has_blk, ret, (void *) buf);
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
    }
    return ret;
}
#undef DEBUGON

ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
    if (fd < FD_DELIMITER) return ORIG(readv, (fd, iov, iovcnt));
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
    if (fildes < FD_DELIMITER) return ORIG(read, (fildes, buf, nbyte));
    return recvfrom(fildes,buf,nbyte,0,NULL,NULL);
}

ssize_t write(int fildes, const void *buf, size_t nbyte)
{
    if (fildes < FD_DELIMITER) return ORIG(write, (fildes, buf, nbyte));
    iovec iov;
    iov.iov_len=nbyte;
    iov.iov_base=(void *)buf;
    return writev(fildes,&iov,1);
}
