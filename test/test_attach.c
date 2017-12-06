//
// Created by ctyi on 11/14/17.
//

#include <stdio.h>
#include "../lib/lib.h"

int main()
{
    //ipclib_sendmsg(REQ_NOP, 0);
    //metaqueue_element data;
    //ipclib_recvmsg(&data);
    //printf("msg :%d\n", data.data.command.data);
    printf("Hello world!\n");
    pin_thread(2);
    long counter = 0;
    ipclib_sendmsg(REQ_THRTEST_INIT, 0);
    while (1)
    {
        ipclib_sendmsg(REQ_THRTEST, counter);
        ++counter;
        //printf("%d", counter);
    }
    return 0;
}