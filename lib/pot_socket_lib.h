//
// Created by ctyi on 4/23/18.
//

#ifndef IPC_DIRECT_POT_SOCKET_LIB_H
#define IPC_DIRECT_POT_SOCKET_LIB_H
#ifdef __cplusplus
extern "C"
{
#endif


#endif //IPC_DIRECT_POT_SOCKET_LIB_H
extern void pot_init_write();
extern ssize_t pot_write_nbyte(int fd, int numofbytes);
extern ssize_t pot_read_nbyte(int sockfd, void *buf, size_t len);
extern ssize_t pot_accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags);
extern int pot_connect(int socket, const struct sockaddr *address, socklen_t address_len);
extern ssize_t pot_rdma_write_nbyte(int sockfd, size_t len);
extern ssize_t pot_rdma_read_nbyte(int sockfd, size_t len);


#ifdef __cplusplus
}
#endif
