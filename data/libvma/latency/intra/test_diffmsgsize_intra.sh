#!/usr/bin/env bash
ip="10.1.2.34"
for (( msgsize=8; msgsize<=1048576; msgsize*=2 ))
do
    echo $msgsize
    filename="samples-intra-$msgsize.out"
    LD_PRELOAD=libvma.so ../../../../build/pot_eval_linux_lat_s $msgsize &
    pid1=$!
    sleep 1
    LD_PRELOAD=libvma.so ../../../../build/pot_eval_linux_lat_c $filename $ip $msgsize
    wait $pid1
done
