//
// Created by ctyi on 11/23/17.
//

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
    if ((iter=bufferhash->find(key)) == bufferhash->end())
        return -1;
    if (buffer[iter->second].isvalid)
        return iter->second;
    else return -1;
}

int thread_sock_data_t::newbuffer(key_t key, int loc) {
    buffer[lowest_available].isvalid = true;
    buffer[lowest_available].data.init(key, loc);
    ++total_num;
    if (total_num == BUFFERNUM)
        FATAL("Dynamic allocation not implemented!");
    int ret=lowest_available;
    ++lowest_available;
    return ret;
}


void usocket_init()
{
    thread_data_t* data;
    data = reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
    data->fd_own_num=0;
    data->fd_peer_num=0;
    data->fd_own_lowest_id = 0;
    data->fd_peer_lowest_id = 0;
    for (int i=0;i<MAX_FD_OWN_NUM;++i) data->fds[i].isvaild = 0;
    for (int i=0;i<MAX_FD_PEER_NUM;++i) data->adjlist[i].is_valid = 0;

    pthread_key_create(&pthread_sock_key, NULL);
    auto *thread_sock_data=new thread_sock_data_t;
    thread_sock_data->bufferhash = new std::unordered_map<key_t, int>;
    for (int i=0;i<BUFFERNUM;++i) thread_sock_data->buffer[i].isvalid = false;
    thread_sock_data->lowest_available = 0;
    pthread_setspecific(pthread_sock_key, reinterpret_cast<void *>(thread_sock_data));
}
int socket(int domain, int type, int protocol) __THROW
{
    if (domain != AF_INET) FATAL("unsupported socket domain");
    if (type != SOCK_STREAM) FATAL("unsupported socket type");
    thread_data_t* data;
    data = reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
    data->fds[data->fd_own_lowest_id].peer_fd_ptr = -1;
    data->fds[data->fd_own_lowest_id].property.is_addrreuse = 0;
    data->fds[data->fd_own_lowest_id].property.is_blocking = 1;
    data->fds[data->fd_own_lowest_id].isvaild = 1;
    int ret=MAX_FD_ID - data->fd_own_lowest_id;
    int n_fd_own_lowest_id=data->fd_own_lowest_id+1;
    while (n_fd_own_lowest_id != data->fd_own_lowest_id)
    {
        if (n_fd_own_lowest_id >= MAX_FD_OWN_NUM) {
            n_fd_own_lowest_id = 0;
            continue;
        }
        if (!data->fds[n_fd_own_lowest_id].isvaild) break;
        ++n_fd_own_lowest_id;
    }
    if (n_fd_own_lowest_id == data->fd_own_lowest_id)
        FATAL("fd is full, cannot insert new fd");
    data->fd_own_lowest_id = n_fd_own_lowest_id;
    ++data->fd_own_num;
    return ret;
}
int bind(int socket, const struct sockaddr *address, socklen_t address_len) __THROW
{
    if (socket < FD_DELIMITER) return ORIG(bind,(socket, address, address_len));
    socket = MAX_FD_ID - socket;
    unsigned short port;
    thread_data_t* data= nullptr;
    data = reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
    port=ntohs(((struct sockaddr_in*)address)->sin_port);
    data->fds[socket].property.tcp.port = port;
    return 0;
}
int listen(int socket, int backlog) __THROW
{
    if (socket < FD_DELIMITER) return ORIG(listen, (socket, backlog));
    socket = MAX_FD_ID - socket;
    metaqueue_element data2m, data_from_m;
    thread_data_t *data;
    data= reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
    data2m.is_valid = 1;
    data2m.data.sock_listen_command.command = REQ_LISTEN;
    if (!data->fds[socket].isvaild)
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
    //TODO: Notify the monitor
    thread_data_t* data=NULL;
    data = reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
    data->fds[fildes].isvaild = 0;
}
int connect(int socket, const struct sockaddr *address, socklen_t address_len)
{
    //init
    if (socket < FD_DELIMITER) return ORIG(connect, (socket, address, address_len));
    socket = MAX_FD_ID - socket;
    char addr_str[100];
    inet_ntop(AF_INET, ((void *)&((struct sockaddr_in *)address)->sin_addr), addr_str, address_len);
    if (strcmp(addr_str, "127.0.0.1")!=0)
        FATAL("not support unlocal address");
    if (address->sa_family != AF_INET)
        FATAL("Only support TCP connection");
    thread_data_t *data;
    data = reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
    if (!data->fds[socket].isvaild)
    {
        errno = EBADF;
        return -1;
    }
    data->fds[socket].type = USOCKET_TCP_CONNECT;
    unsigned short port;
    port = ntohs(((struct sockaddr_in*)address)->sin_port);

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
    key_t key=res_data.data.sock_connect_res.shm_key;
    int loc=res_data.data.sock_connect_res.loc;

    //init buffer
    thread_sock_data_t *thread_buf;
    thread_buf = reinterpret_cast<thread_sock_data_t *>(pthread_getspecific(pthread_sock_key));
    if (thread_buf == nullptr)
        FATAL("Failed to get thread specific data.");
    int idx;
    //TODO: dynamic allocate memory
    int current_fds_idx = data->fd_peer_lowest_id;
    data->adjlist[current_fds_idx].next = data->fds[socket].peer_fd_ptr;
    data->fds[socket].peer_fd_ptr = current_fds_idx;
    ++data->fd_peer_lowest_id;
    ++data->fd_peer_num;
    data->adjlist[current_fds_idx].is_valid = 1;
    data->adjlist[current_fds_idx].fd = -1;
    if ((idx = thread_buf->isexist(res_data.data.sock_connect_res.shm_key)) != -1)
        data->adjlist[current_fds_idx].buffer_idx = idx;
    else
        idx = data->adjlist[current_fds_idx].buffer_idx = thread_buf->newbuffer(key, loc);

    //wait for ACK from peer
    interprocess_t *buffer;
    interprocess_t::queue_t::element element;
    buffer=&thread_buf->buffer[idx].data;
    bool isFind=false;
    while (true)
    {
        for (int i=0;i<INTERPROCESS_SLOTS_IN_BUFFER;++i)
        {
            buffer->q[1].peek(i, element);
            if (!element.isvalid || element.isdel) continue;
            if (element.command == interprocess_t::cmd::NEW_FD)
            {
                data->adjlist[current_fds_idx].fd = element.data_fd_notify.fd;
                buffer->q[1].del(i);
                isFind=true;
                break;
            }
        }
        if (isFind) break;
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
    if (!data->fds[sockfd].isvaild || data->fds[sockfd].type!=USOCKET_TCP_LISTEN)
    {
        errno = EBADF;
        return -1;
    }
    if (element.data.sock_connect_res.port != data->fds[sockfd].property.tcp.port)
        FATAL("Incorrect accept order for different ports");
    key_t key=element.data.sock_connect_res.shm_key;
    int loc=element.data.sock_connect_res.loc;
    auto peer_fd=element.data.sock_connect_res.fd;
    DEBUG("Accept new connection, key %d, loc %d", key, loc);
    int curr_fd=data->fd_own_lowest_id;
    data->fds[curr_fd].peer_fd_ptr = -1;
    data->fds[curr_fd].property.is_addrreuse = 0;
    data->fds[curr_fd].property.is_blocking = 0;
    data->fds[curr_fd].isvaild = 1;
    data->fds[curr_fd].type = USOCKET_TCP_CONNECT;
    ++data->fd_own_lowest_id;
    ++data->fd_own_num;
    //TODO: cyclic data_fd_own_lowest_id
    data->adjlist[data->fd_peer_lowest_id].fd = peer_fd;
    data->adjlist[data->fd_peer_lowest_id].next = data->fds[curr_fd].peer_fd_ptr;
    data->fds[curr_fd].peer_fd_ptr = data->fd_peer_lowest_id;
    int idx;
    if ((idx = sock_data->isexist(key)) != -1)
        data->adjlist[curr_fd].buffer_idx = idx;
    else
        idx = data->adjlist[curr_fd].buffer_idx = sock_data->newbuffer(key, loc);
    ++data->fd_peer_lowest_id;
    ++data->fd_peer_num;
    //TODO:cyclic fd_peer_lowest_id

    interprocess_t *buffer=&sock_data->buffer[idx].data;
    interprocess_t::queue_t::element inter_element;
    inter_element.isdel = 0;
    inter_element.isvalid = 1;
    inter_element.command = interprocess_t::cmd::NEW_FD;
    inter_element.data_fd_notify.fd = curr_fd;
    buffer->q[0].push(inter_element);
    return MAX_FD_ID-curr_fd;
}

int setsockopt(int socket, int level, int option_name, const void* option_value, socklen_t option_len)
{
    if (socket < FD_DELIMITER) return ORIG(setsockopt, (socket, level, option_name, option_value, option_len));
    socket = MAX_FD_ID - socket;
    if ((level == SOL_SOCKET) && (option_name==SO_REUSEADDR))
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
    char * argp=va_arg(p_args, char*);
    if (fildes < FD_DELIMITER);
        ORIG(ioctl, (fildes, request, argp));
    fildes = MAX_FD_ID - fildes;
    thread_data_t *thread_data= reinterpret_cast<thread_data_t *>(pthread_getspecific(pthread_key));
    if (!thread_data->fds[fildes].isvaild)
    {
        errno=EBADF;
        return -1;
    }

    if (request == FIONBIO)
        thread_data->fds[fildes].property.is_blocking=0;
    return 0;
}