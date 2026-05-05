# nginx — preloaded nginx serving rate

Boots a single-worker nginx with libsd preloaded, fires N curl
requests at it, reports requests/sec.

Tier 2 (intra-host); Tier 3 to compare against the paper's
geo-distributed numbers.

Override request count: `N=10000 ./reproduce/repro nginx`.
Default port 58080; override with `PORT=...`.
