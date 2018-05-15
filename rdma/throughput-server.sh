#!/usr/bin/env bash
source $(dirname $0)/scripts/utils.sh
source $(dirname $0)/scripts/mlx_env.sh
export HRD_REGISTRY_IP="10.1.2.4"

drop_shm

num_server_threads=1

blue "Reset server QP registry"
sudo killall memcached
memcached -l 0.0.0.0 1>/dev/null 2>/dev/null &
sleep 1

blue "Starting $1 server threads, each with $2 qps"

flags="
	--num_threads $1 \
	--dual_port 0 \
  --use_uc 0 \
	--is_client 0 \
	--size $3 \
	--postlist 1 \
	--do_read 0 \
    --do_write_with_imm 1 \
    --do_send 0 \
	--num_qps $2
"
shift
shift
shift

# Check for non-gdb mode
if [ "$#" -eq 0 ]; then
  sudo -E numactl --cpunodebind=0 --membind=0 ../build/rdma_throughput $flags
fi

# Check for gdb mode
if [ "$#" -eq 1 ]; then
  sudo -E gdb -ex set follow-fork-mode child -ex run --args ../build/rdma_throughput $flags
fi
