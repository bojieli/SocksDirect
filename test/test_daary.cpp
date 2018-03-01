//
// Created by ctyi on 12/8/17.
//

#include <cstdio>
#include "../common/darray.hpp"
#include "../common/helper.h"
int main()
{
    darray_t<int, 32> darr;
    for (unsigned int i=0;i<63;++i)
    {
        unsigned int ret;
        ret=darr.add(i);
        if (ret != i)
            FATAL("insert error");
    }

    printf("Arr: ");
    for (int i=0;i<63;++i)
        printf("%d ", darr[i]);
    printf("\n");

    for (int i=0;i<63;i+=2)
        darr.del(i);
    printf("highest %u, total %u\n", darr.get_highest_possible(), darr.get_totalsize());

    for (int i=0;i<65;i+=2)
        darr.add(i);

    printf("Arr: ");
    for (int i=0;i<63;++i)
        printf("%d ", darr[i]);
    printf("\n");
    printf("highest %u, total %u\n", darr.get_highest_possible(), darr.get_totalsize());

    for (int i=30;i<64;++i)
        darr.del(i);
    printf("highest %u, total %u\n", darr.get_highest_possible(), darr.get_totalsize());


}