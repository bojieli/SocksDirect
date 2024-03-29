//
// Created by ctyi on 11/23/17.
//

#ifndef IPC_DIRECT_SOCKET_LIB_H
#define IPC_DIRECT_SOCKET_LIB_H
#include "../common/darray.hpp"
#include "../common/interprocess_t_n.hpp"
#include "rdma_lib.h"
#ifdef __cplusplus
#include <utility>
extern "C"
{
#endif
#define MAX_FD_ID 0x7FFFFFFF
#define  MAX_FD_OWN_NUM 1000
#define MAX_FD_PEER_NUM 1000
#define FD_DELIMITER 0x3FFFFFFF
#define CHECK_READ_PER_WRITE 64
enum
{
    USOCKET_TCP_LISTEN,
    USOCKET_TCP_CONNECT
};
typedef struct
{
    bool is_addrreuse;
    bool is_blocking;
    bool is_accept_command_sent; // TCP LISTEN only
    union
    {
        struct
        {
            unsigned short port;
            bool isopened;
        } tcp;
    };
    int status;
} socket_property_t;
typedef struct
{
    int type;
    int peer_fd;
    socket_property_t property;
} file_struc_rd_t;

typedef struct
{
    int buffer_idx;
    unsigned short status;
    int child[2];
} fd_rd_list_t;

typedef struct
{
    int buffer_idx;
    unsigned short status;
    unsigned int write_count;
} fd_wr_list_t;


void usocket_init();
void fd_remapping_init();
#ifdef __cplusplus
}
#endif
#ifdef __cplusplus

#include <unordered_map>

extern pthread_key_t pthread_sock_key;
const int BUFFERNUM = 100;



class thread_sock_data_t
{
public:
    std::unordered_map<key_t, int> *bufferhash;
    int lowest_available;
    int total_num;

    class buffer_t
    {
    public:
        interprocess_n_t * data;
        bool isvalid;
        int loc;
        key_t shmemkey;
        bool isRDMA;
        rdma_peer_t rdma_info;
        int rdma_buf_idx;
        buffer_t() : isvalid(false), isRDMA(false)
        {}
    } buffer[BUFFERNUM];

    thread_sock_data_t() :  bufferhash(nullptr), lowest_available(0), total_num(0)
    {}

    int isexist(key_t key);

    int newbuffer(key_t key, int loc);
    std::pair<int, rdma_self_pack_t *>  newbuffer_rdma(key_t key, int loc);

};
void monitor2proc_hook();

const int INIT_FD_REMAP_TABLE_SIZE = 128;
const int NUM_FD_TYPES = 4;

typedef enum {
    FD_TYPE_SYSTEM = 0,  // kernel fd, the default fd type
    FD_TYPE_SOCKET = 1,  // socket fd
    FD_TYPE_EPOLL  = 2,  // epoll fd
    FD_TYPE_UNKNOWN = 3  // default
} fd_type_t;

typedef struct {
    fd_type_t type;
    int real_fd;
} fd_remap_entry_t;

fd_type_t get_fd_type(int fd);
int get_real_fd(int virtual_fd);
int get_virtual_fd(fd_type_t type, int real_fd);
void set_fd_type(int fd, fd_type_t type);
int alloc_virtual_fd(fd_type_t type, int real_fd);
void delete_virtual_fd(int virtual_fd);
int check_sockfd_receive(int sockfd);
int check_sockfd_receive_ex(int sockfd, bool findAll);
int check_sockfd_send(int sockfd);

void epoll_remove(int fd);

#endif
#endif //IPC_DIRECT_SOCKET_LIB_H
