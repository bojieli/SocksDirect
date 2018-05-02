#!/usr/bin/env bash
ip="10.1.2.34"
for (( msgsize=8; msgsize<=1048576; msgsize*=2 ))
do
    echo $msgsize
    filename="samples-intra-$msgsize.out"
    ../../../../build/pot_eval_lat_s $msgsize &
    pid1=$!
    sleep 1
    ../../../../build/pot_eval_lat_c $filename $msgsize
    wait $pid1
done
