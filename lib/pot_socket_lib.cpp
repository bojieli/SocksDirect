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
#include "../common/helper.h"
#include "../common/metaqueue.h"
#include <sys/ioctl.h>
#include <pthread.h>
#include "fork.h"
#include "pot_socket_lib.h"
#include "../lib/zerocopy.h"
#include "../rdma/hrd.h"
#include <unordered_map>

const int MIN_PAGES_FOR_ZEROCOPY = 2;
const int MAX_TST_MSG_SIZE=1024*1024*4;
static uint8_t pot_mock_data[MAX_TST_MSG_SIZE * 2] __attribute__((aligned(PAGE_SIZE)));
static uint8_t mapping_buf[MAX_TST_MSG_SIZE * 2] __attribute__((aligned(PAGE_SIZE)));

static hrd_ctrl_blk_t* cb = nullptr;
static hrd_qp_attr_t *clt_qp = nullptr;
static hrd_qp_attr_t *srv_qp = nullptr;
static hrd_qp_attr_t *my_qp = nullptr;
static size_t srv_gid = 0;  // Global ID of this server thread
static size_t clt_gid = 0;     // One-to-one connections
static char srv_name[50] = {0};
static char clt_name[50] = {0};
static unsigned long original_phys[MAX_TST_MSG_SIZE * 4 / PAGE_SIZE];
static unsigned long recv_buffer_phys[MAX_TST_MSG_SIZE * 4 / PAGE_SIZE];
static unsigned long send_buffer_phys[MAX_TST_MSG_SIZE * 2 / PAGE_SIZE];

const int kAppUnsigBatch = 64;
static size_t nb_tx = 0;

void pot_init_write()
{
    for (int i=0;i<2*MAX_TST_MSG_SIZE;++i) pot_mock_data[i] = (rand() % 255 + 1);
    virt2physv(reinterpret_cast<uint64_t>(pot_mock_data), send_buffer_phys, 2 * MAX_TST_MSG_SIZE / PAGE_SIZE);
}

void pot_rdma_init(void)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    int thread_id = 0;
    pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    for (int i=0; i<CPU_SETSIZE; i++)
        if (CPU_ISSET(i, &cpuset)) {
            thread_id = i;
            break;
        }

    srv_gid = thread_id;
    clt_gid = thread_id;
    sprintf(srv_name, "socksdirect-server-%zu", srv_gid);
    sprintf(clt_name, "socksdirect-client-%zu", clt_gid);
}

int pot_connect(int socket, const struct sockaddr *address, socklen_t address_len)
{
    static bool initialized = false;
    if (initialized)
        return 0;
    initialized = true;

    pot_rdma_init();

    const size_t ib_port_index = 0;
    hrd_conn_config_t conn_config;
    conn_config.num_qps = 1;
    conn_config.use_uc = 0;
    conn_config.prealloc_buf = nullptr;
    conn_config.buf_size = MAX_TST_MSG_SIZE * 5;
    conn_config.buf_shm_key = -1;

    cb = hrd_ctrl_blk_init(clt_gid, ib_port_index, kHrdInvalidNUMANode,
            &conn_config, nullptr);

    memset(const_cast<uint8_t*>(cb->conn_buf), 0, 2 * MAX_TST_MSG_SIZE);
    memset(const_cast<uint8_t*>(cb->conn_buf + 2 * MAX_TST_MSG_SIZE), 1, 2 * MAX_TST_MSG_SIZE);
    memset(const_cast<uint8_t*>(cb->conn_buf + 4 * MAX_TST_MSG_SIZE), 0, MAX_TST_MSG_SIZE);
    virt2physv(reinterpret_cast<uint64_t>(cb->conn_buf), recv_buffer_phys, MAX_TST_MSG_SIZE * 4 / PAGE_SIZE);

    hrd_publish_conn_qp(cb, 0, clt_name);

    printf("RDMA: Client %s published. Waiting for server %s.\n", clt_name, srv_name);

    do {
        srv_qp = hrd_get_published_qp(srv_name);
        if (srv_qp == nullptr) usleep(200000);
    } while (srv_qp == nullptr);

    hrd_connect_qp(cb, 0, srv_qp);
    hrd_publish_ready(clt_name);
    my_qp = srv_qp;

    printf("RDMA: Client %s connected\n", clt_name);
    return 0;
}

ssize_t pot_accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags)
{
    static bool initialized = false;
    static int fd_num = 2147483647;
    if (initialized)
        return fd_num--;
    initialized = true;

    pot_rdma_init();

    size_t ib_port_index = 0;
    hrd_conn_config_t conn_config;
    conn_config.num_qps = 1;
    conn_config.use_uc = 0;
    conn_config.prealloc_buf = nullptr;
    conn_config.buf_size = MAX_TST_MSG_SIZE * 5;
    conn_config.buf_shm_key = -1;

    cb = hrd_ctrl_blk_init(srv_gid, ib_port_index, kHrdInvalidNUMANode,
            &conn_config, nullptr);

    memset(const_cast<uint8_t*>(cb->conn_buf), 0, 2 * MAX_TST_MSG_SIZE);
    memset(const_cast<uint8_t*>(cb->conn_buf + 2 * MAX_TST_MSG_SIZE), 1, 2 * MAX_TST_MSG_SIZE);
    memset(const_cast<uint8_t*>(cb->conn_buf + 4 * MAX_TST_MSG_SIZE), 0, MAX_TST_MSG_SIZE);
    virt2physv(reinterpret_cast<uint64_t>(cb->conn_buf), recv_buffer_phys, MAX_TST_MSG_SIZE * 4 / PAGE_SIZE);

    hrd_publish_conn_qp(cb, 0, srv_name);

    printf("RDMA: Server %s published. Waiting for client %s.\n", srv_name, clt_name);

    do {
        clt_qp = hrd_get_published_qp(clt_name);
        if (clt_qp == nullptr) usleep(200000);
    } while (clt_qp == nullptr);

    hrd_connect_qp(cb, 0, clt_qp);
    hrd_wait_till_ready(clt_name);
    my_qp = clt_qp;

    printf("RDMA: Server %s connected\n", srv_name);
    return fd_num--;
}

ssize_t pot_rdma_write_nbyte(int sockfd, size_t len)
{
    static unsigned long counter = 0;
    static unsigned long credits = MAX_TST_MSG_SIZE / PAGE_SIZE;
    sockfd = 2147483647 - sockfd;

    if (credits == 0) {
        if (!cb->conn_buf[4 * MAX_TST_MSG_SIZE + 64]) {
            printf("Warning: credit not returned yet!\n");
        }
        while (!cb->conn_buf[4 * MAX_TST_MSG_SIZE + 64]);
        printf("Got %d credits\n", MAX_TST_MSG_SIZE / 2 / PAGE_SIZE);
        credits = MAX_TST_MSG_SIZE / 2 / PAGE_SIZE;
        cb->conn_buf[4 * MAX_TST_MSG_SIZE + 64] = 0;
    }
    credits--;

    struct ibv_send_wr wr, *bad_send_wr;
    struct ibv_sge sgl;
    struct ibv_wc wc;

    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.num_sge = 1;
    wr.next = nullptr;
    wr.sg_list = &sgl;

    wr.send_flags = nb_tx % kAppUnsigBatch == 0 ? IBV_SEND_SIGNALED : 0;
    if (nb_tx % kAppUnsigBatch == 0 && nb_tx != 0) {
        hrd_poll_cq(cb->conn_cq[0], 1, &wc);
    }
    nb_tx++;

    wr.send_flags |= (len <= kHrdMaxInline) ? IBV_SEND_INLINE : 0;

    size_t offset = (counter * PAGE_SIZE) % MAX_TST_MSG_SIZE;
    counter++;

    sgl.addr = reinterpret_cast<uint64_t>(&cb->conn_buf[offset]) + 2 * MAX_TST_MSG_SIZE;
    sgl.length = len;
    sgl.lkey = cb->conn_buf_mr->lkey;

    if ((len >= PAGE_SIZE * MIN_PAGES_FOR_ZEROCOPY) &&
        ((sgl.addr & (PAGE_SIZE - 1)) == 0) &&
        ((reinterpret_cast<uint64_t>(&pot_mock_data[offset]) & (PAGE_SIZE - 1)) == 0) &&
        ((len & (PAGE_SIZE - 1)) == 0))
    {
        map_physv(sgl.addr, &send_buffer_phys[offset / PAGE_SIZE], original_phys, len / PAGE_SIZE);
        log_mapping(reinterpret_cast<void *>(sgl.addr), original_phys, len / PAGE_SIZE);
    }
    else {
        memcpy(reinterpret_cast<void*>(sgl.addr), &pot_mock_data[offset], len);
    }

    wr.wr.rdma.remote_addr = my_qp->buf_addr + offset;
    wr.wr.rdma.rkey = my_qp->rkey;

    int ret = ibv_post_send(cb->conn_qp[0], &wr, &bad_send_wr);
    if (ret != 0) {
       printf("wrong ret %d\n", ret);
       return 0;
    }
    return len;
}

ssize_t pot_rdma_read_nbyte(int sockfd, size_t len)
{
    static unsigned long counter = 0;
    static unsigned long credits = 0;
    sockfd = 2147483647 - sockfd;

    size_t offset = (counter * PAGE_SIZE) % MAX_TST_MSG_SIZE;
    counter++;

    volatile uint8_t *last_addr = cb->conn_buf + offset + len - 1;
    // wait
    while (*last_addr == 0) { }
    // release
    *last_addr = 0;

    uint64_t base_addr = reinterpret_cast<uint64_t>(cb->conn_buf + offset);

    if ((len >= PAGE_SIZE * MIN_PAGES_FOR_ZEROCOPY) &&
        ((base_addr & (PAGE_SIZE - 1)) == 0) &&
        ((reinterpret_cast<uint64_t>(&pot_mock_data[offset]) & (PAGE_SIZE - 1)) == 0) &&
        ((len & (PAGE_SIZE - 1)) == 0))
    {
        map_physv(reinterpret_cast<uint64_t>(&pot_mock_data[offset]), &recv_buffer_phys[offset / PAGE_SIZE], original_phys, len / PAGE_SIZE); 
        log_mapping(&pot_mock_data[offset], original_phys, len / PAGE_SIZE);
    }
    else {
        memcpy(&pot_mock_data[offset], reinterpret_cast<const void *>(base_addr), len);
    }

    credits++;
    if (credits >= MAX_TST_MSG_SIZE / 2 / PAGE_SIZE) {
        credits = 0;

        // send back to sender
        static size_t nb_tx = 0;
    
        struct ibv_send_wr wr, *bad_send_wr;
        struct ibv_sge sgl;
        struct ibv_wc wc;
    
        wr.opcode = IBV_WR_RDMA_WRITE;
        wr.num_sge = 1;
        wr.next = nullptr;
        wr.sg_list = &sgl;
    
        wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;

        cb->conn_buf[4 * MAX_TST_MSG_SIZE] = 1;
        sgl.addr = reinterpret_cast<uint64_t>(&cb->conn_buf[4 * MAX_TST_MSG_SIZE]);
        sgl.length = 1;
        sgl.lkey = cb->conn_buf_mr->lkey;
    
        wr.wr.rdma.remote_addr = my_qp->buf_addr + 4 * MAX_TST_MSG_SIZE + 64;
        wr.wr.rdma.rkey = my_qp->rkey;
    
        int ret = ibv_post_send(cb->conn_qp[0], &wr, &bad_send_wr);
        if (ret != 0) {
           printf("wrong ret %d\n", ret);
           return 0;
        }
        hrd_poll_cq(cb->conn_cq[0], 1, &wc);
    }

    return len;
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

    if ((numofbytes >= PAGE_SIZE * MIN_PAGES_FOR_ZEROCOPY) &&
            (((unsigned long)send_buffer & (PAGE_SIZE - 1)) == 0) &&
            ((numofbytes & (PAGE_SIZE - 1)) == 0))
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
                    short blk = buffer->b[1].popdata(ele.zc_retv.pointer, size, (uint8_t *) received_pages);
                    if (blk == -1)
                    {
                        buffer->q[1].del(blk);
                    } else
                    {
                        ele.zc_retv.pointer = blk;
                        buffer->q[1].set(blk, ele);
                    }

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
        static bool initialized = false;
        if (!initialized) {
            init_mapping();
            atexit(resume_mapping);
            initialized = true;
        }

        unsigned long *return_pages = (unsigned long *)malloc(sizeof(unsigned long) * num_pages);
        if ((unsigned long)(buf) & (PAGE_SIZE - 1) != 0) {
            printf("Warning: receive buffer %p not aligned, fall back to memcpy\n", buf);
            map_physv((unsigned long)mapping_buf, received_pages, return_pages, num_pages);
            log_mapping(mapping_buf, return_pages, num_pages);
            memcpy(buf, mapping_buf, num_pages * PAGE_SIZE);
        }
        else {
            map_physv((unsigned long)buf, received_pages, return_pages, num_pages);
            log_mapping(buf, return_pages, num_pages);

            /*
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
            */
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
                buffer_has_blk->q[1].del(loc_has_blk);
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

