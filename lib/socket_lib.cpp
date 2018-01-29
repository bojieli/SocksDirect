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
#include "lib_internal.h"
#include "../common/metaqueue.h"
#include <sys/ioctl.h>

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
        file_struc_wr_t * curr_wr_handle_list = &thread_data->fds_wr[fd];
        for (int id_in_fd = curr_wr_handle_list->iterator_init(); id_in_fd != -1; id_in_fd = curr_wr_handle_list->iterator_next(id_in_fd))
        {
            if ((*curr_wr_handle_list)[id_in_fd].buffer_idx == old_buffer_id)
            {
                //match the old buffer, then create two new candicate into the list
                DEBUG("Add two candidate");
                fd_vec_t n_candicate[2];
                n_candicate[0].parent_id_in_v = n_candicate[1].parent_id_in_v = id_in_fd;
                n_candicate[0].buffer_idx = new_buffer_id[0];
                n_candicate[1].buffer_idx = new_buffer_id[1];
                n_candicate[0].status = n_candicate[1].status = 0;
            }
        }
    }
}

void monitor2proc_hook()
{
    thread_data_t *thread_data = GET_THREAD_DATA();
    thread_sock_data_t *thread_sock_data = GET_THREAD_SOCK_DATA();
    metaqueue_ctl_element ele;
    while (thread_data->metaqueue.q_emergency[1].pop_nb(ele))
    {
        if (ele.command == RES_PUSH_FORK) res_push_fork_handler(ele);
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
    buffer[lowest_available].data.init(key, loc);
    (*bufferhash)[key]=lowest_available;
    ++total_num;
    if (total_num == BUFFERNUM)
        FATAL("Dynamic allocation not implemented!");
    int ret = lowest_available;
    ++lowest_available;
    return ret;
}


void usocket_init()
{
    thread_data_t *data;
    data = reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
    file_struc_rd_t _fd;
    data->fds_datawithrd.init(0,_fd);
    file_struc_wr_t _fd_wr;
    data->fds_wr.init(0, _fd_wr);
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
    file_struc_wr_t nfd_wr;
    unsigned int idx_nfd_wr = data->fds_wr.add_key(nfd_wr);
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
            interprocess_t *interprocess;
            interprocess = &sock_data->buffer[peer_adj_item.buffer_idx].data;
            interprocess_t::queue_t::element ele;
            ele.command=interprocess_t::cmd::CLOSE_FD;
            ele.close_fd.req_fd=fildes;
            ele.close_fd.peer_fd=data->fds_datawithrd[fildes].peer_fd;
            interprocess->q[0].push(ele);
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

int connect(int socket, const struct sockaddr *address, socklen_t address_len)
{
    //init
    if (socket < FD_DELIMITER) return ORIG(connect, (socket, address, address_len));
    socket = MAX_FD_ID - socket;
    char addr_str[100];
    inet_ntop(AF_INET, ((void *) &((struct sockaddr_in *) address)->sin_addr), addr_str, address_len);
    if (strcmp(addr_str, "127.0.0.1") != 0)
        FATAL("not support unlocal address");
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
    unsigned short port;
    port = ntohs(((struct sockaddr_in *) address)->sin_port);

    //send to monitor and get respone
    metaqueue_ctl_element req_data, res_data;
    req_data.command = REQ_CONNECT;
    req_data.req_connect.fd = socket;
    req_data.req_connect.port = port;
    data->metaqueue.q[0].push(req_data);
    while (!data->metaqueue.q[1].pop_nb(res_data));
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
    unsigned int idx_peer_fd;
    peer_fd_rd.child[0] = peer_fd_rd.child[1] = -1;
    peer_fd_rd.status = 0;
    if ((idx = thread_buf->isexist(key)) != -1)
        peer_fd_rd.buffer_idx = idx;
    else
        idx = peer_fd_rd.buffer_idx = thread_buf->newbuffer(key, loc);
    auto peerfd_iter = data->fds_datawithrd.add_element(socket, peer_fd_rd);
    fd_wr_list_t peer_fd_wr;
    fd_vec_t wr_vec_ele;
    wr_vec_ele.buffer_idx = peer_fd_rd.buffer_idx;
    wr_vec_ele.status = 0;
    wr_vec_ele.parent_id_in_v = -1;
    int wr_idx_in_vec = data->fds_wr[socket].add(wr_vec_ele);
    peer_fd_wr.status = 0;
    peer_fd_wr.id_in_v = wr_idx_in_vec;
    peer_fd_wr.buffer_idx = peer_fd_rd.buffer_idx;
    data->fds_wr.add_element(socket, peer_fd_wr);

    //wait for ACK from peer
    interprocess_t *buffer;
    interprocess_t::queue_t::element element;
    buffer = &thread_buf->buffer[idx].data;
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
    }
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
        idx = npeerfd_rd.buffer_idx = sock_data->newbuffer(key, loc);
    npeerfd_rd.child[0] = npeerfd_rd.child[1] = -1;
    npeerfd_rd.status = 0;
    data->fds_datawithrd.add_element(idx_nfd, npeerfd_rd);

    file_struc_wr_t nfd_wr;
    fd_wr_list_t npeerfd_wr;
    fd_vec_t nfd_wr_vec_ele;
    nfd_wr_vec_ele.buffer_idx = idx;
    nfd_wr_vec_ele.status = 0;
    nfd_wr_vec_ele.parent_id_in_v = -1;
    npeerfd_wr.id_in_v = nfd_wr.add(nfd_wr_vec_ele);
    npeerfd_wr.buffer_idx = idx;
    npeerfd_wr.status = 0;

    data->fds_wr.add_key(nfd_wr);

    data->fds_wr.add_element(idx_nfd, npeerfd_wr);


    interprocess_t *buffer = &sock_data->buffer[idx].data;
    interprocess_t::queue_t::element inter_element;
    inter_element.command = interprocess_t::cmd::NEW_FD;
    inter_element.data_fd_notify.fd = idx_nfd;
    //printf("currfd: %d\n", curr_fd);
    SW_BARRIER;
    buffer->q[0].push(inter_element);
    return MAX_FD_ID - idx_nfd;
}

int setsockopt(int socket, int level, int option_name, const void *option_value, socklen_t option_len)
{
    if (socket < FD_DELIMITER) return ORIG(setsockopt, (socket, level, option_name, option_value, option_len));
    socket = MAX_FD_ID - socket;
    if ((level == SOL_SOCKET) && (option_name == SO_REUSEADDR))
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
    if (fildes < FD_DELIMITER);
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
    iter = iter.next();
    interprocess_t *buffer = &thread_sock_data->
            buffer[iter->buffer_idx].data;
    int peer_fd = thread_data->fds_datawithrd[fd].peer_fd;

    size_t total_size(0);
    for (int i = 0; i < iovcnt; ++i)
    {
        short startloc = buffer->b[0].pushdata(reinterpret_cast<uint8_t *>(iov[i].iov_base), iov[i].iov_len);
        if (startloc == -1) {
            errno = ENOMEM;
            return -1;
        }
        total_size += iov[i].iov_len;
        interprocess_t::queue_t::element ele;
        ele.command = interprocess_t::cmd::DATA_TRANSFER;
        ele.data_fd_rw.fd = peer_fd;
        ele.data_fd_rw.pointer = startloc;
        SW_BARRIER;
        buffer->q[0].push(ele);
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
#define DEBUGON 0
static inline ITERATE_FD_IN_BUFFER_STATE recvfrom_iter_fd_in_buf
        (int target_fd, 
         adjlist<file_struc_rd_t, MAX_FD_OWN_NUM, fd_rd_list_t, MAX_FD_PEER_NUM>::iterator& iter,
         int &loc_in_buffer_has_blk)
{
    thread_data_t *thread_data = GET_THREAD_DATA();
    auto thread_sock_data = GET_THREAD_SOCK_DATA();
    interprocess_t *buffer = &(thread_sock_data->buffer[iter->buffer_idx].data);
    uint8_t pointer = buffer->q[1].tail;
    SW_BARRIER;
    while (true)
    {  //for same fd(buffer), iterate each available slot
        interprocess_t::queue_t::element ele;
        bool ele_isvalid, ele_isdel;
        std::tie(ele_isvalid, ele_isdel) = buffer->q[1].peek(pointer, ele);
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
                    buffer->q[1].del(pointer);
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
                        buffer->q[1].del(pointer);
                }
            }

            if (ele.command == interprocess_t::cmd::DATA_TRANSFER &&
                ele.data_fd_rw.fd == target_fd)
            {
                loc_in_buffer_has_blk = pointer;
                return ITERATE_FD_IN_BUFFER_STATE::FIND;
            }
        }
        if (buffer->q[1].tail > pointer)
            pointer = buffer->q[1].tail;
        else
            ++pointer;
    }
    iter = iter.next();
    return ITERATE_FD_IN_BUFFER_STATE::NOTFIND;
}


#undef DEBUGON
#define DEBUGON 0
ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen)
{
    if (sockfd < FD_DELIMITER) return ORIG(recvfrom, (sockfd, buf, len, flags, src_addr, addrlen));
    sockfd = MAX_FD_ID - sockfd;

    thread_data_t *thread_data = GET_THREAD_DATA();
    if (thread_data->fds_datawithrd.begin(sockfd).end() || thread_data->fds_datawithrd[sockfd].type != USOCKET_TCP_CONNECT
            || !thread_data->fds_datawithrd[sockfd].property.tcp.isopened)
    {
        errno = EBADF;
        return -1;
    }

    monitor2proc_hook();

    auto thread_sock_data = GET_THREAD_SOCK_DATA();
    interprocess_t *buffer_has_blk(nullptr);
    int loc_has_blk(-1);
    bool isFind(false);
    do //if blocking infinate loop
    {
        int prev_adjlist_ptr=-1;
        auto iter = thread_data->fds_datawithrd.begin(sockfd);
        while (true) //iterate different peer fd
        {
            int ret_loc(-1);
            bool isFin(false);
            interprocess_t *buffer = &(thread_sock_data->buffer[iter->buffer_idx].data);
            ITERATE_FD_IN_BUFFER_STATE ret_state = recvfrom_iter_fd_in_buf(sockfd, iter, ret_loc);
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
    } while (thread_data->fds_datawithrd[sockfd].property.is_blocking);
    int ret(len);
    if (!isFind)
    {
        errno = EAGAIN | EWOULDBLOCK;
        return -1;
    } else {
        //printf("found blk loc %d\n", loc_has_blk);
        interprocess_t::queue_t::element ele;
        buffer_has_blk->q[1].peek(loc_has_blk, ele);
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
        }
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
