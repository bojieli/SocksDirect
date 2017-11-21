//
// Created by ctyi on 11/14/17.
//

#ifndef IPC_DIRECT_METAQUEUE_H
#define IPC_DIRECT_METAQUEUE_H
#include <stdint.h>
#include <sys/types.h>
#define MAX_METAQUEUE_SIZE 256
#define METAQUEUE_MASK (MAX_METAQUEUE_SIZE)-1
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
    metaqueue_element data[MAX_METAQUEUE_SIZE];
} metaqueue_data;
typedef struct {
    uint32_t pointer;
} metaqueue_meta;
typedef struct {
    unsigned char is_other_side_blocking;
} shared_mem_meta_struc;
typedef struct {
    metaqueue_data* data;
    metaqueue_meta* meta;
} metaqueue_pack;


void metaqueue_push(metaqueue_pack q_pack, metaqueue_element *data);
void metaqueue_pop(metaqueue_pack q_pack, metaqueue_element *data);
int metaqueue_isempty(metaqueue_pack q_pack);
void metaqueue_init_meta(metaqueue_pack q_pack);
void metaqueue_init_data(metaqueue_pack q_pack);

#endif //IPC_DIRECT_METAQUEUE_H
