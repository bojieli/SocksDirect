#!/usr/bin/env bash
ip="10.1.2.34"
corenum=1
msgsize=8
for (( msgsize=8; msgsize<=1048576; msgsize*=2 ))
do
    echo $msgsize
    filename="samples-intra-$msgsize.out"
    pkill monitor
    ../../../../build/monitor &
    sleep 1
    LD_PRELOAD=../../../../build/libipc.so ../../../../build/pot_eval_linux_lat_s $msgsize $corenum &
    pid1=$!
    sleep 1
    LD_PRELOAD=../../../../build/libipc.so ../../../../build/pot_eval_linux_lat_c $ip $msgsize $corenum &
    wait $pid1
    pkill monitor
done
