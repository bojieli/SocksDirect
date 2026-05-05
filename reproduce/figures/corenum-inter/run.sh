#!/usr/bin/env bash
# corenum-inter — inter-host throughput as core count scales (paper Fig 4-right).
. "$(dirname "$0")/../_lib.sh"
require_repro_env
require_two_hosts
require_libsd 3
skip_figure "corenum-inter driver not yet wired (needs Phase 6 follow-up: paper Fig 4-right)"
