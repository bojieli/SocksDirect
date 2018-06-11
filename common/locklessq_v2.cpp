//
// Created by ctyi on 5/25/18.
//

#include <cstdint>
#include <assert.h>
#include <cstring>
#include "locklessq_v2.h"

#define LOCKLESSQ_SIZE 512
#define LOCKLESSQ_BITMAP_ISVALID 0x1
#define LOCKLESSQ_BITMAP_ISDEL 0x3

#define SW_BARRIER asm volatile("" ::: "memory")

class locklessq_v2 {
public:

    class __attribute__((packed)) inner_element_t
    {
    public:
        struct __attribute__((packed)) fd_rw_t
        {
            int pointer;
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
            unsigned long page;
            unsigned short num_pages;
        };
        struct __attribute__((packed)) zc_retv_t
        {
            int pointer;
            unsigned short num_pages;
        };
        struct __attribute__((packed)) pot_rw_t
        {
            uint8_t raw[9];
        };
        struct __attribute__((packed)) close_t
        {
            int req_fd;
            int peer_fd;
        };

        union __attribute__((packed))
        {
            unsigned char raw[13];
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
    uint32_t credits;
    element_t* bytes_arr;
    bool *return_flag;
    uint32_t CREDITS_PER_RETURN;
    bool credit_disabled;
    inline void atomic_copy16(element_t *dst, element_t *src)
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

    locklessq_v2() : bytes_arr(nullptr), pointer(0), MASK(LOCKLESSQ_SIZE - 1)
    {
        assert((sizeof(element_t)==16));
    }

    inline void init(void *baseaddr, bool is_receiver)
    {
        pointer = 0;
        MASK = LOCKLESSQ_SIZE - 1;
        CREDITS_PER_RETURN = LOCKLESSQ_SIZE / 2 + 1;
        bytes_arr = reinterpret_cast<element_t *>(baseaddr);
        return_flag = reinterpret_cast<bool *>(baseaddr + (sizeof(element_t) * LOCKLESSQ_SIZE));
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

    inline bool push_nb(const element_t &data)
    {
        int slots_occupied = (data.size-1)/16+1+1;
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
        for (int i=1;i<=slots_occupied-1;++i)
        {
            uint32_t local_ptr = (pointer+i) & MASK;
            bytes_arr[local_ptr] = *((element_t *)&data.payload[(i-1)*16]);
        }
        //copy the metadata
        atomic_copy16(&bytes_arr[pointer & MASK],&data);
        pointer+=slots_occupied;
        credits-=slots_occupied;
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


    inline std::tuple<bool, bool> peek(int loc, T &output)
    {
        loc = loc & MASK;
        element_t ele;
        atomic_copy16(&ele, &ringbuffer[loc]);
        SW_BARRIER;
        output = ele.data;
        SW_BARRIER;
        return std::make_tuple(ele.isvalid, ele.isdel);
    }

    inline void set(int loc, T &input)
    {
        SW_BARRIER;
        loc = loc & MASK;
        ringbuffer[loc].data = input;
        SW_BARRIER;
    }

    inline void del(unsigned int loc)
    {
        loc = loc & MASK;
        ringbuffer[loc].isdel=true;
        SW_BARRIER;
        if (loc == (pointer & MASK))
        {
            while (ringbuffer[pointer & MASK].isvalid
                   && ringbuffer[pointer & MASK].isdel)
            {
                ringbuffer[pointer & MASK].isvalid = false;
                ++pointer;
                ++credits;
                if (credits == CREDITS_PER_RETURN) {
                    *return_flag = true;
                    credits = 0;
                }
                SW_BARRIER;
            }
        }
    }

    inline bool isempty()
    {
        uint32_t tmp_tail = pointer;
        bool isempty(true);
        SW_BARRIER;
        element_t curr_blk;
        curr_blk = ringbuffer[tmp_tail & MASK];
        SW_BARRIER;
        while (curr_blk.isvalid)
        {
            if (!curr_blk.isdel)
            {
                isempty = false;
                break;
            }
            tmp_tail++;
            SW_BARRIER;
            curr_blk = ringbuffer[tmp_tail & MASK];
            SW_BARRIER;
        }
        return isempty;
    }

    inline static int getmemsize()
    {
        // have a cache line for return flag
        const int return_flag_size = 64;
        return (sizeof(element_t) * SIZE) + return_flag_size;
    }
};