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

const int MIN_PAGES_FOR_ZEROCOPY = 3;
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

    if (numofbytes >= PAGE_SIZE * MIN_PAGES_FOR_ZEROCOPY &&
            (((unsigned long)send_buffer & (PAGE_SIZE - 1)) == 0))
    {
        int num_pages = numofbytes / PAGE_SIZE;
        unsigned long *phys_addrs = (unsigned long *)malloc(sizeof(unsigned long) * num_pages);
        int ret = virt2physv((unsigned long)send_buffer, phys_addrs, num_pages);
        if (ret < 0) {
            printf("error calling virt2phys: return %d\n", ret);
            goto fallback;
        }

        // check if pages are continuous
        int i;
        for (i=1; i<num_pages; i++) {
            if (phys_addrs[i] != phys_addrs[i-1] + 1)
                break;
        }
        if (i == num_pages) { // if continuous
            ele.command = interprocess_t::cmd::DATA_TRANSFER_ZEROCOPY;
            ele.data_fd_rw_zc.fd = peer_fd;
            ele.data_fd_rw_zc.num_pages = num_pages;
            ele.data_fd_rw_zc.page_high = (unsigned short)(phys_addrs[0] >> 32);
            ele.data_fd_rw_zc.page_low = (unsigned int)(phys_addrs[0]);
            SW_BARRIER;
            buffer->q[0].push(ele);
        }
        else { // send in a vector
            short startloc = buffer->b[0].pushdata((uint8_t *)phys_addrs, sizeof(unsigned long) * num_pages);
            ele.command = interprocess_t::cmd::DATA_TRANSFER_ZEROCOPY_VECTOR;
            ele.data_fd_rw_zcv.fd = peer_fd;
            ele.data_fd_rw_zcv.num_pages = num_pages;
            ele.data_fd_rw_zcv.pointer = startloc;
            SW_BARRIER;
            buffer->q[0].push(ele);
        }

        send_buffer += num_pages * PAGE_SIZE;
        numofbytes -= num_pages * PAGE_SIZE;
    }

fallback:
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
            switch (ele.command) {
                case interprocess_t::cmd::CLOSE_FD:
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
                    break;
                }

                case interprocess_t::cmd::DATA_TRANSFER:
                case interprocess_t::cmd::DATA_TRANSFER_ZEROCOPY:
                case interprocess_t::cmd::DATA_TRANSFER_ZEROCOPY_VECTOR:
                {
                    if (ele.data_fd_rw.fd == target_fd)
                    {
                        loc_in_buffer_has_blk = pointer;
                        if (islockrequired)
                            pthread_mutex_unlock(buffer->rd_mutex);
                        return ITERATE_FD_IN_BUFFER_STATE::FIND;
                    }
                    break;
                }

                case interprocess_t::cmd::ZEROCOPY_RETURN:
                {
                    short num_pages = ele.zc_ret.num_pages;
                    short begin_page = ele.zc_ret.page;
                    for (int i=0; i<num_pages; i++) {
                        enqueue_free_page(begin_page + i);
                    }
                    break;
                }

                case interprocess_t::cmd::ZEROCOPY_RETURN_VECTOR:
                {
                    int num_pages = ele.zc_retv.num_pages;
                    unsigned long *received_pages = (unsigned long *)malloc(sizeof(unsigned long) * num_pages);
                    int size = sizeof(unsigned long) * num_pages;
                    buffer->b[1].popdata(ele.zc_retv.pointer, size, (uint8_t *) received_pages);
                    for (int i=0; i<num_pages; i++) {
                        enqueue_free_page(received_pages[i]);
                    }
                    free(received_pages);
                    break;
                }

                case interprocess_t::cmd::NOP:
                {
                        if (ele.pot_fd_rw.fd == target_fd)
                    {
                        loc_in_buffer_has_blk = pointer;
                        if (islockrequired)
                            pthread_mutex_unlock(buffer->rd_mutex);
                        return ITERATE_FD_IN_BUFFER_STATE::FIND;
                    }
                    break;
                }
            } // end switch
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


static inline void map_and_return_pages(void *buf, unsigned long *received_pages,
    interprocess_t *ret_queue, int num_pages)
{
        unsigned long *return_pages = (unsigned long *)malloc(sizeof(unsigned long) * num_pages);
        map_physv((unsigned long)buf, received_pages, return_pages, num_pages);

        // return pages now
        // check if pages are continuous
        int i;
        for (i=1; i<num_pages; i++) {
            if (return_pages[i] != return_pages[i-1] + 1)
                break;
        }
        interprocess_t::queue_t::element ele;
        if (i == num_pages) { // if continuous
            ele.command = interprocess_t::cmd::ZEROCOPY_RETURN;
            ele.zc_ret.num_pages = num_pages;
            ele.zc_ret.page = return_pages[0];
            SW_BARRIER;
            ret_queue->q[1].push(ele);
        }
        else { // send in a vector
            short startloc = ret_queue->b[1].pushdata((uint8_t *)return_pages, sizeof(unsigned long) * num_pages);
            ele.command = interprocess_t::cmd::ZEROCOPY_RETURN_VECTOR;
            ele.zc_retv.num_pages = num_pages;
            ele.zc_retv.pointer = startloc;
            SW_BARRIER;
            ret_queue->q[1].push(ele);
        }

        free(return_pages);
}

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
            int num_pages = ele.data_fd_rw_zc.num_pages;
            unsigned long *received_pages = (unsigned long *)malloc(sizeof(unsigned long) * num_pages);
            long phys_page = (ele.data_fd_rw_zc.page_high << 32) | (ele.data_fd_rw_zc.page_low);
            for (int i=0; i<num_pages; i++) {
                received_pages[i] = phys_page + i;
            }
            map_and_return_pages(buf, received_pages, buffer_has_blk, num_pages);
            free(received_pages);
        }
        if (ele.command == interprocess_t::cmd::DATA_TRANSFER_ZEROCOPY_VECTOR)
        {
            int num_pages = ele.data_fd_rw_zcv.num_pages;
            unsigned long *received_pages = (unsigned long *)malloc(sizeof(unsigned long) * num_pages);
            short blk = buffer_has_blk->b[1].popdata(ele.data_fd_rw_zcv.pointer, ret, (uint8_t *) received_pages);
            if (blk == -1)
            {
                FATAL("zero copy vector not found error\n");
                buffer_has_blk->q[1].del(loc_has_blk);
            } else
            {
                ele.data_fd_rw.pointer = blk;
                buffer_has_blk->q[1].set(loc_has_blk, ele);
            }

            map_and_return_pages(buf, received_pages, buffer_has_blk, num_pages);
            free(received_pages);
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

