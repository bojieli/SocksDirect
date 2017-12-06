//
// Created by ctyi on 11/23/17.
//

#ifndef IPC_DIRECT_SOCKET_LIB_H
#define IPC_DIRECT_SOCKET_LIB_H
#ifdef __cplusplus
extern "C"
{
#endif
#define MAX_FD_ID 0x7FFFFFFF
#define MAX_FD_OWN_NUM 100
#define MAX_FD_PEER_NUM 1000
#define FD_DELIMITER 0x3FFFFFFF
enum
{
    USOCKET_TCP_LISTEN,
    USOCKET_TCP_CONNECT
};
typedef struct
{
    int is_addrreuse;
    int is_blocking;
    union
    {
        unsigned short port;
    } tcp;
} socket_property_t;
typedef struct
{
    int isvaild;
    int type;
    int peer_fd_ptr;
    int next_op_fd;
    socket_property_t property;
} file_struc_t;
typedef struct
{
    int fd;
    int next;
    int buffer_idx;
    int is_valid;
} fd_list_t;
void usocket_init();
#ifdef __cplusplus
}
#endif
#ifdef __cplusplus

#include <unordered_map>
#include "../common/interprocess_t.h"

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
        interprocess_t data;
        bool isvalid;

        buffer_t() : isvalid(false)
        {}
    } buffer[BUFFERNUM];

    thread_sock_data_t() : lowest_available(0), total_num(0), bufferhash(nullptr)
    {}

    int isexist(key_t key);

    int newbuffer(key_t key, int loc);
};

#endif
#endif //IPC_DIRECT_SOCKET_LIB_H
