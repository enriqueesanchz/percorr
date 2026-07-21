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
