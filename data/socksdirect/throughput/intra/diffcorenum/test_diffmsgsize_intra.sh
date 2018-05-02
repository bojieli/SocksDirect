#!/usr/bin/env bash
ip="127.0.0.1"
msgsize=8
for (( corenum=13; corenum<=16; corenum+=1 ))
do
    echo $corenum
    filename="samples-intra-$msgsize.out"
    LD_PRELOAD=libvma.so ../../../../../build/pot_eval_linux_tput_s $msgsize $corenum &
    pid1=$!
    sleep 1
    LD_PRELOAD=libvma.so ../../../../../build/pot_eval_linux_tput_c $ip $msgsize $corenum 
    wait $pid1
done
