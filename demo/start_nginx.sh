#!/bin/bash
sed -i "s/worker_processes.*\$/worker_processes $1;/" ./nginx.conf
./nginx -c /root/IPC-Direct/demo/nginx.conf &
