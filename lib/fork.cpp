//
// Created by ctyi on 1/25/18.
//
#include "fork.h"
#include "lib_internal.h"

int fork_traverse_rd_tree(int curr_idx, int match_bid)
{
    if (curr_idx==-1)
        return -1;
    thread_data_t *tdata = GET_THREAD_DATA();
    fd_list_t curr_node = tdata->rd_tree[curr_idx];
    if (match_bid == curr_node.buffer_idx)
        return curr_idx;
    int ret;
    if (tdata->rd_tree[curr_idx].child[0] != -1)
    {
        ret = fork_traverse_rd_tree(tdata->rd_tree[curr_idx].child[0], match_bid);
        if (ret != -1) return ret;
    }
    if (tdata->rd_tree[curr_idx].child[1] != -1)
    {
        ret = fork_traverse_rd_tree(tdata->rd_tree[curr_idx].child[1], match_bid);
        if (ret != -1) return ret;
    }
    return -1;

}