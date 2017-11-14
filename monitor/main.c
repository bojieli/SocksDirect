//
// Created by ctyi on 11/10/17.
//

#include "../common/helper.h"
#include "attachqueue_monitor.h"
#include "../common/attachqueue.h"
#define DEBUGON 1
int main()
{
    attachqueue_sysinit();
    ctl_struc data;
    while (1) {
        if (!attachqueue_isempty())
        {
            attachqueue_pullreq(&data);
            DEBUG("process id: %d\n", data.pid);
        }
    }
    return 0;
}