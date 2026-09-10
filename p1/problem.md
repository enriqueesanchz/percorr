# Problem: finding an int in a very large file, as fast as this hardware allows

## Statement

Given a large binary file containing an unsorted array of integers that
doesn't fit into memory, find a specified value.

Use a file of raw 32-bit ints, printing `FOUND: <value>` for every occurrence.

The mentioned reference file is `input_data.out`, 32GB, `2^33` ints,
`buf[i] = (int)i` (every value appears twice). Unsorted and unindexed means no
early exit — the whole file is scanned every run, so the only lever is *how
fast the file can be read and scanned*.

Target machine: laptop, single client-grade NVMe SSD, 16GB RAM (file can't fit
in page cache), 8 cores (i7-8550U, AVX2-capable).

**Hardware ceiling** (from an `fio` sweep across block size/iodepth/numjobs):
best measured combo `bs=256k qd=32 nj=1` → 3434 MB/s, implying a theoretical
floor of **~10.0s** for this file. A single queue/job saturates this drive;
more concurrent jobs generally hurt throughput.

## Implementations

### `find-number.c` — threads + `O_DIRECT`
8 threads, static file split, `O_DIRECT` + `pread` into 4KB-aligned buffers,
inline scan. Evolved from an original naive buffered-`fread` version by
tuning thread count, buffer size, and adding an `fadvise` hint. Also carries
the SIMD scan dispatch (see below).

### `find-number-hdd.c` — HDD-tuned contrast
One sequential reader thread feeding a bounded queue, consumed by a small
worker pool. Correct design for a spinning disk (avoids seek penalties), but
included deliberately as a mismatched-hardware baseline, not a real
candidate — it throws away the parallelism this SSD can use.

### `find-number-pipelined.c` — decoupled reader/scanner
Same `O_DIRECT`/`pread` I/O as `find-number.c`, but reading and scanning are
split: 8 reader threads only `pread` into a pre-allocated, recycled buffer
pool, while 1 dedicated thread scans completed buffers, so no reader ever
blocks waiting on a scan.

### `find-number-uring.c` — `io_uring`
Real async I/O: one `O_DIRECT` fd, one ring, one submitter thread keeping a
steady `QUEUE_DEPTH` of reads in flight, feeding the same decoupled-scanner
pattern as the pipelined version. `QUEUE_DEPTH=4` (matching `fio`'s own best
config) was swept empirically and is the clear winner. Also carries the SIMD
scan dispatch.

### `find-number-libaio.c` — older Linux AIO
Structurally identical to the `io_uring` version (same ring/scanner pattern),
but through the older `io_setup`/`io_submit`/`io_getevents` interface — the
same engine `fio` itself used for its benchmarks.

### SIMD scanning (in `find-number.c` and `find-number-uring.c`)
Scalar/SSE2/AVX2 scan variants, chosen once at startup via
`__builtin_cpu_supports`. Doesn't change wall-clock time on this machine
(scanning is already hidden behind I/O wait) but keeps the scan from becoming
the bottleneck on faster storage or other CPUs.

## Results: rigorous cold-cache comparison

3 trials each, page cache dropped before every run, interleaved round-robin,
plugged into AC power.

| binary | mean (s) | throughput | % of `fio` ceiling (3434 MB/s) |
|---|---|---|---|
| `find-number-hdd.out` | 25.58 | ~1343 MB/s | ~39% |
| `find-number-pipelined.out` | 11.38 | ~3019 MB/s | ~88% |
| `find-number-libaio.out` | 11.11 | ~3093 MB/s | ~90% |
| **`find-number.out`** | **11.04** | **~3113 MB/s** | **~91%** |
| **`find-number-uring.out`** | **10.23** | **~3360 MB/s** | **~98%** |

## Conclusions

- **`io_uring` wins outright**, within a few percent of the drive's own `fio`-measured ceiling.
- **Architecture must match the storage medium**: the HDD-tuned design is correct for its target but worst here (~39%) — proof a well-reasoned design for the wrong hardware is still wrong.
- **`fio`'s concurrency knobs don't transfer literally to threads**: `numjobs` assumes each submitter already has its own queue depth; blocking `pread`/`fread` threads don't, so more OS threads helped here even though `fio` favored fewer jobs. Block size and total bytes-in-flight *did* transfer directly (1MB chunks, `QUEUE_DEPTH=4`).
- **Diminishing returns**: naive (~25-27s) → `io_uring` (~10.23s) is 2.5x; the last step (91% → 98% of ceiling) bought less than a second. At 98% of an independently-measured hardware ceiling, very little is left for software to claim.
- **SIMD scanning is a bet on other hardware**: no wall-clock effect here (scan is fully hidden behind I/O), but protects against an unvectorized scan becoming the bottleneck on faster drives or other machines.

```

➜  p1 git:(main) ✗ sudo ./compare.sh input_data.out 8589934591 3
[sudo] password for enrique: 
File: input_data.out
Needle: 8589934591
Trials per binary: 3 (interleaved round-robin, cold cache each run)

--- round 1/3 ---
  find-number.out                 10.87 s real,    62% cpu
  find-number-hdd.out             24.15 s real,   107% cpu
  find-number-pipelined.out       11.42 s real,   158% cpu
  find-number-uring.out           10.20 s real,    87% cpu
  find-number-libaio.out          11.07 s real,   162% cpu
--- round 2/3 ---
  find-number.out                 10.84 s real,    78% cpu
  find-number-hdd.out             25.70 s real,   110% cpu
  find-number-pipelined.out       11.34 s real,   157% cpu
  find-number-uring.out           10.31 s real,    97% cpu
  find-number-libaio.out          11.07 s real,   162% cpu
--- round 3/3 ---
  find-number.out                 10.89 s real,    79% cpu
  find-number-hdd.out             25.51 s real,   110% cpu
  find-number-pipelined.out       11.44 s real,   162% cpu
  find-number-uring.out           10.27 s real,    98% cpu
  find-number-libaio.out          11.12 s real,   162% cpu

=== summary (cold cache, 3 trials each) ===
binary                          mean(s)     min(s)     max(s)  description
find-number.out                   10.87      10.84      10.89  O_DIRECT, static per-thread split, 8 threads
find-number-hdd.out               25.12      24.15      25.70  single sequential reader + worker pool (HDD-tuned)
find-number-pipelined.out         11.40      11.34      11.44  O_DIRECT, decoupled reader/scanner buffer pool
find-number-uring.out             10.26      10.20      10.31  io_uring, single submitter, QUEUE_DEPTH=4
find-number-libaio.out            11.09      11.07      11.12  libaio, single submitter, QUEUE_DEPTH=4
```

## Results: rotational disk (HDD) comparison

Same file layout (32GB, `buf[i] = (int)i`), same needle (`8589934591`), same
`compare.sh` methodology (3 trials, interleaved round-robin, cold cache
dropped before every run), but this time the target is a spinning disk:

**Disk:** TOSHIBA MQ01UBD100, 2.5" 5400 RPM, behind a USB 3.0 external
enclosure, formatted exFAT, mounted at `/media/enrique/Archives`. `O_DIRECT`
was verified to work on this mount before running the comparison. Note this
is "HDD behind USB 3.0", not a bare SATA connection — the enclosure/bridge
chip and exFAT driver are additional variables versus the SSD's NVMe/ext4
setup, though at 5400 RPM the mechanical drive itself, not USB3's own
throughput ceiling, is clearly the bottleneck (see below).

### Hardware ceiling (trimmed `fio` sweep)

`/sys/block/sda/queue/nr_requests` is **2** on this USB-attached drive — the
block layer itself won't queue more than a couple of requests, so unlike the
SSD's 126-combination sweep (`bench.sh`, block size × iodepth 1-256 ×
numjobs 1/2/4), a smaller, targeted sweep (`bench-hdd.sh`) was run instead:
block sizes 4k/64k/256k/1m/4m × iodepth 1/2/4 × numjobs 1/2 (30 combinations,
~20 min total) — depths beyond 4 or extra jobs can't reveal anything new
when the device queue itself caps at 2.

| best configs | throughput |
|---|---|
| `bs=1m qd=2 nj=1` | 41.94 MiB/s |
| `bs=4m qd=4 nj=1` | 41.81 MiB/s |
| `bs=4m qd=2 nj=1` | 41.72 MiB/s |
| `bs=256k qd=2 nj=1` | 40.74 MiB/s |

| worst configs | throughput |
|---|---|
| `bs=4k qd=1 nj=1` | 9.90 MiB/s |
| `bs=4k qd=1 nj=2` | 12.36 MiB/s |
| `bs=4k qd=2 nj=1` | 12.67 MiB/s |

Ceiling is **~42 MiB/s**, reached at large block sizes (≥256k), `nj=1`, with
iodepth beyond 1-2 buying almost nothing — consistent with `nr_requests=2`.
This implies a theoretical floor of **~800s (32768 MiB / 42 MiB/s)** for the
32GB file, close to what the winning binaries (`find-number-hdd.out`,
`io_uring`, `libaio`) achieved below (~871-879s, **~89-90% of this ceiling**
— see the exact per-binary breakdown in the results table below).
Small 4k blocks collapse throughput to ~10-20 MiB/s — nearly 3-4x worse —
confirming this drive is seek/rotational-latency bound, not bandwidth bound,
below a few hundred KB per request. Plots are in `results/fio_plots_hdd/`.

**Confirming the ceiling is real, not a sweep artifact:**

- *Plateau check*: throughput was still flat/rising at `bs=4m` (the largest
  size in the 30-combo sweep), so two more points were added — `bs=8m` and
  `bs=16m` at `qd=1/2, nj=1` — to rule out a higher peak beyond the tested
  range. Result: 41.03-41.91 MiB/s, indistinguishable from `1m`/`4m`. The
  ~42 MiB/s ceiling is a genuine plateau, not a truncated sweep.
- *I/O-mode check*: the sweep above used `--direct=1` throughout, but the
  actual winning binary, `find-number-hdd.out`, uses **buffered** `fread` +
  `posix_fadvise(SEQUENTIAL)`, not `O_DIRECT` — a potential mismatch between
  what was measured and what was raced. A follow-up buffered sweep
  (`--direct=0`, `ioengine=sync`, `bs=64k/256k/1m/4m`, `qd=1, nj=1`) was run
  for comparison: 40.80-41.74 MiB/s — statistically the same as the
  `O_DIRECT` numbers. For a 32GB single-pass sequential scan (file far
  exceeds the 15GB of RAM, so no cache reuse), kernel readahead can't beat
  what direct sequential reads already extract from the drive: the disk
  itself is the bottleneck either way, so the `O_DIRECT` ceiling is a valid
  stand-in for the buffered binary's real ceiling.

| binary | mean (s) | min (s) | max (s) | throughput | % of `fio` ceiling (41.94 MiB/s) | description |
|---|---|---|---|---|---|---|
| **`find-number-hdd.out`** | **871.48** | 870.42 | 872.34 | **~37.6 MiB/s** | **~89.7%** | single sequential reader + worker pool (HDD-tuned) |
| `find-number-libaio.out` | 875.75 | 866.58 | 882.89 | ~37.4 MiB/s | ~89.2% | libaio, single submitter, QUEUE_DEPTH=4 |
| `find-number-uring.out` | 879.20 | 877.47 | 880.79 | ~37.3 MiB/s | ~88.9% | io_uring, single submitter, QUEUE_DEPTH=4 |
| `find-number-pipelined.out` | 1251.50 | 1246.16 | 1254.88 | ~26.2 MiB/s | ~62.4% | O_DIRECT, decoupled reader/scanner buffer pool |
| `find-number.out` | 1260.06 | 1251.65 | 1264.66 | ~26.0 MiB/s | ~62.0% | O_DIRECT, static per-thread split, 8 threads |

(CPU utilization on every binary dropped to single digits — 3-14% — versus
62-162% on the SSD, confirming the workload is now almost entirely seek/
rotational-latency bound rather than throughput bound.)

### Conclusions

- **The ranking inverts.** `find-number-hdd.out` — the design that was
  *worst* on the SSD (~39% of that drive's ceiling) — is now the **fastest**
  design here, exactly as its "HDD-tuned" name promised: a single sequential
  reader avoids the seek thrashing that competing concurrent readers cause on
  a spinning platter.
- **`io_uring`/`libaio` are close behind, not far ahead.** Both single-
  submitter, bounded-queue-depth designs land within ~1% of the HDD-tuned
  design (875-879s vs 871s). A modest, bounded number of in-flight requests
  still lets the drive's own I/O scheduler (or the elevator/merge logic
  underneath) reorder and coalesce nearby requests, which approximates the
  single-sequential-stream behavior closely enough not to matter much.
- **Static 8-way file splitting is the clear loser on rotational media.**
  `find-number.out` and `find-number-pipelined.out` both issue reads from 8
  independent, widely separated file offsets concurrently. On the SSD this
  was fine (nothing to seek); on a spinning disk it forces the head to jump
  between 8 far-apart regions continuously, and both designs land at
  **~1.45x the time** of the HDD-tuned/io_uring/libaio cluster (1251-1260s
  vs ~871-879s) — a ~44% throughput penalty from concurrency alone.
- **This reproduces `problem.md`'s own thesis in reverse.** The SSD section
  concluded "architecture must match the storage medium" using the HDD-tuned
  design as the cautionary counter-example. Running the same five binaries
  against an actual HDD confirms the converse: the SSD-optimized concurrent/
  `O_DIRECT`-per-thread designs are the ones penalized here, while the design
  built around avoiding seeks is now correct for its medium and wins.
- **Absolute numbers, for scale:** every binary here takes roughly 35-50x
  longer than its SSD counterpart on the same 32GB file (e.g. `io_uring`:
  10.26s → 879.20s), driven by mechanical seek latency rather than the USB3
  link (5400 RPM implies ~11ms average seek + rotational latency per
  non-sequential access, which dominates once reads are split across
  multiple concurrent offsets).

