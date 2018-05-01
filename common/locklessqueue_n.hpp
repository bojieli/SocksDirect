#ifndef IPC_DIRECT_METAQUEUE_N_HPP
#define IPC_DIRECT_METAQUEUE_N_HPP

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <tuple>
#include <cassert>

#define SW_BARRIER asm volatile("" ::: "memory")

template<class T, uint32_t SIZE>
class locklessqueue_t
{
public:

    struct __attribute__((packed, aligned(16))) element_t 
    {
        T data;
        bool isvalid;
        bool isdel;
    };
private:
    element_t *ringbuffer;
    bool *return_flag;
    uint32_t credits;
    uint32_t CREDITS_PER_RETURN;
    bool credit_disabled;
private:
    inline void atomic_copy16(element_t *dst, element_t *src)
    {
        asm volatile ( "movdqa (%0),%%xmm0\n"
                       "movaps %%xmm0,(%1)\n"
                       : /* no output registers */
                       : "r" (src), "r" (dst)
                       : "xmm0" );
    }
    
public:
    union
    {
        uint32_t pointer;
        uint32_t head;
        uint32_t tail;
    };
    uint32_t MASK;

    locklessqueue_t() : ringbuffer(nullptr), pointer(0), MASK(SIZE - 1)
    {
        assert((sizeof(element_t)==16));
    }

    inline void init(void *baseaddr, bool is_receiver)
    {
        pointer = 0;
        MASK = SIZE - 1;
        CREDITS_PER_RETURN = SIZE / 2 + 1;
        ringbuffer = reinterpret_cast<element_t *>(baseaddr);
        return_flag = reinterpret_cast<bool *>(baseaddr + (sizeof(element_t) * SIZE));
        *return_flag = false;
        if (is_receiver)
            credits = 0;
        else
            credits = SIZE;
        credit_disabled = false;
    }

    inline void init_mem()
    {
        memset(ringbuffer, 0, sizeof(element_t) * SIZE);
    }

    inline bool push_nb(const T &data)
    {
        //is full?
        if (!credit_disabled && credits == 0) {
            if (*return_flag) {
                credits += CREDITS_PER_RETURN;
                *return_flag = false;
            }
            else
                return false;
        }

        //fillup element
        element_t ele;
        ele.data = data;
        ele.isvalid = true;
        ele.isdel = false;
        SW_BARRIER;

        uint32_t local_ptr = pointer & MASK;
        atomic_copy16(&ringbuffer[local_ptr], &ele);
        pointer++;
        credits--;
        return true;
    }

    inline void push(const T &data)
    {
        while (!push_nb(data));
    }
    
    inline void setpointer(uint32_t _pointer)
    {
        pointer = _pointer;    
    }

    inline void disable_credit()
    {
        credit_disabled = true;
    }

    //true: success false: fail
    inline bool pop_nb(T &data)
    {
        int local_ptr = pointer & MASK;
        SW_BARRIER;
        element_t ele;
        atomic_copy16(&ele, &ringbuffer[local_ptr]);
        SW_BARRIER;
        if (!ele.isvalid)
            return false;
        SW_BARRIER;
        data = ele.data;
        SW_BARRIER;

        /*
        ringbuffer[local_ptr].isvalid = false;
        SW_BARRIER;
        pointer++;
        SW_BARRIER;
        while (ringbuffer[pointer & MASK].isvalid && ringbuffer[pointer & MASK].isdel)
        {
            SW_BARRIER;
            ringbuffer[pointer & MASK].isvalid = false;
            pointer++;
        }*/
        del(local_ptr);
        return true;
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

#endif
