#!/usr/bin/env bash
# build-deb.sh — local .deb build via Docker. Output lands in ./dist/.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
mkdir -p "$REPO/dist"
docker buildx build \
    --output "type=local,dest=$REPO/dist" \
    -f "$HERE/Dockerfile.deb-builder" \
    "$REPO"
ls -1 "$REPO/dist/out"/*.deb 2>/dev/null || {
    echo "no .deb produced; check the build log above" >&2
    exit 1
}
echo "wrote:"
ls -1 "$REPO/dist/out"/*.deb
