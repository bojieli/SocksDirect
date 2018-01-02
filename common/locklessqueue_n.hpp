#ifndef IPC_DIRECT_METAQUEUE_N_HPP
#define IPC_DIRECT_METAQUEUE_N_HPP

#include <cstdint>
#include <cstring>
#include <cstdio>

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
public:
    uint32_t pointer;
    const uint32_t MASK;

    locklessqueue_t() : MASK(SIZE - 1), pointer(0), ringbuffer(nullptr)
    {}

    inline void init(void *baseaddr)
    {
        pointer = 0;
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
        ele.isvalid = false;
        ele.isdel = false;
        SW_BARRIER;
        ringbuffer[local_ptr] = ele;
        SW_BARRIER;
        ringbuffer[local_ptr].isvalid = true;
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
        if (!ringbuffer[local_ptr].isvalid)
            return false;
        SW_BARRIER;
        data = ringbuffer[local_ptr].data;
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

    inline void peek(int loc, T &output)
    {
        loc = loc & MASK;
        SW_BARRIER;
        output = ringbuffer[loc].data;
    }

    inline void del(int loc)
    {
        loc = loc & MASK;
        ringbuffer[loc].isdel=true;
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
