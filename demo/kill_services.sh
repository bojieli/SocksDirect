#!/bin/bash
pkill nginx
pkill web_ser
pkill memcached
pkill http_bench
pkill monitor
rm /usr/share/nginx/html/data/*.txt
sleep 1
systemctl start nginx
ps aux | grep demo
