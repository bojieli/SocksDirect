#!/usr/bin/env bash
ip="10.1.2.34"
for (( msgsize=8; msgsize<=1048576; msgsize*=2 ))
do
    echo $msgsize
    filename="samples-intra-$msgsize.out"
    LD_PRELOAD=/usr/lib/rsocket/librspreload.so ../../../../build/pot_eval_linux_lat_s $msgsize 8081 &
    pid1=$!
    sleep 1
    ssh 10.1.2.4 "hostname; LD_PRELOAD=/usr/lib/rsocket/librspreload.so /home/boj/libsd/build/pot_eval_linux_lat_c $filename $ip $msgsize 8081"
    wait $pid1
done
