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
private:
    inline void atomic_copy16(element_t *dst, element_t *src)
    {
        asm volatile ( "movdqa (%0),%%xmm0\n"
                       "movaps %%xmm0,(%1)\n"
                       : /* no output registers */
                       : "r" (src), "r" (dst)
                       : "xmm0" );
    }

    inline bool atomic_set_bit(bool* ptr)
    {
        bool ret;
        asm (
        "mov $0, %%al\n"
        "mov $1, %%cl\n"
        "lock; cmpxchg %%cl, (%2)\n"
        "mov %%al, %0"
        :"=r"(ret), "+m"(*ptr)
        :"r"(ptr)
        :"al","cl","memory"
        );
        return !ret;
    }
public:
    union
    {
        uint32_t pointer;
        uint32_t head;
        uint32_t tail;
    };
    uint32_t MASK;

    locklessqueue_t() : MASK(SIZE - 1), pointer(0), ringbuffer(nullptr)
    {
        assert((sizeof(element_t)==16));
    }

    inline void init(void *baseaddr)
    {
        pointer = 0;
        MASK = SIZE - 1;
        ringbuffer = reinterpret_cast<element_t *>(baseaddr);
    }

    inline void init_mem()
    {
        memset(ringbuffer, 0, sizeof(element_t) * SIZE);
    }

    inline bool push_nb(const T &data)
    {
        uint32_t local_ptr = pointer & MASK;
        SW_BARRIER;
        //is full?
        if (ringbuffer[local_ptr].isvalid)
            return false;
        //fillup element
        element_t ele;
        ele.data = data;
        ele.isvalid = true;
        ele.isdel = false;
        SW_BARRIER;
        atomic_copy16(&ringbuffer[local_ptr], &ele);
        SW_BARRIER;
        pointer++;
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
        ringbuffer[local_ptr].isvalid = false;
        SW_BARRIER;
        pointer++;
        SW_BARRIER;
        while (ringbuffer[pointer & MASK].isvalid && ringbuffer[pointer & MASK].isdel)
        {
            SW_BARRIER;
            ringbuffer[pointer & MASK].isvalid = false;
            pointer++;
        }
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

    inline void del(int loc)
    {
        loc = loc & MASK;
        bool issuccess = atomic_set_bit(&ringbuffer[loc].isdel);
        if (!issuccess) return;
        SW_BARRIER;
        if ((ringbuffer[pointer & MASK].isvalid) && (loc == (pointer & MASK)))
        {
            ringbuffer[loc].isvalid = false;
            ++pointer;
            SW_BARRIER;
            while (ringbuffer[pointer & MASK].isvalid
                   && ringbuffer[pointer & MASK].isdel)
            {
                ringbuffer[pointer & MASK].isvalid = false;
                ++pointer;
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
        return (sizeof(element_t) * SIZE);
    }

};

#endif
