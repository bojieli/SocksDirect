#!/usr/bin/env bash
ip="10.1.2.34"
msgsize=8
for (( corenum=1; corenum<=16; corenum+=1 ))
do
    echo $corenum
    filename="samples-intra-$msgsize.out"
    LD_PRELOAD=/usr/lib/rsocket/librspreload.so ../../../../../build/pot_eval_linux_tput_s $msgsize $corenum &
    pid1=$!
    sleep 1
    LD_PRELOAD=/usr/lib/rsocket/librspreload.so ../../../../../build/pot_eval_linux_tput_c $ip $msgsize $corenum
    wait $pid1
done
