#!/usr/bin/env bash
ip="127.0.0.1"
for (( msgsize=8; msgsize<=1048576; msgsize*=2 ))
do
    echo $msgsize
    filename="intra.out"
    ../../../../../build/pot_eval_tput_s $msgsize $filename &
    pid1=$!
    sleep 1
    ../../../../../build/pot_eval_tput_c $msgsize
    wait $pid1
done
