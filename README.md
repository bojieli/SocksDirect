# SocksDirect: Fast and Compatible Sockets in User Space

Linux socket is implemented in the kernel space with shared data structures that needs concurrency protection, which incurs significant overhead. Communication intensive applications in hosts with multi-core CPU and highspeed networking hardware often put considerable stress on the socket system. Recent work on user-space sockets either does not support intra-host communication among containers and applications, or has limitations on compatibility, isolation and multi-thread scalability.

In this paper, we describe SOCKSDIRECT, a high performance socket system. SOCKSDIRECT is implemented in user space to avoid kernel crossing cost. It achieves security and isolation by employing a trusted monitor daemon to handle control plane operations such as connection establishment and access control. SOCKSDIRECT is fully compatible with Linux socket and can be used as a drop-in replacement with no modification to existing applications. The design fully handles Linux fork semantics, and can handle both intra- and inter-host communications with hosts equipped with SOCKSDIRECT as well as those without. Last but not least, SOCKSDIRECT is performant. SOCKSDIRECT uses shared memory queue and modern RDMA transport for intra- and inter-host communication. It removes multithread synchronization in common cases and improves memory efficiency with many concurrent connections. It leverages techniques such as cooperative multitasking and pageremapping based zero-copy to remove many overheads of existing socket systems. Experiment shows that SOCKSDIRECT achieves 7 to 20x better message throughput, 17 to 35x better latency, and 20x connection setup throughput compared with Linux socket.

Please refer to the paper for technical details.

## Build

Currently SocksDirect is available on Linux using ```cmake``` build system.

1. Create a build directory (e.g. ```build```).
2. ```cd``` to the build directory.
3. ```cmake ..```
4. ```make -j```
5. The binaries should be built in the ```cmake``` directory.

## Usage

### Standalone tests without ```LD_PRELOAD```

In build directory, run ```./test_sock_server``` in background (or in another terminal), then run ```./test_sock_client```. It should print the throughput (messages per second) every second.

Similarly, we can run the pairs of ```./test_sock_server_2``` and ```./test_sock_client_2```, as well ```./test_sock_server_3``` and ```./test_sock_client_3```.

The code for the tests are in ```$libsd/test/```.

### Tests with ```LD_PRELOAD```

```LD_PRELOAD=$libsd/build/libsd.so $progname $args```

You can use a standard BSD socket application (e.g. ```iperf```).

The current code branch is not fully tested with many applications.

### Tests to replicate experiments in the paper

They are ```./pot_server_*``` and ```./pot_client_*```. Documents TODO.

The code for the tests are in ```$libsd/pot/```.

## Application experiment
    In demo folder, there is a sample nginx configure file, please change the path accordingly. 
    If you want to see more errors. Change the error level in the config.
    In your build folder, run ./pot_web_service as the backend service
    run ./test_http_client -p <port> -b <batch size per request> -n <thread_number> <IP> <test response size>
