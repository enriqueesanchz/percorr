#!/usr/bin/env bash
# Compares find-number.out vs find-number-hdd.out with the page cache
# genuinely dropped before every single run. Needs root (for drop_caches),
# so run it yourself: sudo will prompt for your password.
#
# Usage: ./compare-cold.sh <file> <needle> [trials]

set -euo pipefail

FILE="${1:?usage: $0 <file> <needle> [trials]}"
NEEDLE="${2:?usage: $0 <file> <needle> [trials]}"
TRIALS="${3:-3}"

drop_caches() {
    sync
    sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
}

run_cold() {
    local bin="$1"
    drop_caches
    /usr/bin/time -f "  %e s real, %P cpu" "$bin" "$FILE" "$NEEDLE" 2>&1 | tail -1
}

echo "File: $FILE"
echo "Needle: $NEEDLE"
echo "Trials per binary: $TRIALS"
echo

echo "=== find-number.out (seek-per-thread, static split) -- cold each run ==="
for i in $(seq 1 "$TRIALS"); do
    echo "run $i:"
    run_cold ./find-number.out
done

echo
echo "=== find-number-hdd.out (single sequential reader + worker pool) -- cold each run ==="
for i in $(seq 1 "$TRIALS"); do
    echo "run $i:"
    run_cold ./find-number-hdd.out
done
