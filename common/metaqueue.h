//
// Created by ctyi on 11/14/17.
//

#ifndef IPC_DIRECT_METAQUEUE_H
#define IPC_DIRECT_METAQUEUE_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <sys/types.h>

#define MAX_METAQUEUE_SIZE 256
#define METAQUEUE_MASK ((MAX_METAQUEUE_SIZE)-1)
typedef struct {
    unsigned short command;
    pid_t pid;
    int data;
} command_t;
typedef struct {
    unsigned short command;
    unsigned short port;
    unsigned short is_reuseaddr;
} command_sock_listen_t;
typedef struct {
    unsigned short command;
    unsigned short port;
    int fd;
} command_sock_connect_t;
typedef struct {
    unsigned short command;
    key_t shm_key;
    int fd;
    unsigned short port;
    int8_t loc;
} res_sock_connect_t;
typedef struct {
    unsigned short command;
    int data;
} res_error_t;
typedef struct {
    union {
        unsigned char raw[15];
        command_t command;
        command_sock_listen_t sock_listen_command;
        command_sock_connect_t sock_connect_command;
        res_sock_connect_t sock_connect_res;
        res_error_t res_error;
    } data;
    unsigned char is_valid;
} metaqueue_element;
typedef struct {
    metaqueue_element data[MAX_METAQUEUE_SIZE];
} metaqueue_data;
typedef struct {
    uint32_t pointer;
} metaqueue_meta_t;
typedef struct {
    unsigned char is_other_side_blocking;
} shared_mem_meta_struc;
typedef struct {
    metaqueue_data *data;
    metaqueue_meta_t *meta;
} metaqueue_pack;


void metaqueue_push(metaqueue_pack q_pack, metaqueue_element *data);

void metaqueue_pop(metaqueue_pack q_pack, metaqueue_element *data);

int metaqueue_isempty(metaqueue_pack q_pack);

void metaqueue_init_meta(metaqueue_pack q_pack);

void metaqueue_init_data(metaqueue_pack q_pack);

#ifdef __cplusplus
}
#endif
#endif //IPC_DIRECT_METAQUEUE_H
