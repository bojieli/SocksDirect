#!/usr/bin/env bash
ip="127.0.0.1"
msgsize=8
for (( corenum=0; corenum<=15; corenum+=1 ))
do
    echo "Testing $corenum"
    filename="intra-$(($corenum+1)).out"
    pkill pot
    ipcrm -a
    ssh -n 10.1.2.4 "pkill pot; ipcrm -a"
    ssh -n 10.1.2.4 "systemctl restart memcached"
    rm $filename
    for (( core=0; core<=corenum; core+=1 ))
    do
        HRD_REGISTRY_IP=10.1.2.4 ../../../../../build/pot_eval_rdma_tput_s $msgsize $filename $core &
    done
    sleep 3
    for (( core=0; core<=corenum; core+=1 ))
    do
        ssh -n -f 10.1.2.4 "cd /home/boj/libsd/build; HRD_REGISTRY_IP=10.1.2.4 nohup ./pot_eval_rdma_tput_c $msgsize $filename $core &"
    done
    wait
done
