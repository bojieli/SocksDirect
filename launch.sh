#!/usr/bin/env bash
CURR_PATH=`pwd`
LIB_PATH=$CURR_PATH'/cmake-build-debug/libipc.so'
LD_PRELOAD=$LIB_PATH $1

