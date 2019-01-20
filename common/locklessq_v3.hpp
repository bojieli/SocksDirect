#ifndef IPC_DIRECT_METAQUEUE_N_HPP
#define IPC_DIRECT_METAQUEUE_N_HPP

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <tuple>
#include <cassert>

#define SW_BARRIER asm volatile("" ::: "memory")




template<class T, uint32_t SIZE>
class locklessqueue_t_v3 {
public:
    class locklessq_block_t;

    class element_t;

    class locklessq_block_t {
    public:
        element_t *ringbuffer;
        bool *return_flag;
        locklessq_block_t *next;
        bool is_avail;
    };

    struct __attribute__((packed, aligned(16))) element_t {
        T data;
        bool isvalid;
        bool isdel;
    };

    class mem_ptr_t {
    public:
        element_t *ringbuffer;
        bool *return_flag;
    };

private:
    //start ptr point to the current available block
    //prevptr point to the block previous to the startptr
    locklessq_block_t *startptr, *prevptr;
    bool *return_flag;
    bool is_receiver;
private:
    inline void atomic_copy16(element_t *dst, element_t *src) {
        asm volatile ( "movdqa (%0),%%xmm0\n"
                       "movaps %%xmm0,(%1)\n"
        : /* no output registers */
        : "r" (src), "r" (dst)
        : "xmm0" );
    }

public:
    union {
        uint32_t pointer;
        uint32_t head;
        uint32_t tail;
    };
    uint32_t MASK;

    locklessqueue_t_v3() : pointer(0), MASK(SIZE - 1), startptr(nullptr), prevptr(nullptr) {
        assert((sizeof(element_t) == 16));
    }

    static inline bool check_next_avail(locklessq_block_t *ptr) {
        SW_BARRIER;
        if (*(ptr->return_flag)) {
            ptr->is_avail = true;
            *(ptr->return_flag) = 0;
            return true;
        }
        return ptr->is_avail;
    }

    class iterator {
    private:
        uint32_t pointer;
        locklessq_block_t *currptr;
        locklessqueue_t_v3 *q;
    public:
        iterator() : pointer(SIZE), currptr(nullptr), q(nullptr) {}

        iterator &operator++() {
            //if it is the end of current block

            if (pointer == SIZE-1) {
                    currptr = currptr->next;
                    pointer = 0;
                    return *this;
            }

            pointer++;
            return *this;
        }

        inline element_t *operator->() const {
            return &(currptr->ringbuffer[pointer]);
        }

        inline void del() {
            if (pointer == SIZE) return;
            if (currptr == nullptr) return;
            currptr->ringbuffer[pointer].isdel = true;

            while (q->startptr->ringbuffer[q->pointer].isvalid &&
                    q->startptr->ringbuffer[q->pointer].isdel)
            {
                q->startptr->ringbuffer[q->pointer].isvalid = false;
                SW_BARRIER;
                if (q->startptr == currptr && pointer == q->pointer) ++pointer;
                ++q->pointer;
                if (q->pointer == SIZE)
                {
                    if (q->startptr == currptr && pointer == q->pointer)
                    {
                        pointer = 0;
                        currptr = currptr->next;
                    }
                    SW_BARRIER;
                    *(q->startptr->return_flag) = true;
                    SW_BARRIER;
                    q->prevptr = q->startptr;
                    q->startptr = q->startptr->next;

                    q->pointer = 0;
                }
            }

        }

        inline void init(locklessq_block_t * _currptr, uint32_t _pointer, locklessqueue_t_v3 * _q)
        {
            q=_q;
            pointer = _pointer;
            currptr = _currptr;
        }

        inline bool valid()
        {
            if (currptr == nullptr)
                return false;
            if (pointer >= SIZE)
                return false;
            return true;
        }
    };

    inline void init_ptr(mem_ptr_t blk1_ptr, mem_ptr_t blk2_ptr, bool is_receiver_tmp) {
        locklessq_block_t *blk1 = new locklessq_block_t;
        locklessq_block_t *blk2 = new locklessq_block_t;
        startptr = blk1;
        prevptr = blk2;
        blk1->ringbuffer = blk1_ptr.ringbuffer;
        blk1->return_flag = blk1_ptr.return_flag;
        blk1->is_avail = true;

        //build a ring
        blk1->next = blk2;
        blk2->next = blk1;

        blk2->ringbuffer = blk2_ptr.ringbuffer;
        blk2->return_flag = blk2_ptr.return_flag;
        blk2->is_avail = true;

        pointer = 0;
        is_receiver = is_receiver_tmp;
    }

    inline void init_mem() {
        locklessq_block_t *ptr = startptr;
        do {
            memset(ptr->ringbuffer, 0, sizeof(element_t) * SIZE);
            *(ptr->return_flag) = false;
            ptr = ptr->next;
        } while (ptr != startptr);
    }

    inline bool push_nb(const T &data) {

        //first check the slot
        if (pointer == SIZE) {
            SW_BARRIER;
            startptr->is_avail=false;
            //next block is not released
            if (check_next_avail(startptr->next)) {
                pointer = 0;
                prevptr = startptr;
                startptr = startptr->next;

            } else
                return false;
        }


            //fillup element
            element_t ele;
            ele.data = data;
            ele.isvalid = true;
            ele.isdel = false;
            SW_BARRIER;

            uint32_t local_ptr = pointer & MASK;
            atomic_copy16(&startptr->ringbuffer[local_ptr], &ele);
            pointer++;
            return true;


    }

    inline bool is_full()
    {
        return (pointer == SIZE);
    }

    inline iterator begin()
    {
        iterator iter;
        iter.init(startptr, pointer, this);
        return iter;
    }
};

#endif
