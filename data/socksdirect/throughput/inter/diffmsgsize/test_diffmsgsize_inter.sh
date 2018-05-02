#!/usr/bin/env bash
ip="10.1.2.4"
for (( msgsize=8; msgsize<=1048576; msgsize*=2 ))
do
    echo $msgsize
    filename="inter-$msgsize.out"
    pkill pot
    ssh -n 10.1.2.4 "pkill pot"
    ssh -n 10.1.2.4 "systemctl restart memcached"
    HRD_REGISTRY_IP=10.1.2.4 ../../../../../build/pot_eval_rdma_tput_s $msgsize $filename &
    sleep 1
    ssh -n 10.1.2.4 "cd /home/boj/libsd/build; HRD_REGISTRY_IP=10.1.2.4 ./pot_eval_rdma_tput_c $msgsize $filename"
    wait
done
