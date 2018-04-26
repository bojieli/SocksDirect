#!/usr/bin/env bash
ip="10.1.2.4"
for (( msgsize=8; msgsize<=1048576; msgsize*=2 ))
do
    echo $msgsize
    filename="samples-inter-$msgsize.out"
    ssh -n -f 10.1.2.4 "LD_PRELOAD=libvma.so cd /home/ctyi/work/ipc/src/build;nohup ./pot_eval_linux_lat_s $msgsize &"
    sleep 1
    LD_PRELOAD=libvma.so ../../../../build/pot_eval_linux_lat_c $filename $ip $msgsize
done
