#!/usr/bin/env bash
# Run fio sequential read benchmarks varying block size, queue depth, and jobs.
# Results are saved as JSON files under ./fio_results/

set -euo pipefail

FILENAME="$(pwd)/input_data.txt"
OUTDIR="$(pwd)/results/fio_results"
mkdir -p "$OUTDIR"

BLOCK_SIZES=(4k 16k 64k 256k 1m 4m)
QDEPTHS=(1 4 16 32 64 128 256)
JOBS=(1 2 4)

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
        --runtime=30 \
        --time_based \
        --output-format=json \
        --output="$outfile"
      sleep 10
      done=$(( done + 1 ))
      sleep 20
    done
    sleep 30
  done
done

echo "Done. Results in $OUTDIR/"
