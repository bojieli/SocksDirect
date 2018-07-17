//
// Created by ctyi on 7/17/18.
//
#include "rdma.h"
#include <sys/socket.h>

static int rdma_sock_fd;


void create_rdma_socket()
{
    rdma_sock_fd = socket(AF_INET, );
}