//
// Created by ctyi on 11/14/17.
//

#ifndef IPC_DIRECT_METAQUEUE_H
#define IPC_DIRECT_METAQUEUE_H
enum {
    REQ_NOP,
    REQ_FORK,
    REQ_EXIT,
    REQ_EXEC,
    REQ_CLONE,
    REQ_SETPID,
    REQ_LOCKINIT,
    REQ_LOCK,
    REQ_TRYLOCK,
    REQ_UNLOCK,
    REQ_ECHO,
    REQ_DEBUG,
    REQ_ERROR,
    REQ_THRTEST
}; 
typedef struct 
{
    pid_t pid;
    int command;
    int data;
} command_struc;
typedef struct {
    unsigned char is_valid;
    union {
        unsigned char raw[15];
        command_struc command;
    } data;
} metaqueue_element;
typedef struct {
    metaqueue_element data[256];
} metaqueue_data;

typedef struct {
    unsigned char is_other_side_blocking;
} shared_mem_meta_struc;
#endif //IPC_DIRECT_METAQUEUE_H
