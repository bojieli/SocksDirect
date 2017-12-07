#ifndef IPC_DIRECT_METAQUEUE_N_HPP
#define IPC_DIRECT_METAQUEUE_N_HPP

#include <cstdint>
#include <cstring>

#define SW_BARRIER asm volatile("" ::: "memory")

template<class T, uint32_t SIZE>
class locklessqueue_t
{
public:

    struct element_t
    {
        T data;
        bool isvalid;
    };
private:
    element_t *ringbuffer;
    uint32_t pointer;
public:
    const uint32_t MASK;

    locklessqueue_t() : MASK(SIZE - 1), pointer(0), ringbuffer(nullptr)
    {}

    inline void init(void *baseaddr)
    {
        pointer = 0;
        ringbuffer = reinterpret_cast<element_t *>(baseaddr);
    }

    void init_mem()
    {
        memset(ringbuffer, 0, sizeof(element_t) * SIZE);
    }

    inline void push(const T &data)
    {
        element_t ele;
        ele.data = data;
        ele.isvalid = false;
        //is full?
        while (ringbuffer[pointer & MASK].isvalid)
                SW_BARRIER;
        SW_BARRIER;
        ringbuffer[pointer & MASK] = ele;
        SW_BARRIER;
        ringbuffer[pointer & MASK].isvalid = true;
        SW_BARRIER;
        pointer++;
    }

    //true: success false: fail
    inline bool pop_nb(T &data)
    {
        if (!ringbuffer[pointer & MASK].isvalid) return false;
        SW_BARRIER;
        data = ringbuffer[pointer & MASK].data;
        SW_BARRIER;
        ringbuffer[pointer & MASK].isvalid = false;
        SW_BARRIER;
        pointer++;
        return true;
    }

    inline void pop(T &data)
    {
        while (!ringbuffer[pointer & MASK].isvalid)
                SW_BARRIER;
        data = ringbuffer[pointer & MASK].data;
        SW_BARRIER;
        ringbuffer[pointer & MASK].isvalid = false;
        SW_BARRIER;
        pointer++;
    }

    inline bool isempty()
    {
        return !ringbuffer[pointer & MASK].isvalid;
    }

    inline static int getmemsize()
    {
        return (sizeof(element_t) * SIZE);
    }

};

#endif