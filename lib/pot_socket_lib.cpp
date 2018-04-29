//
// Created by ctyi on 4/23/18.
//
#include "../common/darray.hpp"
#include "lib.h"
#include "socket_lib.h"
#include "../common/helper.h"
#include "../common/interprocess_t.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstdarg>
#include "lib_internal.h"
#include "../common/metaqueue.h"
#include <sys/ioctl.h>
#include "fork.h"
#include "pot_socket_lib.h"
#include "../lib/zerocopy.h"

const int MAX_TST_MSG_SIZE=64*1024;
static uint8_t pot_mock_data[MAX_TST_MSG_SIZE] __attribute__((aligned(PAGE_SIZE)));

void pot_init_write()
{
    for (int i=0;i<MAX_TST_MSG_SIZE;++i) pot_mock_data[i] = rand() % 256;
}

ssize_t pot_write_nbyte(int fd, int numofbytes)
{
    if (fd < FD_DELIMITER) return -1;
    fd = MAX_FD_ID - fd;

    thread_data_t *thread_data = GET_THREAD_DATA();
    if (thread_data->fds_wr.begin(fd).end() || thread_data->fds_datawithrd[fd].type != USOCKET_TCP_CONNECT
        || !thread_data->fds_datawithrd[fd].property.tcp.isopened)
    {
        errno = EBADF;
        return -1;
    }

    monitor2proc_hook();

    auto thread_sock_data = GET_THREAD_SOCK_DATA();
    auto iter = thread_data->fds_wr.begin(fd);
    interprocess_t *buffer = &thread_sock_data->
            buffer[iter->buffer_idx].data;
    int peer_fd = thread_data->fds_datawithrd[fd].peer_fd;
    interprocess_t::queue_t::element ele;

    uint8_t *send_buffer = pot_mock_data;

    if (numofbytes <= 16)
    {
        ele.command = interprocess_t::cmd ::NOP;
        ele.pot_fd_rw.fd = peer_fd;
        memcpy(ele.pot_fd_rw.raw, send_buffer, 9);
        SW_BARRIER;
        buffer->q[0].push(ele);
        return 0;
    }

    while (numofbytes >= 4096)
    {
        long physaddr = virt2phys((unsigned long)send_buffer);
        if (physaddr < 0)
                FATAL("error calling virt2phys: return %ld\n", physaddr);
        ele.command = interprocess_t::cmd::DATA_TRANSFER_ZEROCOPY;
        ele.data_fd_rw_zc.fd = peer_fd;
        ele.data_fd_rw_zc.page_high = (unsigned short)(physaddr >> 32);
        ele.data_fd_rw_zc.page_low = (unsigned int)(physaddr);
        SW_BARRIER;
        buffer->q[0].push(ele);
        send_buffer += PAGE_SIZE;
    }

    if (numofbytes > 0)
    {
        short startloc = buffer->b[0].pushdata(send_buffer, numofbytes);
        ele.command = interprocess_t::cmd::DATA_TRANSFER;
        ele.data_fd_rw.fd = peer_fd;
        ele.data_fd_rw.pointer = startloc;
        SW_BARRIER;
        buffer->q[0].push(ele);
        return 0;

    }
    return 0;
}

enum ITERATE_FD_IN_BUFFER_STATE
{
    CLOSED,
    FIND,
    NOTFIND,
    ALLCLOSED
};

static inline ITERATE_FD_IN_BUFFER_STATE recvfrom_iter_fd_in_buf
        (int target_fd,
         adjlist<file_struc_rd_t, MAX_FD_OWN_NUM, fd_rd_list_t, MAX_FD_PEER_NUM>::iterator& iter,
         int &loc_in_buffer_has_blk, thread_data_t *thread_data, thread_sock_data_t *thread_sock_data)
{

    interprocess_t *buffer = &(thread_sock_data->buffer[iter->buffer_idx].data);
    uint8_t pointer = buffer->q[1].tail;
    bool islockrequired = (bool)(iter->status & FD_STATUS_RD_RECV_FORKED);
    if (islockrequired)
        pthread_mutex_lock(buffer->rd_mutex);
    SW_BARRIER;
    while (true)
    {  //for same fd(buffer), iterate each available slot
        interprocess_t::queue_t::element ele;
        bool ele_isvalid, ele_isdel;
        std::tie(ele_isvalid, ele_isdel) = buffer->q[1].peek(pointer, ele);
        SW_BARRIER;
        if (!ele_isvalid)
            break;
        if (!ele_isdel)
        {
            if (ele.command == interprocess_t::cmd::CLOSE_FD)
            {
                if (ele.close_fd.peer_fd == target_fd &&
                    ele.close_fd.req_fd == thread_data->fds_datawithrd[target_fd].peer_fd)
                {
                    buffer->q[1].del(pointer);
                    DEBUG("Received close req for %d from %d", ele.close_fd.peer_fd, ele.close_fd.req_fd);

                    iter = thread_data->fds_datawithrd.del_element(iter);
                    if (iter.end())
                    {
                        DEBUG("Destroyed self fd %d.", target_fd);
                        thread_data->fds_datawithrd.del_key(target_fd);
                        if (islockrequired)
                            pthread_mutex_unlock(buffer->rd_mutex);
                        return ITERATE_FD_IN_BUFFER_STATE::ALLCLOSED;
                    }
                    if (islockrequired)
                        pthread_mutex_unlock(buffer->rd_mutex);
                    return ITERATE_FD_IN_BUFFER_STATE::CLOSED; //no need to traverse this queue anyway
                } else {
                    if (!thread_data->fds_datawithrd.is_keyvalid(ele.close_fd.peer_fd))
                        buffer->q[1].del(pointer);
                }
            }

            if ((ele.command == interprocess_t::cmd::DATA_TRANSFER || 
                 ele.command == interprocess_t::cmd::DATA_TRANSFER_ZEROCOPY) &&
                ele.data_fd_rw.fd == target_fd)
            {
                loc_in_buffer_has_blk = pointer;
                if (islockrequired)
                    pthread_mutex_unlock(buffer->rd_mutex);
                return ITERATE_FD_IN_BUFFER_STATE::FIND;
            }
            if (ele.command == interprocess_t::cmd::NOP &&
                    ele.pot_fd_rw.fd == target_fd)
            {
                loc_in_buffer_has_blk = pointer;
                if (islockrequired)
                    pthread_mutex_unlock(buffer->rd_mutex);
                return ITERATE_FD_IN_BUFFER_STATE::FIND;
            }
        }
        if (buffer->q[1].tail > pointer)
            pointer = buffer->q[1].tail;
        else
            ++pointer;
    }
    iter = recv_empty_hook(iter, target_fd);
    if (islockrequired)
        pthread_mutex_unlock(buffer->rd_mutex);
    return ITERATE_FD_IN_BUFFER_STATE::NOTFIND;
}

#undef DEBUGON
#define DEBUGON 1

#undef DEBUGON
#define DEBUGON 0
ssize_t pot_read_nbyte(int sockfd, void *buf, size_t len)
{
    //test whether fd exists
    if (sockfd < FD_DELIMITER) FATAL("Invalid fd");
    sockfd = MAX_FD_ID - sockfd;

    thread_data_t *thread_data = GET_THREAD_DATA();
    if (thread_data->fds_datawithrd.begin(sockfd).end() || thread_data->fds_datawithrd[sockfd].type != USOCKET_TCP_CONNECT
        || !thread_data->fds_datawithrd[sockfd].property.tcp.isopened)
    {
        errno = EBADF;
        return -1;
    }

    //hook for all the process
    monitor2proc_hook();

    auto thread_sock_data = GET_THREAD_SOCK_DATA();
    interprocess_t *buffer_has_blk(nullptr);
    int loc_has_blk(-1);
    bool isFind(false);
    do //if blocking infinate loop
    {
        auto iter = thread_data->fds_datawithrd.begin(sockfd);
        while (true) //iterate different peer fd
        {
            int ret_loc(-1);
            bool isFin(false);
            interprocess_t *buffer = &(thread_sock_data->buffer[iter->buffer_idx].data);
            ITERATE_FD_IN_BUFFER_STATE ret_state = recvfrom_iter_fd_in_buf(sockfd, iter, ret_loc, thread_data, thread_sock_data);
            switch (ret_state)
            {
                case ITERATE_FD_IN_BUFFER_STATE::ALLCLOSED:
                    return 0;
                case ITERATE_FD_IN_BUFFER_STATE::CLOSED:
                    break;
                case ITERATE_FD_IN_BUFFER_STATE::FIND:
                    buffer_has_blk = buffer;
                    loc_has_blk = ret_loc;
                    isFind=true;
                    break;
                case ITERATE_FD_IN_BUFFER_STATE::NOTFIND:
                    if (iter.end())
                        isFin=true;
                    break;
            }
            if (isFin) break; //Finish iterate all peer fds
            if (isFind) break; //Get the requested block
        }
        if (isFind) break;
        monitor2proc_hook();
    } while (thread_data->fds_datawithrd[sockfd].property.is_blocking);
    int ret(len);
    if (!isFind)
    {
        errno = EAGAIN | EWOULDBLOCK;
        return -1;
    } else {
        //printf("found blk loc %d\n", loc_has_blk);
        interprocess_t::queue_t::element ele;
        buffer_has_blk->q[1].peek(loc_has_blk, ele);
        if (ele.command == interprocess_t::cmd::DATA_TRANSFER)
        {
            short blk = buffer_has_blk->b[1].popdata(ele.data_fd_rw.pointer, ret, (uint8_t *) buf);
            if (blk == -1)
            {
                SW_BARRIER;
                buffer_has_blk->q[1].del(loc_has_blk);
                SW_BARRIER;
            } else
            {
                ele.data_fd_rw.pointer = blk;
                buffer_has_blk->q[1].set(loc_has_blk, ele);
            }
        }
        if (ele.command == interprocess_t::cmd::DATA_TRANSFER_ZEROCOPY)
        {
            long physaddr = (ele.data_fd_rw_zc.page_high << 32) | (ele.data_fd_rw_zc.page_low);
            long ret = map_phys((unsigned long)buf, physaddr);
            if (ret < 0)
                FATAL("map_phys failed: %d\n", ret);
        }
        if (ele.command == interprocess_t::cmd::NOP)
        {
            memcpy(buf, ele.pot_fd_rw.raw, 9);
            ret = 9;
            SW_BARRIER;
            buffer_has_blk->q[1].del(loc_has_blk);
            SW_BARRIER;
        }
    }
    return ret;
}

