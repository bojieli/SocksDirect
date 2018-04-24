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

#ifdef __cplusplus
}
#endif