#!/usr/bin/env bash
ip="127.0.0.1"
for (( corenum=$1; corenum<=15; corenum+=1 ))
do
    echo "Testing $corenum"
    filename="intra-$(($corenum+1)).out"
    pkill pot
    ipcrm
    rm $filename
    for (( core=0; core<=corenum; core+=1 ))
    do
        ../../../../../build/pot_eval_tput_s 8 $filename $core &
    done
    sleep 1
    for (( core=0; core<=corenum; core+=1 ))
    do
        ../../../../../build/pot_eval_tput_c 8 $filename $core &
    done
    wait
    pkill pot
done
