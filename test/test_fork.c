//
// Created by ctyi on 11/23/17.
//

#include <stdio.h>
#include <unistd.h>
#include "../lib/lib.h"
#include "../common/metaqueue.h"

int main()
{
    pid_t ret = fork();
    if (ret == 0)
    {
        printf("Child process!");
        ipclib_sendmsg(REQ_PING, 8);
        metaqueue_element data;
        ipclib_recvmsg(&data);
        printf("child back data%d\n", data.data.command.data);
    } else
    {
        printf("Parent process!");
        ipclib_sendmsg(REQ_PING, 16);
        metaqueue_element data;
        ipclib_recvmsg(&data);
        printf("parent back data%d\n", data.data.command.data);
    }
    return 0;
}