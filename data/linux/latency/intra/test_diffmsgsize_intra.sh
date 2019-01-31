#!/usr/bin/env bash
ip="127.0.0.1"
for (( msgsize=8; msgsize<=1048576; msgsize*=2 ))
do
    echo $msgsize
    filename="samples-intra-$msgsize.out"
    ../../../../build/pot_eval_linux_lat_s $msgsize 8081 &
    pid1=$!
    sleep 1
    ../../../../build/pot_eval_linux_lat_c $filename $ip $msgsize 8081
    wait $pid1
done
