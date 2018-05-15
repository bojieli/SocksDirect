#!/usr/bin/env bash
source $(dirname $0)/scripts/utils.sh
source $(dirname $0)/scripts/mlx_env.sh
export HRD_REGISTRY_IP="10.1.2.4"

drop_shm

executable="../build/rdma_throughput"
blue "Running $1 client threads, each with $2 qps"

# Check number of arguments
#if [ "$#" -gt 2 ]; then
#  blue "Illegal number of arguments."
#  blue "Usage: ./run-machine.sh <machine_id>, or ./run-machine.sh <machine_id> gdb"
#	exit
#fi
#
#if [ "$#" -eq 0 ]; then
#  blue "Illegal number of arguments."
#  blue "Usage: ./run-machine.sh <machine_id>, or ./run-machine.sh <machine_id> gdb"
#	exit
#fi

flags="\
  --num_threads $1 \
	--dual_port 0 \
	--use_uc 0 \
	--is_client 1 \
	--postlist 1 \
	--machine_id 0 \
	--do_read 0 \
	--num_qps $2
"
shift
shift

# Check for non-gdb mode
if [ "$#" -eq 0 ]; then
  sudo -E numactl --cpunodebind=0 --membind=0 $executable $flags
fi

# Check for gdb mode
if [ "$#" -eq 1 ]; then
  sudo -E gdb -ex run --args $executable $flags
fi
