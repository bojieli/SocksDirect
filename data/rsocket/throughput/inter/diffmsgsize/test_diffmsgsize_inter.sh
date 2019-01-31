#!/usr/bin/env bash
ip="10.1.2.4"
msgsize=8
corenum=1
for (( msgsize=8; msgsize <= 1048576; msgsize *= 2 ))
do
    echo $corenum
    LD_PRELOAD=/usr/lib/rsocket/librspreload.so ../../../../../build/pot_eval_linux_tput_s $msgsize $corenum &
    pid1=$!
    sleep 1
    ssh  10.1.2.4 "cd /home/boj/libsd/build; LD_PRELOAD=/usr/lib/rsocket/librspreload.so ./pot_eval_linux_tput_c 10.1.2.34 $msgsize $corenum"
    wait $pid1
done
