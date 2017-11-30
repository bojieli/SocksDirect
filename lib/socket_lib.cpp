//
// Created by ctyi on 11/23/17.
//

#include "lib.h"
#include "socket_lib.h"
#include "../common/helper.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "lib_internal.h"
#include "../common/metaqueue.h"

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
    data->fds->type = USOCKET_TCP_CONNECT;
    unsigned short port;
    port = ntohs(((struct sockaddr_in*)address)->sin_port);
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
    printf("%d\n", res_data.data.sock_connect_res.shm_key);
    if (res_data.data.sock_connect_res.command == RES_SUCCESS)
        return 0;
    return -1;
}