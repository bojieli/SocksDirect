# nfv — NFV pipeline throughput

Pipeline of small network functions (firewall → meter → NAT) piped
together via libsd. Reproduces the paper's eval-tun-tput figure.

Driver lives at `apps/nfv-pipeline/pipeline.sh` (see that directory for
the NF processes).
