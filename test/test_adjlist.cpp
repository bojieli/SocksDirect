//
// Created by ctyi on 1/5/18.
//

#include <cstdio>
#include "../common/adjlist_t.hpp"
//define a adjlist with 4 template parameter <T1, size1, T2, size2>
//T1 is the type of the index
//T2 is the value type of each element in the index
//the adjlist is indexed by a unsigned_int
adjlist<int, 100, int, 100> adjlist1;
int main()
{
    //init all the index, mark them valid, otherwise please use adjlist1.add_key to add an index
    adjlist1.init(100, 0);
    adjlist1[2] = 3;
    printf("value in the key 2: %d\n", adjlist1[2]);

    //add elements for the key 2
    for (int i=0;i<100;++i)
        adjlist1.add_element(2, i);
    //traverse the list
    for(auto iter = adjlist1.begin(2); !iter.end(); iter=iter.next())
    {
        printf("%d ", *iter);
    }
    printf("\n");
    printf("Trying to modify a value in the element\n");
    for(auto iter = adjlist1.begin(2); !iter.end(); iter=iter.next())
    {
        if (*iter == 99)
            *iter = 100;
    }
    printf("Print Again\n");
    for(auto iter = adjlist1.begin(2); !iter.end(); iter=iter.next())
    {
        printf("%d ", *iter);
    }
    printf("\nIterate two times, one time 50 element\n");
    auto iter=adjlist1.begin(2);
    printf("Times 1:");
    for (int i=0;i<50;++i)
    {
        printf("%d ", *iter);
        iter.next();
    }
    printf("\nTimes 2:");
    iter = adjlist1.begin(2);
    for (int i=0;i<50;++i)
    {
        printf("%d ", *iter);
        iter.next();
    }
    printf("\nTry to delete 98 elements\n Deled: ");
    iter = adjlist1.begin(2);
    for (int i=0; i<98;++i)
    {
        //caution: after the delation, the return value will point to the next
        printf("%d ", *iter);
        iter = adjlist1.del_element(iter);
    }
    printf("\nIterate again: ");
    for (iter = adjlist1.begin(2); !iter.end(); iter = iter.next())
    {
        printf("%d ", *iter);
    }
    printf("\nTry to delete another one");
    iter = adjlist1.begin(2);
    iter = adjlist1.del_element(iter);
    iter = adjlist1.begin(2);
    printf("\nLeft : %d\n", *iter);
    printf("Delete last one\n");
    adjlist1.del_element(adjlist1.begin(2));
    printf("check whether exists: %d", !adjlist1.begin(2).end());
    printf("\n");
    return 0;
}