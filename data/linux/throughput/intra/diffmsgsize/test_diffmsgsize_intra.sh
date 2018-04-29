#!/usr/bin/env bash
ip="127.0.0.1"
for (( msgsize=8; msgsize<=1048576; msgsize*=2 ))
do
    echo $msgsize
    filename="samples-intra-$msgsize.out"
    ../../../../../build/pot_eval_linux_tput_s $msgsize 1 2>>data.out &
    pid1=$!
    sleep 1
    ../../../../../build/pot_eval_linux_tput_c $ip $msgsize 1 
    wait $pid1
done
