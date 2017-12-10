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
    data->fds.init();
    data->adjlist.init();

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
    file_struc_t nfd;
    nfd.peer_fd_ptr = -1;
    nfd.property.is_addrreuse = 0;
    nfd.property.is_blocking = 1;
    nfd.property.tcp.isopened = false;
    unsigned int idx_nfd=data->fds.add(nfd);
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
    data->fds[socket].property.tcp.port = port;
    return 0;
}

int listen(int socket, int backlog) __THROW
{
    if (socket < FD_DELIMITER) return ORIG(listen, (socket, backlog));
    socket = MAX_FD_ID - socket;
    metaqueue_element data2m, data_from_m;
    thread_data_t *data;
    data = reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
    data2m.is_valid = 1;
    data2m.data.sock_listen_command.command = REQ_LISTEN;
    if (!data->fds.isvalid(socket))
    {
        errno = EBADF;
        return -1;
    }
    data->fds[socket].type = USOCKET_TCP_LISTEN;
    data2m.data.sock_listen_command.is_reuseaddr = data->fds[socket].property.is_addrreuse;
    data2m.data.sock_listen_command.port = data->fds[socket].property.tcp.port;
    metaqueue_pack q_pack;
    q_pack.data = &data->metaqueue[0];
    q_pack.meta = &data->metaqueue_metadata[0];
    metaqueue_push(q_pack, &data2m);
    q_pack.data = &data->metaqueue[1];
    q_pack.meta = &data->metaqueue_metadata[1];
    metaqueue_pop(q_pack, &data_from_m);
    if (data_from_m.data.res_error.command == RES_ERROR) return -1;
    else return 0;
}

int close(int fildes)
{
    if (fildes < FD_DELIMITER) return ORIG(close, (fildes));
    fildes = MAX_FD_ID - fildes;
    thread_data_t *data = NULL;
    data = reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
    if (!data->fds.isvalid(fildes))
    {
        errno = EBADF;
        return -1;
    }
    if (data->fds[fildes].type == USOCKET_TCP_CONNECT)
    {
        thread_sock_data_t* sock_data=GET_THREAD_SOCK_DATA();
        for(int idx = data->fds[fildes].peer_fd_ptr;idx != -1;idx=data->adjlist[idx].next)
        {
            fd_list_t peer_adj_item(data->adjlist[idx]);
            interprocess_t *interprocess;
            interprocess = &sock_data->buffer[peer_adj_item.buffer_idx].data;
            interprocess_t::queue_t::element ele;
            ele.command=interprocess_t::cmd::CLOSE_FD;
            ele.close_fd.req_fd=fildes;
            ele.close_fd.peer_fd=peer_adj_item.fd;
            interprocess->q[0].push(ele);
        }
    }

    if (data->fds[fildes].type == USOCKET_TCP_LISTEN)
    {
        metaqueue_element ele;
        ele.data.command.command=REQ_CLOSE;
        ele.data.res_close.port=data->fds[fildes].property.tcp.port;
        ele.data.res_close.listen_fd=fildes;
        metaqueue_pack q_pack;
        q_pack.data = &data->metaqueue[0];
        q_pack.meta = &data->metaqueue_metadata[0];
        metaqueue_push(q_pack, &ele);
    }
    data->fds[fildes].property.tcp.isopened=false;
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
    if (!data->fds.isvalid(socket))
    {
        errno = EBADF;
        return -1;
    }
    if (data->fds[socket].property.tcp.isopened) close(socket);
    data->fds[socket].type = USOCKET_TCP_CONNECT;
    data->fds[socket].property.tcp.isopened=true;
    data->fds[socket].next_op_fd = data->fds[socket].peer_fd_ptr = -1;
    unsigned short port;
    port = ntohs(((struct sockaddr_in *) address)->sin_port);

    //send to monitor and get respone
    metaqueue_element req_data, res_data;
    req_data.data.sock_connect_command.command = REQ_CONNECT;
    req_data.data.sock_connect_command.fd = socket;
    req_data.data.sock_connect_command.port = port;
    metaqueue_pack q_pack;
    q_pack.data = &data->metaqueue[0];
    q_pack.meta = &data->metaqueue_metadata[0];
    metaqueue_push(q_pack, &req_data);
    q_pack.data = &data->metaqueue[1];
    q_pack.meta = &data->metaqueue_metadata[1];
    metaqueue_pop(q_pack, &res_data);
    if (res_data.data.sock_connect_res.command != RES_SUCCESS)
        return -1;
    //printf("%d\n", res_data.data.sock_connect_res.shm_key);
    key_t key = res_data.data.sock_connect_res.shm_key;
    int loc = res_data.data.sock_connect_res.loc;

    //init buffer
    thread_sock_data_t *thread_buf;
    thread_buf = reinterpret_cast<thread_sock_data_t *>(pthread_getspecific(pthread_sock_key));
    if (thread_buf == nullptr)
        FATAL("Failed to get thread specific data.");
    int idx;
    fd_list_t peer_fd;
    unsigned int idx_peer_fd;
    peer_fd.next = data->fds[socket].peer_fd_ptr;
    peer_fd.fd = -1;
    idx_peer_fd = data->adjlist.add(peer_fd);
    data->fds[socket].peer_fd_ptr = idx_peer_fd;
    data->fds[socket].next_op_fd = idx_peer_fd;
    if ((idx = thread_buf->isexist(res_data.data.sock_connect_res.shm_key)) != -1)
        data->adjlist[idx_peer_fd].buffer_idx = idx;
    else
        idx = data->adjlist[idx_peer_fd].buffer_idx = thread_buf->newbuffer(key, loc);

    //wait for ACK from peer
    volatile interprocess_t *buffer;
    interprocess_t::queue_t::element element;
    buffer = &thread_buf->buffer[idx].data;
    bool isFind = false;
    while (true)
    {
        SW_BARRIER;
        buffer->q[1].peek(buffer->q[1].tail, element);
        //printf("head peek %d\n", buffer->q[1].tail);
        SW_BARRIER;
        for (unsigned int i = buffer->q[1].tail; element.isvalid; ++i)
        {
            if (!element.isvalid || element.isdel)
            {
                //printf("%d peeked\n", i+1);
                buffer->q[1].peek((i+1) & INTERPROCESS_Q_MASK, element);
                continue;
            }
            if (element.command == interprocess_t::cmd::NEW_FD)
            {
                if (element.data_fd_notify.fd == 0){
                    buffer->q[1].peek((i+1) & INTERPROCESS_Q_MASK, element);
                    continue;
                }
                    data->adjlist[idx_peer_fd].fd = element.data_fd_notify.fd;
                if (element.data_fd_notify.fd == 0) 
                    FATAL("error!");
                DEBUG("peer fd: %d\n",element.data_fd_notify.fd);
                SW_BARRIER;
                buffer->q[1].del(i & INTERPROCESS_Q_MASK);
                isFind = true;
                break;
            }
            SW_BARRIER;
            //printf("%d peeked\n", i+1);
            buffer->q[1].peek((i+1) & INTERPROCESS_Q_MASK, element);
            SW_BARRIER;
            //printf("Failed!\n");
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
    metaqueue_pack q_pack;
    q_pack.data = &data->metaqueue[1];
    q_pack.meta = &data->metaqueue_metadata[1];
    metaqueue_element element;
    metaqueue_pop(q_pack, &element);
    if (element.data.command.command != RES_NEWCONNECTION)
        FATAL("unordered accept response");
    if (!data->fds.isvalid(sockfd) || data->fds[sockfd].type != USOCKET_TCP_LISTEN)
    {
        errno = EBADF;
        return -1;
    }
    if (element.data.sock_connect_res.port != data->fds[sockfd].property.tcp.port)
        FATAL("Incorrect accept order for different ports");
    key_t key = element.data.sock_connect_res.shm_key;
    int loc = element.data.sock_connect_res.loc;
    auto peer_fd = element.data.sock_connect_res.fd;
    DEBUG("Accept new connection, key %d, loc %d", key, loc);
    file_struc_t nfd;
    fd_list_t npeerfd;

    nfd.peer_fd_ptr = -1;
    nfd.property.is_addrreuse = 0;
    nfd.property.is_blocking = (flags & SOCK_NONBLOCK)?0:1;
    nfd.type = USOCKET_TCP_CONNECT;
    nfd.property.tcp.isopened = true;
    unsigned int idx_nfd=data->fds.add(nfd);
    npeerfd.fd = peer_fd;
    npeerfd.is_ready = 1;
    npeerfd.next = nfd.peer_fd_ptr;
    unsigned int idx_npeerfd=data->adjlist.add(npeerfd);
    data->fds[idx_nfd].peer_fd_ptr = idx_npeerfd;
    data->fds[idx_nfd].next_op_fd = idx_npeerfd;

    int idx;
    if ((idx = sock_data->isexist(key)) != -1)
        data->adjlist[idx_npeerfd].buffer_idx = idx;
    else
        idx = data->adjlist[idx_npeerfd].buffer_idx = sock_data->newbuffer(key, loc);

    volatile interprocess_t *buffer = &sock_data->buffer[idx].data;
    interprocess_t::queue_t::element inter_element;
    inter_element.isdel = 0;
    inter_element.isvalid = 1;
    inter_element.command = interprocess_t::cmd::NEW_FD;
    inter_element.data_fd_notify.fd = idx_nfd;
    if (idx_nfd == 0) FATAL("error!");
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
        thread->fds[socket].property.is_addrreuse = *reinterpret_cast<const int *>(option_value);
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
    if (!thread_data->fds.isvalid(fildes))
    {
        errno = EBADF;
        return -1;
    }

    if (request == FIONBIO)
        thread_data->fds[fildes].property.is_blocking = 0;
    return 0;
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    if (fd < FD_DELIMITER) return ORIG(writev, (fd, iov, iovcnt));
    fd = MAX_FD_ID - fd;

    thread_data_t *thread_data = GET_THREAD_DATA();
    if (!thread_data->fds.isvalid(fd) || thread_data->fds[fd].type != USOCKET_TCP_CONNECT
            || !thread_data->fds[fd].property.tcp.isopened)
    {
        errno = EBADF;
        return -1;
    }

    auto thread_sock_data = GET_THREAD_SOCK_DATA();
    interprocess_t *buffer = &thread_sock_data->
            buffer[thread_data->adjlist[thread_data->fds[fd].next_op_fd].buffer_idx].data;
    int peer_fd = thread_data->adjlist[thread_data->fds[fd].next_op_fd].fd;
    thread_data->fds[fd].next_op_fd = thread_data->adjlist[thread_data->fds[fd].next_op_fd].next;
    if (thread_data->fds[fd].next_op_fd == -1)
        thread_data->fds[fd].next_op_fd = thread_data->fds[fd].peer_fd_ptr;

    size_t total_size(0);
    for (int i = 0; i < iovcnt; ++i)
    {
        short startloc = buffer->b[0].pushdata(reinterpret_cast<uint8_t *>(iov[i].iov_base), iov[i].iov_len);
        if (startloc == -1) return -1;
        total_size += iov[i].iov_len;
        interprocess_t::queue_t::element ele;
        ele.isvalid = 1;
        ele.isdel = 0;
        ele.command = interprocess_t::cmd::DATA_TRANSFER;
        ele.data_fd_rw.fd = peer_fd;
        ele.data_fd_rw.pointer = startloc;
        SW_BARRIER;
        buffer->q[0].push(ele);
    }
    return total_size;
}

#undef DEBUGON
#define DEBUGON 1
ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen)
{
    if (sockfd < FD_DELIMITER) return ORIG(recvfrom, (sockfd, buf, len, flags, src_addr, addrlen));
    sockfd = MAX_FD_ID - sockfd;

    thread_data_t *thread_data = GET_THREAD_DATA();
    if (!thread_data->fds.isvalid(sockfd) || thread_data->fds[sockfd].type != USOCKET_TCP_CONNECT
            || !thread_data->fds[sockfd].property.tcp.isopened)
    {
        errno = EBADF;
        return -1;
    }

    auto thread_sock_data = GET_THREAD_SOCK_DATA();
    int idx_curr_next_fd = thread_data->fds[sockfd].peer_fd_ptr;
    int prev_ptr=-1;
    bool isFind(false);
    volatile interprocess_t *buffer_has_blk(nullptr);
    int loc_has_blk(-1);

    do //if blocking, infinate loop
    {
        do //iterate different peer fd
        {
            fd_list_t* curr_peer_fd=&thread_data->adjlist[idx_curr_next_fd];
            volatile interprocess_t *buffer = &thread_sock_data->buffer[curr_peer_fd->buffer_idx].data;
            uint8_t pointer = buffer->q[1].tail;
            SW_BARRIER;
            bool isdel(false);
            while (1) {  //for same fd(buffer), iterate each available slot
                volatile auto ele = buffer->q[1].data->data[pointer];
                SW_BARRIER;
                if (!ele.isvalid)
                    break;
                if (!ele.isdel)
                {
                    if (ele.command == interprocess_t::cmd::CLOSE_FD &&
                        ele.close_fd.peer_fd == sockfd &&
                        ele.close_fd.req_fd == curr_peer_fd->fd)
                    {
                        isdel=true;
                        DEBUG("Received close req for %d from %d", ele.close_fd.peer_fd, ele.close_fd.req_fd);
                        //this is the first on the adjlist
                        if (prev_ptr == -1)
                        {
                            thread_data->fds[sockfd].peer_fd_ptr
                                    = thread_data->fds[sockfd].next_op_fd = curr_peer_fd->next;
                            int n_idx_curr_next_fd=curr_peer_fd->next;
                            thread_data->adjlist.del(idx_curr_next_fd);
                            idx_curr_next_fd = n_idx_curr_next_fd;
                            curr_peer_fd = nullptr;
                            //All the peer fd destructed
                            if (thread_data->fds[sockfd].peer_fd_ptr == -1)
                            {
                                DEBUG("Self fd fd %d destructed", sockfd);
                                thread_data->fds.del(sockfd);
                                return 0;
                            }
                        } else
                        {
                            thread_data->adjlist[prev_ptr].next = curr_peer_fd->next;
                            int n_idx_curr_next_fd = curr_peer_fd->next;
                            curr_peer_fd=nullptr;
                            thread_data->adjlist.del(idx_curr_next_fd);
                            idx_curr_next_fd = n_idx_curr_next_fd;
                        }
                        break; //no need to traverse this queue anyway
                    }
                    if (ele.command == interprocess_t::cmd::DATA_TRANSFER &&
                        ele.data_fd_rw.fd == sockfd)
                    {
                        isFind = true;
                        buffer_has_blk = buffer;
                        loc_has_blk = pointer;
                        break;
                    }
                }
                ++pointer;
            }
            if (isFind) break;
            if (!isdel)
            {
                prev_ptr = idx_curr_next_fd;
                idx_curr_next_fd = thread_data->adjlist[idx_curr_next_fd].next;
            }
            if (idx_curr_next_fd == -1) idx_curr_next_fd = thread_data->fds[sockfd].peer_fd_ptr;
        } while (idx_curr_next_fd != -1);
        if (isFind) break;
    } while (thread_data->fds[sockfd].property.is_blocking);
    int ret(len);
    if (!isFind)
    {
        errno = EAGAIN | EWOULDBLOCK;
        return -1;
    } else {
        //printf("found blk loc %d\n", loc_has_blk);
        auto ele = &buffer_has_blk->q[1].data->data[loc_has_blk];
        short blk = buffer_has_blk->b[1].popdata(ele->data_fd_rw.pointer, ret, (uint8_t *) buf);
        if (blk == -1)
        {
            SW_BARRIER;
            buffer_has_blk->q[1].del(loc_has_blk);
            SW_BARRIER;
        } else
        {
            ele->data_fd_rw.pointer = blk;
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