# p1 — finding an int in a very large file

Given a large binary file holding an unsorted array of 32-bit ints that doesn't
fit in memory, find every occurrence of a value as fast as the hardware allows.
Unsorted and unindexed means no early exit: the whole file is scanned every run,
so the only lever is how fast it can be read and scanned.

Five C implementations (threads + `O_DIRECT`, an HDD-tuned producer/consumer, a
decoupled reader/scanner pipeline, `io_uring` and `libaio`), plus an `fio` sweep
that establishes the drive's ceiling and a benchmark harness that compares them
with a cold page cache.

The full write-up — designs, measured results and conclusions — is in
[`problem.md`](problem.md).

## Requirements

Linux (all implementations use `O_DIRECT`, `posix_fadvise`, `io_uring` or `libaio`).

```bash
sudo apt install build-essential fio liburing-dev libaio-dev
```

Optional, only for re-plotting the `fio` sweep: Python 3 with `matplotlib`,
`pandas`, `numpy`, `seaborn`.

## Build

There is no Makefile; each program is a single translation unit.

```bash
gcc -O2 -o create-input-data.out      create-input-data.c
gcc -O2 -o read-input-data.out        read-input-data.c

gcc -O2 -pthread -o find-number.out            find-number.c
gcc -O2 -pthread -o find-number-hdd.out        find-number-hdd.c
gcc -O2 -pthread -o find-number-pipelined.out  find-number-pipelined.c
gcc -O2 -pthread -o find-number-uring.out      find-number-uring.c   -luring
gcc -O2 -pthread -o find-number-libaio.out     find-number-libaio.c  -laio
```

`find-number.c` and `find-number-uring.c` contain scalar/SSE2/AVX2 scan
variants selected at runtime via `__builtin_cpu_supports`, so no `-march`
flag is needed or wanted.

## Generate the input file

```bash
./create-input-data.out input_data.out 32768   # <filename> <size_in_MB>
```

This writes raw 32-bit ints with `buf[i] = (int)i`. On a 32 GB file the counter
wraps past `INT_MAX`, so every representable value appears exactly twice — a
correct search prints two `FOUND:` lines.

Pick a size **larger than your RAM**, otherwise the file sits in the page cache
and you are benchmarking memory, not the disk.

`read-input-data.out <filename>` is a small sanity checker that reads the file
back sequentially.

## Search for a value

Every implementation takes the same arguments and prints `FOUND: <value>` once
per occurrence:

```bash
./find-number-uring.out input_data.out 123456789
```

| binary | design |
|---|---|
| `find-number.out` | 8 threads, static file split, `O_DIRECT` + `pread`, inline SIMD scan |
| `find-number-hdd.out` | one sequential reader feeding a bounded queue + worker pool (HDD-tuned; deliberately mismatched here) |
| `find-number-pipelined.out` | 8 `O_DIRECT` readers into a recycled buffer pool, 1 dedicated scanner thread |
| `find-number-uring.out` | `io_uring`, single submitter, `QUEUE_DEPTH=4` — fastest here (~98% of the drive's ceiling) |
| `find-number-libaio.out` | same structure via the older `io_setup`/`io_submit` AIO interface |

Tunables (`NTHREADS`, `BUFFER_INTS`, `QUEUE_DEPTH`, …) are `#define`s at the top
of each file; change and rebuild.

## Compare implementations

`compare.sh` runs every binary round-robin, dropping the page cache before each
individual run so no binary is measured warm, and prints mean/min/max:

```bash
sudo ./compare.sh input_data.out 123456789 3   # <file> <needle> [trials]
```

Needs root for `/proc/sys/vm/drop_caches`. It warns if the machine is on
battery — power state alone was observed to skew results by ~3x during this
project's benchmarking, so plug in before trusting any numbers.

## Measure the hardware ceiling

`bench.sh` sweeps `fio` over block size × iodepth × numjobs (126 combinations,
30 s each plus cooldowns — expect a few hours) and writes JSON to
`results/fio_results/`. Existing result files are skipped, so the sweep is
resumable.

```bash
./bench.sh
```

> Note: `bench.sh` targets `input_data.txt`. Edit `FILENAME` to point at the file
> you actually generated (e.g. `input_data.out`). It runs `fio --readonly`, so it
> only reads the file.

Then render the plots into `results/fio_plots/`:

```bash
python3 -m venv venv
./venv/bin/pip install matplotlib pandas numpy seaborn
./venv/bin/python plot_results.py
```

This produces throughput-vs-blocksize / iodepth / numjobs line plots, three
heatmaps, and a top-20 configuration bar chart. On the reference machine the
best combo was `bs=256k qd=32 nj=1` → 3434 MB/s, implying a ~10.0 s floor for a
32 GB scan; `find-number-uring.out` reaches 10.23 s.

`plot_results.py` accepts optional `<results_dir> <plots_dir>` args (default
`results/fio_results` / `results/fio_plots`) so a differently-located sweep
(e.g. a rotational disk's, see below) can be plotted without overwriting the
originals.

### Rotational disk (HDD)

`bench-hdd.sh` is a trimmed variant for spinning/USB-attached disks: many
such drives cap their block-layer queue at `nr_requests=2`
(`/sys/block/<dev>/queue/nr_requests`), so sweeping iodepth up to 256 and
numjobs up to 4 like `bench.sh` does just burns hours for no new signal.
It sweeps block size × iodepth 1/2/4 × numjobs 1/2 (30 combinations, ~15-20
min) against a file path passed as its first argument:

```bash
./bench-hdd.sh /path/to/input_data.out
./venv/bin/python plot_results.py results/fio_results_hdd results/fio_plots_hdd
```

See `problem.md` for a full HDD write-up and comparison against the SSD
results.

## Repository layout

```
problem.md              write-up: statement, designs, results, conclusions
create-input-data.c     generate the test file
read-input-data.c       sequential read sanity check
find-number*.c          the five search implementations
compare.sh              cold-cache round-robin benchmark harness
bench.sh                fio sweep over bs x iodepth x numjobs (SSD)
bench-hdd.sh            trimmed fio sweep for rotational/USB disks
plot_results.py         turn fio JSON into plots (accepts results/plots dir args)
results/                fio_results/ + fio_plots/ (SSD), fio_results_hdd/ + fio_plots_hdd/ (HDD)
```

Build artifacts (`*.out`), `results/` and `venv/` are gitignored — note that
this also covers the generated `input_data.out`.
