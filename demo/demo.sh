#!/bin/bash

function showmsg() {
    echo
    echo "============================================="
    echo $1
    echo "============================================="
}

function start_web_server() {
    showmsg "Starting Web Server"
    showmsg "Starting memcached with $1 core"
    set -x
    ./start_memcached.sh $1
    set +x
    sleep 1
    showmsg "Starting nginx with $1 core"
    set -x
    ./start_nginx.sh $1
    set +x
    sleep 1
    showmsg "Starting web backend service with $1 core"
    set -x
    ./start_web_service.sh $1
    set +x
}

function kill_services() {
    ./kill_services.sh >/dev/null 2>&1
}

showmsg "IPC-DIRECT DEMO BEGIN"

pkill demo_cpu
./demo_cpu_util /usr/share/nginx/html/data/cpu.txt >/dev/null 2>&1 &
kill_services
systemctl start nginx

start_web_server 1
sleep 3

showmsg "Now we check that the Web server is running on netsys34:8080"
set -x
curl http://netsys34:8080/
set +x

sleep 3

showmsg "For each refresh, the counter increments by 1"
set -x
curl http://netsys34:8080/
sleep 1
curl http://netsys34:8080/
sleep 1
curl http://netsys34:8080/
set +x

sleep 3

showmsg "Launch HTTP benchmark for 20 seconds. See the graphs for throughput and latency."
set -x
timeout 20s ./demo_http_bench 1
set +x

showmsg "Benchmark complete."
sleep 1

showmsg "Now we access a slow page that accesses 100,000 records in the Memcached key-value store"
set -x
time curl http://netsys34:8080/slow_query
set +x

showmsg "It takes more than 1,000 milliseconds."

sleep 3

showmsg "Next we see how IPC-Direct can accelerate sockets. Kill the services."
set -x
kill_services
set +x

sleep 1

showmsg "Now we start IPC-Direct accelerator."
showmsg "Step #1. Launch monitor service."
set -x
./start_monitor.sh 1
set +x

sleep 3

showmsg "Step #2. Set environment variable to preload IPC library."
export LD_PRELOAD=./libipc.so
set +x

sleep 3

showmsg "Now IPC-Direct is ready to accelerate applications."
showmsg "Try 'ls' and it will show IPC library is loaded."
set -x
ls
set +x

sleep 1

start_web_server 1

sleep 3

showmsg "Launch HTTP benchmark for 20 seconds. See the graphs for throughput and latency."
set -x
timeout 20s ./demo_http_bench 1
set +x

sleep 1

showmsg "Now we access the slow page that accesses 100,000 records in the Memcached key-value store"
set -x
time curl http://netsys34:8080/slow_query
set +x

showmsg "It takes roughly 50 milliseconds."

sleep 3

showmsg "Next we show multi-core scalability of IPC-Direct"
showmsg "Stop services and IPC-Direct"
set -x
export LD_PRELOAD=
kill_services
set +x

sleep 3

start_web_server 8
sleep 1

showmsg "Now we check that the Web server is running on netsys34:8080"
set -x
curl http://netsys34:8080/
set +x

sleep 1

showmsg "Launch HTTP benchmark for 20 seconds. See the graphs for throughput and latency."
set -x
timeout 20s ./demo_http_bench 8
set +x

showmsg "Benchmark complete."
sleep 1

showmsg "Kill services."
set -x
./kill_services.sh
set +x

showmsg "Now we start IPC-Direct accelerator."
showmsg "Step #1. Launch monitor service."
set -x
./start_monitor.sh 1
set +x

sleep 1

showmsg "Step #2. Set environment variable to preload IPC library."
set -x
export LD_PRELOAD=./libipc.so
set +x

sleep 1

showmsg "Now IPC-Direct is ready to accelerate socket applications."
start_web_server 8

sleep 1

showmsg "Now we check that the Web server is running on netsys34:8080"
set -x
curl http://netsys34:8080/
set +x

sleep 1
showmsg "Launch HTTP benchmark for 20 seconds. See the graphs for throughput and latency."
set -x
timeout 20s ./demo_http_bench 8
set +x

showmsg "Benchmark complete."
sleep 1

pkill demo_cpu
kill_services
showmsg "END OF IPC-DIRECT DEMO"
