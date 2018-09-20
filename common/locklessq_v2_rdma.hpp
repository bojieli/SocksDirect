//
// Created by ctyi on 5/25/18.
//



#ifndef LOCKLESSQ_V2_RDMA_HPP
#define LOCKLESSQ_V2_RDMA_HPP
#include <cstdint>
#include <assert.h>
#include <cstring>
#include "interprocess_t_n.hpp"
#include "rdma.h"
#define LOCKLESSQ_SIZE 512
#define LOCKLESSQ_BITMAP_ISVALID 0x1
#define LOCKLESSQ_BITMAP_ISDEL 0x2

#define SW_BARRIER asm volatile("" ::: "memory")

class locklessq_v2_rdma: public RDMA_flow_ctl_t::unpushed_data_t
{
public:

    class __attribute__((packed)) inner_element_t
    {
    public:
        struct __attribute__((packed)) fd_rw_t
        {
            int offset;
        };
        struct __attribute__((packed)) fd_rw_zc_t
        {
            unsigned short num_pages;
            unsigned short page_high;
            unsigned int page_low;
        };
        struct __attribute__((packed)) fd_rw_zcv_t
        {
            int pointer;
            unsigned short num_pages;
        };
        struct __attribute__((packed)) zc_ret_t
        {
            unsigned short page_lo, page_mid, page_hi;
            unsigned short num_pages;
        };
        struct __attribute__((packed)) zc_retv_t
        {
            int pointer;
            unsigned short num_pages;
        };
        struct __attribute__((packed)) pot_rw_t
        {
            uint8_t raw[8];
        };
        struct __attribute__((packed)) close_t
        {
            int req_fd;
            int peer_fd;
        };

        union __attribute__((packed))
        {
            unsigned char raw[8];
            fd_rw_t data_fd_rw;
            fd_rw_zc_t data_fd_rw_zc;
            fd_rw_zcv_t data_fd_rw_zcv;
            zc_ret_t zc_ret;
            zc_retv_t zc_retv;
            close_t close_fd;
            pot_rw_t pot_fd_rw;
        };
    };
    struct __attribute__((packed, aligned(16))) element_t {
        uint16_t size;
        uint8_t flags;
        uint8_t command;
        int fd;
        inner_element_t inner_element;
        uint8_t payload[0];
    };
private:
    RDMA_flow_ctl_t * RDMA_flow_ctl;
    uint32_t credits;
    element_t* bytes_arr;
    bool *return_flag;
    uint32_t CREDITS_PER_RETURN;
    bool credit_disabled;
    uint64_t remote_baseaddr;
    uint32_t RDMA_pointer;
    bool is_recv;
    inline void atomic_copy16(element_t *dst, const element_t *src)
    {
        asm volatile ( "movdqa (%0),%%xmm0\n"
                       "movaps %%xmm0,(%1)\n"
        : /* no output registers */
        : "r" (src), "r" (dst)
        : "xmm0" );
    }

    inline void atomic_copy8(element_t *dst, const element_t *src)
    {
        asm volatile (
                "mov (%0), %rax\n"
                "mov %rax, (%1)\n"
                :
                : "r"(src), "r"(dst)
                : "rax");
    }

public:
    union
    {
        uint32_t pointer;
        uint32_t head;
        uint32_t tail;
    };
    uint32_t MASK;

    locklessq_v2_rdma() : bytes_arr(nullptr), pointer(0), RDMA_pointer(0), MASK(LOCKLESSQ_SIZE - 1), RDMA_flow_ctl(nullptr),remote_baseaddr(0)
    {
        assert((sizeof(element_t)==16));
        is_recv = false;
    }

    inline void init(void *baseaddr, uint64_t _remote_addr, bool is_receiver, RDMA_flow_ctl_t * _RDMA_ctl)
    {
        pointer = RDMA_pointer = 0;
        remote_baseaddr = _remote_addr;
        is_recv = is_receiver;
        RDMA_flow_ctl = _RDMA_ctl;
        RDMA_flow_ctl->reg(this);
        MASK = LOCKLESSQ_SIZE - 1;
        CREDITS_PER_RETURN = LOCKLESSQ_SIZE / 2 + 1;
        bytes_arr = reinterpret_cast<element_t *>(baseaddr);
        return_flag = reinterpret_cast<bool *>(baseaddr + 2 * (sizeof(element_t) * LOCKLESSQ_SIZE));
        *return_flag = false;
        if (is_receiver)
            credits = 0;
        else
            credits = LOCKLESSQ_SIZE;
        credit_disabled = false;
    }

    inline void init_mem()
    {
        memset(bytes_arr, 0, sizeof(element_t) * LOCKLESSQ_SIZE);
    }

    inline bool push_nb(const element_t &data, void* payload_ptr)
    {
        int slots_occupied = (data.size+15)/16+1;
        //is full?
        if (!credit_disabled && credits < slots_occupied) {
            if (*return_flag) {
                credits += CREDITS_PER_RETURN;
                *return_flag = false;
            }
            else
                return false;
        }

        //copy the data
        memcpy((void *)&bytes_arr[(pointer + 1) & MASK], payload_ptr, data.size);
        //copy the metadata
        atomic_copy16(&bytes_arr[pointer & MASK],&data);
        pointer+=slots_occupied;
        credits-=slots_occupied;
        RDMA_flow_ctl->sync();
        return true;
    }

    inline void setpointer(uint32_t _pointer)
    {
        pointer = _pointer;
    }

    inline void disable_credit()
    {
        credit_disabled = true;
    }


    inline element_t peek_meta(unsigned int loc)
    {
        loc = loc & MASK;
        return bytes_arr[loc];
    }

    inline void set_meta(unsigned int loc, const element_t &data)
    {
        SW_BARRIER;
        bytes_arr[loc & MASK]=data;
        SW_BARRIER;
    }

    inline void* peek_data(unsigned int loc)
    {
        loc = loc & MASK;
        return (void*)&bytes_arr[loc].payload[0];
    }

    inline uint32_t del(unsigned int loc)
    {
        loc = loc & MASK;
        bytes_arr[loc].flags|=LOCKLESSQ_BITMAP_ISDEL;
        SW_BARRIER;
        if (loc == (pointer & MASK))
        {
            while ((bytes_arr[pointer & MASK].flags & LOCKLESSQ_BITMAP_ISVALID)
                   && (bytes_arr[pointer & MASK].flags & LOCKLESSQ_BITMAP_ISDEL))
            {
                bytes_arr[pointer & MASK].flags &= ~LOCKLESSQ_BITMAP_ISVALID;
                int slots_occupied=(bytes_arr[pointer & MASK].size + 15) / 16 + 1;
                pointer += slots_occupied;
                credits+=slots_occupied;
                if (credits >= CREDITS_PER_RETURN) {
                    *return_flag = true;
                    RDMA_flow_ctl->sync();
                    credits -= CREDITS_PER_RETURN;
                }
                SW_BARRIER;
            }
        }
        return pointer;
    }

    inline bool isempty()
    {
        uint32_t tmp_tail = pointer;
        bool isempty(true);
        SW_BARRIER;
        element_t curr_blk;
        curr_blk = bytes_arr[tmp_tail & MASK];
        SW_BARRIER;
        while (curr_blk.flags & LOCKLESSQ_BITMAP_ISVALID)
        {
            if (curr_blk.flags & LOCKLESSQ_BITMAP_ISDEL)
            {
                isempty = false;
                break;
            }
            int slots_occupied=(bytes_arr[tmp_tail & MASK].size + 15) / 16 + 1;
            tmp_tail+=slots_occupied;
            SW_BARRIER;
            curr_blk = bytes_arr[tmp_tail & MASK];
            SW_BARRIER;
        }
        return isempty;
    }

    inline static int getmemsize()
    {
        // have a cache line for return flag
        const int return_flag_size = 64;
        return (sizeof(element_t) * LOCKLESSQ_SIZE) + return_flag_size;
    }

    inline static int getalignedmemsize()
    {
        return (sizeof(element_t) * LOCKLESSQ_SIZE);
    }

    inline uint32_t nextptr(uint32_t ptr)
    {
        return (uint32_t)(ptr + (bytes_arr[ptr & MASK].size + 15) / 16 + 1) % MASK;
    }

    RDMA_flow_ctl_t::unpushed_data_t::mem_buffer_t get() final
    {
        RDMA_flow_ctl_t::unpushed_data_t::mem_buffer_t ret;
        if (is_recv && *return_flag)
        {
            ret.isexist = true;
            ret.size = 1;
            ret.localptr = return_flag;
            ret.remoteptr = remote_baseaddr + ((uint8_t *)return_flag - (uint8_t *)bytes_arr);
            *return_flag = false;
            return ret;
        }
        if (RDMA_pointer != pointer)
        {
            //It is a ring
            if (RDMA_pointer < pointer)
            {
                ret.isexist = true;
                ret.size = sizeof(element_t) * (pointer - RDMA_pointer);
                ret.localptr = (void *)&bytes_arr[RDMA_pointer];
                ret.remoteptr = remote_baseaddr + ((uint8_t *)&bytes_arr[RDMA_pointer] - (uint8_t *)bytes_arr);
                RDMA_pointer = pointer;
                return ret;
            } else
            {
                ret.isexist = true;
                ret.size = sizeof(element_t) * (LOCKLESSQ_SIZE - RDMA_pointer);
                ret.localptr = (void *)&bytes_arr[RDMA_pointer];
                ret.remoteptr = remote_baseaddr + ((uint8_t *)&bytes_arr[RDMA_pointer] - (uint8_t *)bytes_arr);
                RDMA_pointer = 0;
            }
        }
        ret.isexist = false;
        return ret;
    }
};

#endif