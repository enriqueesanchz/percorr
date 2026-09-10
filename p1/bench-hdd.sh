#!/usr/bin/env bash
# Run fio sequential read benchmarks against the rotational HDD, varying
# block size, queue depth, and jobs. Results are saved as JSON files under
# ./results/fio_results_hdd/
#
# Combination matrix is deliberately trimmed vs bench.sh (the SSD sweep):
# /sys/block/sda/queue/nr_requests is 2 on this USB-attached HDD, meaning
# the block layer itself won't queue more than a couple of requests, so
# iodepths above ~4 (and the extra numjobs values) can't reveal anything
# beyond what queue depth 1-4 already shows -- they'd just cost hours for
# no new information.

set -euo pipefail

FILENAME="${1:-/media/enrique/Archives/input_data.out}"
OUTDIR="$(pwd)/results/fio_results_hdd"
mkdir -p "$OUTDIR"

BLOCK_SIZES=(4k 64k 256k 1m 4m)
QDEPTHS=(1 2 4)
JOBS=(1 2)

total=$(( ${#BLOCK_SIZES[@]} * ${#QDEPTHS[@]} * ${#JOBS[@]} ))
done=0

for nj in "${JOBS[@]}"; do
  for bs in "${BLOCK_SIZES[@]}"; do
    for qd in "${QDEPTHS[@]}"; do
      outfile="$OUTDIR/bs${bs}_qd${qd}_nj${nj}.json"
      if [[ -f "$outfile" ]]; then
        echo "Skipping (already exists): $outfile"
        done=$(( done + 1 ))
        continue
      fi
      echo "[$(( done + 1 ))/$total] bs=$bs iodepth=$qd numjobs=$nj ..."
      fio \
        --name=seqread \
        --filename="$FILENAME" \
        --rw=read \
        --bs="$bs" \
        --iodepth="$qd" \
        --numjobs="$nj" \
        --ioengine=libaio \
        --direct=1 \
        --group_reporting \
        --readonly \
        --runtime=20 \
        --time_based \
        --output-format=json \
        --output="$outfile"
      sleep 10
      done=$(( done + 1 ))
    done
    sleep 15
  done
done

echo "Done. Results in $OUTDIR/"
