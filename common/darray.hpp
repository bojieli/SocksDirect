//
// Created by ctyi on 12/8/17.
//

#ifndef IPC_DIRECT_DARRAY_HPP
#define IPC_DIRECT_DARRAY_HPP

#include <cstdint>
#include <vector>
#include <stdio.h>

template<class T, uint32_t INITSIZE>
class darray_t
{
private:
    struct data_t
    {
        T data;
        bool isvalid;
        data_t():data(),isvalid(false){}
    };
    unsigned int lowest_available;
    unsigned int highest_possible;
    unsigned int total_num;
    unsigned int size;

    std::vector<data_t> data;
public:
    darray_t(): lowest_available(0), highest_possible(0), total_num(0),size(INITSIZE)
    {
        data.resize(INITSIZE);
    }
    inline unsigned int add(const T&input)
    {
        unsigned int idx=lowest_available;
        data[lowest_available].data = input;
        data[lowest_available].isvalid = true;
        ++total_num;
        if (size <=  total_num)
        {
            size *= 2;
            data.resize(size);
        }
        while (true)
        {
            ++lowest_available;
            if (lowest_available == size) lowest_available = 0;
            if (!data[lowest_available].isvalid)
                break;
        }
        if (idx > highest_possible) highest_possible = idx;
        return idx;
    }
    inline void del(unsigned int loc)
    {
        data[loc].isvalid = false;
        if (loc == highest_possible)
        {
            while ((highest_possible > 0) && (!data[highest_possible].isvalid))
                --highest_possible;
        }
        --total_num;
    }
    inline void init()
    {
        lowest_available = 0;
        highest_possible = 0;
        total_num = 0;
        size = INITSIZE;
        data.resize(INITSIZE);
    }
    inline T & operator[](unsigned int i)
    {
        return data[i].data;
    }
    inline unsigned int get_totalsize()
    {
        return total_num;
    }
    inline unsigned int get_highest_possible()
    {
        return highest_possible;
    }
    inline bool isvalid(unsigned int loc)
    {
        if (loc > highest_possible) return false;
        return data[loc].isvalid;
    }

    inline int iterator_init()
    {
        int curr_pointer=0;
        while (curr_pointer <= highest_possible)
        {
            if (isvalid(curr_pointer)) return curr_pointer;
            ++curr_pointer;
        }
        return -1;
    }
    inline int iterator_next(int prev_pointer)
    {
        int next_pointer=prev_pointer+1;
        while (next_pointer <= highest_possible)
        {
            if (isvalid(next_pointer)) return next_pointer;
            ++next_pointer;
        }
        return -1;
    }
};
#endif //IPC_DIRECT_DARRAY_HPP
