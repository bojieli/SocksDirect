#!/bin/bash
if [ -z "$1" ]; then set $1 1; fi
./start_nginx.sh $1
./start_memcached.sh $1
./start_web_service.sh $1

