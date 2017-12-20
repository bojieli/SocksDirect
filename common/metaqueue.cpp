#include "metaqueue.h"
#include "../common/helper.h"

void metaqueue_push(metaqueue_pack q_pack, metaqueue_element *data)
{
    metaqueue_data *q = q_pack.data;
    metaqueue_meta_t *q_m = q_pack.meta;
    metaqueue_element *tail = &q->data[q_m->pointer & METAQUEUE_MASK];
    //is full?
    while (tail->is_valid)
            SW_BARRIER;
    data->is_valid = 0;
    SW_BARRIER;
    *tail = *data;
    SW_BARRIER;
    tail->is_valid = 1;
    SW_BARRIER;
    q_m->pointer++;
}

void metaqueue_pop(metaqueue_pack q_pack, metaqueue_element *data)
{
    metaqueue_data *q = q_pack.data;
    metaqueue_meta_t *q_m = q_pack.meta;
    metaqueue_element *head = &q->data[q_m->pointer & METAQUEUE_MASK];
    //is empty?
    while (!head->is_valid)
            SW_BARRIER;
    *data = *head;
    SW_BARRIER;
    head->is_valid = 0;
    SW_BARRIER;
    q_m->pointer++;
}

int metaqueue_isempty(metaqueue_pack q_pack)
{
    metaqueue_data *q = q_pack.data;
    metaqueue_meta_t *q_m = q_pack.meta;
    return !q->data[q_m->pointer & METAQUEUE_MASK].is_valid;
}

void metaqueue_init_data(metaqueue_pack q_pack)
{
    memset(q_pack.data->data, 0, sizeof(metaqueue_data));
}

void metaqueue_init_meta(metaqueue_pack q_pack)
{
    q_pack.meta->pointer = 0;
}
