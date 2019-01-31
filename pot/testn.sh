#!/usr/bin/env bash
CNIC=mlx4_0
RNIC=mlx4_1
RPATH=/sampa/home/cuity/projects/libsd/cmake-build-release
CPATH=/sampa/home/cuity/projects/libsd/cmake-build-release
RIP=10.2.5.203
for (( MSGSIZE=32; MSGSIZE < 4096 ; MSGSIZE*=2 ))
do
    echo $MSGSIZE
    ssh $RIP "cd $RPATH && echo $RNIC > rdma_config.txt"
    ssh $RIP "cd $RPATH/../cmake-build-debug/ && ./monitor" &
    PIDM=$!
    sleep 5
    ssh $RIP "cd $RPATH && LD_PRELOAD=./libipc.so ./pot_web_service" &
    PIDW=$!
    sleep 5
    ssh $RIP "cd $RPATH && LD_PRELOAD=./libipc.so /usr/sbin/nginx -c $RPATH/nginx.conf" &
    PIDN=$!
    echo $CNIC > rdma_config.txt
    sleep 10
    ./monitor &
    sleep 5
    echo !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!1
    LD_PRELOAD=./libipc.so ./test_http_client -p 8080 -n 1 -b 1 $RIP $MSGSIZE > data_$MSGSIZE.txt
    ssh $RIP "killall monitor; killall nginx; killall pot_web_service"
    killall monitor
    sleep 5
    #kill $PIDM
    #kill $PIDW
    #kill $PIDN
done

