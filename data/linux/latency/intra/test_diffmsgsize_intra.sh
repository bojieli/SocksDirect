#!/usr/bin/env bash
ip="127.0.0.1"
for (( msgsize=131072; msgsize<=1048576; msgsize*=2 ))
do
    echo $msgsize
    filename="samples-intra-$msgsize.out"
    ../../../../cmake-build-release/pot_eval_linux_lat_s $msgsize &
    pid1=$!
    sleep 1
    ../../../../cmake-build-release/pot_eval_linux_lat_c $filename $ip $msgsize
    wait $pid1
done
