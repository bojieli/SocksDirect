#!/usr/bin/env bash
ip="10.1.2.4"
for (( msgsize=8; msgsize<=1048576; msgsize*=2 ))
do
    echo $msgsize
    filename="samples-inter-$msgsize.out"
    ssh -n -f 10.1.2.4 "sudo systemctl restart memcached; pkill pot 2>&1"
    echo "start remote"
    ssh -n -f 10.1.2.4 "cd /home/boj/libsd/build; HRD_REGISTRY_IP=10.1.2.4 ./pot_eval_rdma_lat_s $msgsize" &
    pid=$!
    sleep 3
    echo "start local"
    pkill pot 2>&1
    HRD_REGISTRY_IP=10.1.2.4 ../../../../build/pot_eval_rdma_lat_c $filename $msgsize
    wait $pid
done
