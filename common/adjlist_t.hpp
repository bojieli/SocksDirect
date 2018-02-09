//
// Created by ctyi on 1/5/18.
//

#ifndef IPC_DIRECT_ADJLIST_T_HPP
#define IPC_DIRECT_ADJLIST_T_HPP
#include "darray.hpp"
#include <cstdint>
template<class T1, uint32_t initsizet1, class T2, uint32_t initsizet2>
class adjlist;

template<class T1, uint32_t initsizet1, class T2, uint32_t initsizet2>
class adjlist_iterator_t
{
private:
    int curr_ptr;
    int prev_ptr;
    int start_ptr;
    using adjlist_type=adjlist<T1, initsizet1, T2, initsizet2>;
    adjlist_type *adj;

    int idx;
    bool isvalid;

    friend class adjlist<T1, initsizet1, T2, initsizet2>;
public:
    adjlist_iterator_t(adjlist_type *_adj, int _idx):
            adj(_adj),
            idx(_idx),
            isvalid(false),
            curr_ptr(-1),
            prev_ptr(-1),
            start_ptr(-1)
    {}
    adjlist_iterator_t<T1, initsizet1, T2, initsizet2> & next();
    T2 &operator* ();
    T2 *operator-> ();
    bool end();
};

template<class T1, uint32_t initsizet1, class T2, uint32_t initsizet2>
class adjlist
{
private:
    class index_t
    {
    public:
        T1 data;
        int pointer;
    };
    darray_t<index_t, initsizet1> index;
    class adj_element_t
    {
    public:
        T2 adjdata;
        int next;
    };
    darray_t<adj_element_t, initsizet2> _adjlist;
    friend class adjlist_iterator_t<T1, initsizet1, T2, initsizet2>;
public:
    int add_key(const T1 &input);
    void del_key(unsigned int key_idx);
    inline T1 & operator[](unsigned int i);
    typedef adjlist_iterator_t<T1, initsizet1, T2, initsizet2> iterator;
    iterator add_element(int key_idx, const T2 &input);
    iterator add_element_at(iterator iter, int key_idx, const T2 &input);
    iterator del_element(iterator iter);
    iterator begin(int key_idx);
    bool is_keyvalid(int key_idx);
    void init(uint32_t size, const T1& input);
    int hiter_begin();
    int hiter_next(int prev);
};


//the implementation
template<class T1, uint32_t initsizet1, class T2, uint32_t initsizet2>
inline int adjlist<T1, initsizet1, T2, initsizet2>::add_key(const T1 &input)
{
    index_t new_idx;
    new_idx.pointer = -1;
    new_idx.data = input;
    int n_idx_idx = index.add(new_idx);
    return n_idx_idx;
}

template<class T1, uint32_t initsizet1, class T2, uint32_t initsizet2>
inline void adjlist<T1, initsizet1, T2, initsizet2>::del_key(unsigned int key_idx)
{
    index.del(key_idx);
}

template<class T1, uint32_t initsizet1, class T2, uint32_t initsizet2>
inline T1 & adjlist<T1, initsizet1, T2, initsizet2>::operator[](unsigned int i)
{
    return index[i].data;
}

template<class T1, uint32_t initsizet1, class T2, uint32_t initsizet2>
inline adjlist_iterator_t<T1, initsizet1, T2, initsizet2>
adjlist<T1, initsizet1, T2, initsizet2>::add_element(int key_idx, const T2 &input)
{
    iterator ret(this, key_idx);
    if (!index.isvalid(key_idx))
    {
        ret.isvalid = false;
        return ret;
    }

    //prepare the ret value
    ret.isvalid = true;
    ret.idx = key_idx;

    int ptr = index[key_idx].pointer;

    //it is the first element for the designated key
    if (ptr == -1)
    {
        adj_element_t ele;
        ele.adjdata = input;
        ele.next = -1;
        int n_adjele_idx = _adjlist.add(ele);
        //Link the next to itself to build a circle
        _adjlist[n_adjele_idx].next = n_adjele_idx;
        index[key_idx].pointer = n_adjele_idx;
        //settle the ret
        ret.start_ptr = ret.curr_ptr = n_adjele_idx;
        ret.prev_ptr = n_adjele_idx;
    } else //it is not the first element for the designated key
    {
        int prev_ptr = ptr;
        adj_element_t ele;
        ele.adjdata = input;
        ele.next = _adjlist[prev_ptr].next;
        int n_adjele_idx = _adjlist.add(ele);
        _adjlist[prev_ptr].next = n_adjele_idx;
        //settle the ret
        ret.start_ptr = ret.curr_ptr = ret.prev_ptr = n_adjele_idx;
    }
    return ret;
}

template<class T1, uint32_t initsizet1, class T2, uint32_t initsizet2>
inline adjlist_iterator_t<T1, initsizet1, T2, initsizet2>
adjlist<T1, initsizet1, T2, initsizet2>::add_element_at(iterator iter, int key_idx, const T2 &input)
{
    iterator ret(this, key_idx);
    if (!index.isvalid(key_idx))
    {
        ret.isvalid = false;
        return ret;
    }

    //prepare the ret value
    ret.isvalid = true;
    ret.idx = key_idx;

    int ptr = index[key_idx].pointer;

    //it is the first element for the designated key
    if (ptr == -1)
    {
        adj_element_t ele;
        ele.adjdata = input;
        ele.next = -1;
        int n_adjele_idx = _adjlist.add(ele);
        //Link the next to itself to build a circle
        _adjlist[n_adjele_idx].next = n_adjele_idx;
        index[key_idx].pointer = n_adjele_idx;
        //settle the ret
        ret.start_ptr = ret.curr_ptr = n_adjele_idx;
        ret.prev_ptr = n_adjele_idx;
    } else //it is not the first element for the designated key
    {
        int prev_ptr = ptr;
        adj_element_t ele;
        ele.adjdata = input;
        ele.next = _adjlist[iter.curr_ptr].next;
        int n_adjele_idx = _adjlist.add(ele);
        _adjlist[iter.curr_ptr].next = n_adjele_idx;
        //settle the ret
        ret = iter;
        if (ele.next == ret.curr_ptr)
        {
            ret.prev_ptr = n_adjele_idx;
        }
    }
    return ret;
}

template<class T1, uint32_t initsizet1, class T2, uint32_t initsizet2>
inline void adjlist<T1, initsizet1, T2, initsizet2>::init(uint32_t size, const T1& input)
{
    index_t ele;
    ele.data = input;
    ele.pointer = -1;
    index.init();
    for (int i=0;i<size;++i)
        index.add(ele);
}

template<class T1, uint32_t initsizet1, class T2, uint32_t initsizet2>
inline bool adjlist<T1, initsizet1, T2, initsizet2>::is_keyvalid(int key_idx)
{
    return index.isvalid(key_idx);
}

template<class T1, uint32_t initsizet1, class T2, uint32_t initsizet2>
inline adjlist_iterator_t<T1, initsizet1, T2, initsizet2>
adjlist<T1, initsizet1, T2, initsizet2>::del_element(iterator iter)
{
    if (!iter.isvalid) return iter;
    int curr_ptr = iter.curr_ptr;
    iterator ret(iter.adj, iter.idx);
    //itself is the only element in the list
    if (_adjlist[curr_ptr].next == curr_ptr)
    {
        index[iter.idx].pointer = -1;
        _adjlist.del(curr_ptr);
        ret.isvalid = false;
        ret.curr_ptr = ret.prev_ptr = ret.start_ptr = -1;
        return ret;
    } else //itself is on a ring, and it is not the last
    {
        int prev_ptr = iter.prev_ptr;
        int next_ptr = _adjlist[curr_ptr].next;
        _adjlist[prev_ptr].next = next_ptr;
        _adjlist.del(curr_ptr);
        if (index[iter.idx].pointer == curr_ptr)
            index[iter.idx].pointer = next_ptr;
        if (iter.start_ptr == curr_ptr)
            iter.start_ptr = next_ptr;
        ret = iter;
        ret.curr_ptr = next_ptr;
        return ret;
    }
}

template<class T1, uint32_t initsizet1, class T2, uint32_t initsizet2>
inline adjlist_iterator_t<T1, initsizet1, T2, initsizet2>
adjlist<T1, initsizet1, T2, initsizet2>::begin(int key_idx)
{
    iterator iter(this, key_idx);
    if ((!index.isvalid(key_idx)) || (index[key_idx].pointer == -1))
    {
        iter.isvalid = false;
        return iter;
    }
    iter.isvalid = true;
    iter.prev_ptr = index[key_idx].pointer;
    iter.start_ptr = iter.curr_ptr = _adjlist[iter.prev_ptr].next;
    return iter;
}
template<class T1, uint32_t initsizet1, class T2, uint32_t initsizet2>
inline adjlist_iterator_t<T1, initsizet1, T2, initsizet2> &
adjlist_iterator_t<T1, initsizet1, T2, initsizet2>::next()
{
    adj->index[idx].pointer = curr_ptr;
    int n_next_ptr = adj->_adjlist[curr_ptr].next;
    //Already one round
    if (n_next_ptr == start_ptr)
    {
        isvalid = false;
        return *this;
    }
    prev_ptr = curr_ptr;
    curr_ptr = n_next_ptr;
    return *this;
};

template<class T1, uint32_t initsizet1, class T2, uint32_t initsizet2>
inline bool adjlist_iterator_t<T1, initsizet1, T2, initsizet2>::end()
{
    if (!isvalid) return true;
    return false;
};

template<class T1, uint32_t initsizet1, class T2, uint32_t initsizet2>
inline T2& adjlist_iterator_t<T1, initsizet1, T2, initsizet2>::operator*()
{
    return adj->_adjlist[curr_ptr].adjdata;
}

template<class T1, uint32_t initsizet1, class T2, uint32_t initsizet2>
inline T2* adjlist_iterator_t<T1, initsizet1, T2, initsizet2>::operator->()
{
    return &(adj->_adjlist[curr_ptr].adjdata);
};

template<class T1, uint32_t initsizet1, class T2, uint32_t initsizet2>
inline int adjlist<T1, initsizet1, T2, initsizet2>::hiter_begin()
{
    return index.iterator_init();
}

template<class T1, uint32_t initsizet1, class T2, uint32_t initsizet2>
inline int adjlist<T1, initsizet1, T2, initsizet2>::hiter_next(int prev)
{
    return index.iterator_next(prev);
}

#endif //IPC_DIRECT_ADJLIST_T_HPP
