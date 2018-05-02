#!/usr/bin/env bash
ip="127.0.0.1"
msgsize=8
filename="inter.out"
core=10
for (( connnum=1; connnum<=1000000000; connnum*=10 ))
do
    echo "Testing $connnum"
    pkill pot
    ipcrm -a
    ssh -n 10.1.2.4 "pkill pot; ipcrm -a"
    ssh -n 10.1.2.4 "systemctl restart memcached"
    rm $filename
    HRD_REGISTRY_IP=10.1.2.4 ../../../../../build/pot_eval_rdma_tput_diffconnnum_s $msgsize $filename $core $connnum &
    sleep 1
    ssh -n -f 10.1.2.4 "cd /home/boj/libsd/build; HRD_REGISTRY_IP=10.1.2.4 nohup ./pot_eval_rdma_tput_diffconnnum_c $msgsize $filename $core $connnum &"
    wait
done
