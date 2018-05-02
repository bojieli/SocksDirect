#!/usr/bin/env bash
ip="127.0.0.1"
msgsize="8"
filename="intra.out"
thread="0"
for (( connnum=10000000; connnum<=1073741824; connnum*=10 ))
do
    echo $connnum
    pkill monitor
    ipcrm -a
    ../../../../../build/monitor &
    sleep 1
    ../../../../../build/pot_eval_tput_diffconnnum_s $msgsize $filename $thread $connnum &
    pid1=$!
    sleep 1
    ../../../../../build/pot_eval_tput_diffconnnum_c $msgsize $filename $thread $connnum
    wait $pid1
done
