#!/usr/bin/env bash
ip="10.1.2.4"
msgsize=8
for (( corenum=1; corenum<=16; corenum+=1 ))
do
    echo $corenum
    LD_PRELOAD=/usr/lib/rsocket/librspreload.so ../../../../../build/pot_eval_linux_tput_s $msgsize $corenum &
    pid1=$!
    sleep 1
    ssh  10.1.2.4 "cd /home/boj/libsd/build; LD_PRELOAD=/usr/lib/rsocket/librspreload.so ./pot_eval_linux_tput_c 10.1.2.34 $msgsize $corenum"
    wait $pid1
done
