#!/usr/bin/env bash
ip="10.1.2.4"
for (( msgsize=8; msgsize<=1048576; msgsize*=2 ))
do
    echo $msgsize
    ../../../../../build/pot_eval_linux_tput_s $msgsize 1 &
    pid1=$!
    ssh  10.1.2.4 "cd /home/ctyi/work/ipc/src/build; nohup ./pot_eval_linux_tput_c 10.1.2.34 $msgsize 1 &"
    wait $pid1
done
