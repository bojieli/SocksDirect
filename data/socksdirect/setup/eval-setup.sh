#!/usr/bin/env bash
ip="127.0.0.1"
for (( corenum=$1; corenum<=15; corenum+=1 ))
do
    echo "Testing $corenum"
    filename="setup-$(($corenum+1)).out"
    pkill pot
    rm setup.out
    for (( core=0; core<=corenum; core+=1 ))
    do
        ../../../build/pot_eval_create_s $((($core * 4) % 29)) &
    done
    sleep 1
    for (( core=0; core<=corenum; core+=1 ))
    do
        ../../../build/pot_eval_create_c $((($core * 4 + 2)%29)) &
    done
    wait
    mv setup.out $filename
    pkill pot
done
