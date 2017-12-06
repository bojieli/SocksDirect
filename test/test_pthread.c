//
// Created by ctyi on 11/23/17.
//

#include <stdio.h>
#include <unistd.h>
#include "../lib/lib.h"
#include <pthread.h>

void *child_thread(void *arg)
{
    printf("Child thread!\n");
    ipclib_sendmsg(REQ_PING, 8);
    metaqueue_element data;
    ipclib_recvmsg(&data);
    printf("child back data%d\n", data.data.command.data);
    return NULL;
}

int main()
{
    pthread_t thread;
    pthread_create(&thread, NULL, child_thread, NULL);
    printf("Parent process!");
    ipclib_sendmsg(REQ_PING, 16);
    metaqueue_element data;
    ipclib_recvmsg(&data);
    printf("parent back data%d\n", data.data.command.data);
    pthread_join(thread, NULL);
    return 0;
}