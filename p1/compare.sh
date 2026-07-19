#!/usr/bin/env bash
# Compares every find-number implementation with the page cache genuinely
# dropped before every single run. Needs root (for drop_caches), so run it
# yourself: sudo will prompt for your password.
#
# Trials are interleaved round-robin across binaries (not grouped one binary
# at a time), so any drift over the course of the run -- thermal throttling,
# a laptop switching from battery to AC or back, background load -- hits
# every binary about equally instead of biasing whichever one happens to run
# last. (Battery vs. AC power alone was observed to skew results by ~3x
# during this project's own benchmarking, hence the check below.)
#
# Usage: ./compare.sh <file> <needle> [trials]

set -euo pipefail

FILE="${1:?usage: $0 <file> <needle> [trials]}"
NEEDLE="${2:?usage: $0 <file> <needle> [trials]}"
TRIALS="${3:-3}"

BINARIES=(
    find-number.out
    find-number-hdd.out
    find-number-pipelined.out
    find-number-uring.out
    find-number-libaio.out
)

declare -A DESC=(
    [find-number.out]="O_DIRECT, static per-thread split, 8 threads"
    [find-number-hdd.out]="single sequential reader + worker pool (HDD-tuned)"
    [find-number-pipelined.out]="O_DIRECT, decoupled reader/scanner buffer pool"
    [find-number-uring.out]="io_uring, single submitter, QUEUE_DEPTH=4"
    [find-number-libaio.out]="libaio, single submitter, QUEUE_DEPTH=4"
)

for bin in "${BINARIES[@]}"; do
    if [[ ! -x "./$bin" ]]; then
        echo "Missing or non-executable: ./$bin -- build it first." >&2
        exit 1
    fi
done

check_power() {
    local status
    status=$(cat /sys/class/power_supply/*/status 2>/dev/null | head -1)
    status="${status:-unknown}"
    if [[ "$status" != "Charging" && "$status" != "Full" ]]; then
        echo "WARNING: power status is '$status', not Charging/Full."
        echo "Running on battery visibly throttled results by ~3x earlier in"
        echo "this project's benchmarking -- plug in before trusting these numbers."
        read -rp "Continue anyway? [y/N] " reply
        [[ "$reply" =~ ^[Yy]$ ]] || exit 1
    fi
}

drop_caches() {
    sync
    sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
}

check_power

LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

echo "File: $FILE"
echo "Needle: $NEEDLE"
echo "Trials per binary: $TRIALS (interleaved round-robin, cold cache each run)"
echo

for round in $(seq 1 "$TRIALS"); do
    echo "--- round $round/$TRIALS ---"
    for bin in "${BINARIES[@]}"; do
        drop_caches
        timing=$(/usr/bin/time -f "%e %P" "./$bin" "$FILE" "$NEEDLE" 2>&1 | tail -1)
        secs=$(awk '{print $1}' <<< "$timing")
        cpu=$(awk '{print $2}' <<< "$timing")
        printf "  %-28s %8s s real, %6s cpu\n" "$bin" "$secs" "$cpu"
        echo "$bin $secs" >> "$LOG"
    done
done

echo
echo "=== summary (cold cache, $TRIALS trials each) ==="
printf "%-28s %10s %10s %10s  %s\n" "binary" "mean(s)" "min(s)" "max(s)" "description"
for bin in "${BINARIES[@]}"; do
    awk -v bin="$bin" -v desc="${DESC[$bin]}" '
        $1 == bin {
            n++; sum += $2
            if (n == 1 || $2 < min) min = $2
            if (n == 1 || $2 > max) max = $2
        }
        END { if (n > 0) printf "%-28s %10.2f %10.2f %10.2f  %s\n", bin, sum/n, min, max, desc }
    ' "$LOG"
done
