# redis-demo

Run `redis-server` (and a small `redis-cli` benchmark) under
`LD_PRELOAD=libsd.so`.

## Quick start

```bash
sudo apt install redis-server redis-tools
./run-demo.sh                       # in one terminal
./client-demo.sh                    # in another
```

The client runs `redis-benchmark` against the preloaded server. The
`SET`/`GET` numbers on a single host should be substantially better
than vanilla once Phase 3 lands the libsd data path; today they're
about the same as vanilla, since libsd is in passthrough mode for
unknown libc surfaces.

## What this demonstrates

The point isn't a benchmark beat — it's that "I can take an unmodified
network application and slot SocksDirect underneath it" works. If
redis crashes under preload, that's an issue worth filing.
