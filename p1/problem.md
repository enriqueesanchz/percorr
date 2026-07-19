# Problem: finding an int in a very large file, as fast as this hardware allows

## Statement

Given a large binary file of raw 32-bit ints and a target value (the "needle"),
print `FOUND: <value>` for every occurrence in the file. The reference file used
throughout this project is 32GB (`input_data.out`, `2^33` ints, `buf[i] = (int)i`,
so every 32-bit value appears in it exactly twice). The implementation must scan
the entire file every run — there's no index, no sorting, no early exit — so the
only lever available is *how fast the file can be read and scanned*.

The target machine: a laptop with a single client-grade NVMe SSD, 16GB of RAM (so
the 32GB file can never fully sit in page cache), and 8 CPU cores. Because the
file doesn't fit in RAM, every run does real disk I/O regardless of whether the
page cache was explicitly dropped — but only a rigorously cold-cache benchmark
(see Results) is trustworthy for comparing implementations against each other.

The guiding method throughout: don't guess at what should be fast — measure the
raw hardware first with `fio` (`results/fio_results/`, 126 runs sweeping block
size × iodepth × numjobs), then measure the actual program, and let discrepancies
between the two drive the next design change.

## Hardware ceiling (from `fio`)

Sweeping `--rw=read --direct=1 --ioengine=libaio` across block sizes (4k–4m),
iodepths (1–256), and numjobs (1/2/4) on this exact drive:

- **Best measured combo:** `bs=256k qd=32 nj=1` → 3434 MB/s
- **Close second:** `bs=1m qd=4 nj=1` → 3428 MB/s
- **A single queue/job is enough to saturate this drive** — sorting all 126 runs
  by bandwidth, 9 of the top 10 are `nj=1`; more concurrent jobs generally hurt
  the average (nj=1 avg 1820 MB/s vs nj=4 avg 798 MB/s across the whole sweep).
- Sweet spot block size is 256KB–1MB; 4KB is uniformly bad (~530 MB/s average),
  and depths above ~64 start regressing even for nj=1.
- For this exact file, 3434 MB/s implies a theoretical floor of **~10.0s**.

This became the number every implementation was measured against.

## Approaches tried

### 1. Naive multi-thread, buffered I/O (`find-number.c`, original)
Four threads, each `fseeko` + buffered `fread` into a 64MB buffer over a static
slice of the file, scanning inline. No `fio`-informed tuning at all — just "more
threads should help." ~25-27s in early testing.

### 2. Tuning the naive design
Three small, separately-verified changes: a `posix_fadvise(SEQUENTIAL)` hint per
thread (borrowed from the HDD-tuned design below), shrinking the buffer from
64MB to 1MB (`fio`'s best-measured block size, whereas 64MB was never actually
tested), and — after a wrong first guess — raising the thread count from 4 to 8.

The thread-count change is the interesting one: `fio`'s own `numjobs` sweep says
a single job beats four (see ceiling section above), so the first attempt cut
`NTHREADS` to 2. Direct A/B testing on the real file showed the opposite: 1
thread timed out (>2 min), 2 threads ~37s, 4 threads ~16s, 8 threads ~13s, 16
threads ~12.4s. **`fio`'s `numjobs` and this program's OS threads are not the
same kind of concurrency** — `numjobs` describes independent async submitters
each already using an explicit `iodepth`, while a buffered `fread()` thread gets
only the kernel's implicit per-stream readahead, which is far below what
saturates this drive on its own. More threads was how *this* code reached
adequate concurrency, not less. Landed on 8 threads. ~13.0-13.4s.

### 3. `O_DIRECT` + `pread` (`find-number.c`, current)
Same 8-thread static split, but bypassing the page cache entirely with
`O_RDONLY | O_DIRECT` and `pread` into 4096-byte-aligned buffers. This surfaced
a real correctness bug worth recording: **`O_DIRECT` requires the *requested*
read length to be alignment-sized, even for the final short read at true EOF** —
the kernel is only allowed to return fewer bytes than requested, not accept an
unaligned request length. Fixed by rounding the request up and trusting the
returned byte count. Reduced CPU usage substantially (page-cache double-copy
avoided) and gave a modest wall-clock win. ~12.2-12.9s in development.

### 4. Single sequential reader tuned for spinning disks (`find-number-hdd.c`)
Included deliberately as a mismatched-hardware contrast, not as a candidate to
win: one producer thread reads the whole file strictly sequentially (the only
access pattern that doesn't cost a rotational disk a seek penalty) into a
bounded queue, with a small pool of consumer threads scanning the completed
chunks. Correct design *for a spinning disk* — but on this SSD it throws away
all the parallelism the drive can actually use.

### 5. Decoupling I/O from scanning (`find-number-pipelined.c`)
Motivated by measuring the O_DIRECT version with its scan loop stubbed out
entirely: pure I/O took ~11.3s vs ~13.4s with the inline scan — a real ~15-18%
gap, bigger than expected for a cheap equality check over a 1MB buffer. Built a
producer/consumer pipeline: 8 reader threads do nothing but `pread` into a fixed
pool of pre-allocated aligned buffers (recycled through two bounded ring
buffers), and one dedicated scanner thread drains them, so a reader is never
blocked waiting for a scan to finish.

The first attempt at this (allocating a fresh aligned buffer per 1MB chunk) was
*slower* than the plain O_DIRECT version (~25s), because glibc routes ≥128KB
allocations through individual `mmap`/`munmap`, and doing that ~32,000 times
across many threads causes serious kernel-side contention. Switching to a
pre-allocated, recycled buffer pool fixed this and reached ~11.2-11.4s in
development — matching the pure-I/O floor almost exactly.

### 6. Real async I/O: `io_uring` (`find-number-uring.c`)
All prior versions approximate concurrency with blocking OS threads. `fio`'s own
numbers came from genuine kernel-managed queue depth (`--ioengine=libaio`), and
its sweep shows a *single* queue is enough to saturate this drive — so this
version drops the per-thread file split entirely. One `O_DIRECT` fd, one
`io_uring` ring, one submitter thread maintaining a steady-state window of
exactly `QUEUE_DEPTH` reads in flight (one blocking wait per batch, drain
everything else already-ready non-blockingly, refill, submit once per batch),
feeding the same decoupled-scanner-thread pattern as the pipelined version.

`QUEUE_DEPTH` was swept empirically rather than assumed: 2 → ~13.8s, **4 → best**,
8 → ~11.5-12.3s, 16 → ~12.7-13.6s, 32 → **~29-34s, catastrophic**. At 1MB chunks,
`qd=32` means 32MB simultaneously in flight — far more than `fio`'s own best
`qd=32` result used (`bs=256k`, only 8MB in flight) — and this specific
client-grade drive falls over under that much queued data. `QUEUE_DEPTH=4`
matches `fio`'s `bs=1m qd=4 nj=1` almost exactly and was the clear winner,
~10.8-10.9s in development.

One aside worth recording: a throwaway `fio --ioengine=io_uring` run at the same
`bs=1m qd=4` config measured only 3222 MB/s on this machine — below `fio`'s own
`libaio`-engine result (3428 MB/s) for the identical parameters. The two async
engines don't perform identically here, which matters when judging "how close
to the ceiling" a given implementation actually is.

### 7. Real async I/O: the older Linux AIO interface (`find-number-libaio.c`)
Structurally identical to the `io_uring` version (same ring/scanner code reused
verbatim, same single-submitter/steady-state-queue-depth loop), but through
`io_setup`/`io_submit`/`io_getevents`/`io_destroy` — literally the engine `fio`
itself used for every benchmark in `results/fio_results`. The completion loop is
simpler than `io_uring`'s: `io_getevents(ctx, 1, QUEUE_DEPTH, events, NULL)`
blocks until at least one completion is ready but hands back the whole ready
batch in one call, no separate wait-then-peek step needed.

Swept the same `QUEUE_DEPTH` values with the same pattern: `qd=4` best,
`qd=32` catastrophic (~28s) — confirming the "too much data in flight overwhelms
this drive" finding is about the drive, not the async API. In development,
`libaio` trailed the `io_uring` version by a few percent with more run-to-run
variance, despite `fio`'s own numbers suggesting `libaio` should be the faster
engine on this machine — an observed but not fully diagnosed gap.

### 8. SIMD-accelerated scanning, dispatched per-CPU (`find-number.c`, `find-number-uring.c`)
Not motivated by this laptop — scanning is already fully hidden behind I/O wait
here (established back in approach 5's inline-vs-decoupled-scan A/B test, and
reconfirmed by `io_uring` reaching ~98% of the hardware ceiling with a plain
scalar scanner thread). Motivated by portability: this code runs on other
laptops too, and on storage meaningfully faster than this drive's ~3.4 GB/s
ceiling, an unvectorized scan could become the new bottleneck the way it
briefly did before the decoupled-scanner architecture hid it.

**First cut: SSE2.** Compares 4 ints per instruction (`_mm_cmpeq_epi32` +
`_mm_movemask_epi8` to detect any match in the 128-bit lane, falling back to a
small scalar cleanup only on an actual match, plus a scalar tail loop for the
remainder). Chosen specifically because SSE2 is part of the mandatory x86-64
ABI baseline — every x86-64 CPU has it, so this needed no runtime feature
detection and no special compile flags (confirmed `gcc` predefines `__SSE2__`
on this target). Guarded by `#if defined(__SSE2__)` with a scalar fallback for
non-x86 builds.

**Second cut: per-CPU runtime dispatch.** SSE2 turned out not to be the best
*this* laptop can do — an i7-8550U, which supports AVX2 (8 ints/instruction).
First attempted GCC's function-multiversioning-by-redefinition (repeating
`scan_buffer` under different `target("...")` attributes and letting GCC
generate the dispatch automatically); a standalone repro confirmed this
specific mechanism is C++-only — GCC 13 rejects it in plain C with a
"redefinition" error, for both `static` and external-linkage functions.
Switched to a C-compatible manual dispatch instead: three separately-named
functions (`scan_buffer_avx2`, `scan_buffer_sse2`, `scan_buffer_scalar`), each
hand-written for its own vector width, resolved once via
`__builtin_cpu_supports("avx2")`/`("sse2")` in `main()` before any
threads/the scanner thread are created, and handed down as a plain function
pointer through `worker_args`/`scanner_args`. Resolving once up front, rather
than lazily on first call, avoids a real data race that a naive
lazily-initialized function pointer would have had across multiple threads.

Verification: `objdump` confirms `scan_buffer_avx2` genuinely compiles to real
256-bit AVX2 instructions (`vpcmpeqd`/`vpmovmskb %ymm...`), and
`__builtin_cpu_supports("avx2")` does return true on this CPU — so the
dispatch isn't just theoretically correct, it's the path actually exercised
here. The edge-case correctness battery was expanded with sizes chosen
specifically to hit each SIMD width's tail-loop boundary (3/5/7 ints for
SSE2's 4-wide tail, 15/16/17 ints for AVX2's 8-wide tail), on top of the
existing empty/non-multiple-of-4/alignment-chunk files — 154 checks across
both binaries, all passed, plus a full cross-check against each other on the
real 32GB file across 5 needles.

Performance: an isolated in-memory microbenchmark (scanning ~2GB of random
ints repeatedly, a volatile sink counter to block dead-code elimination)
measured scalar at 3600 MB/s, SSE2 at 25407 MB/s (7.06x), and AVX2 at 43225
MB/s (12.01x vs scalar, a further 1.70x over SSE2 alone) — all comfortably
above anything this drive can feed the scanner. End-to-end wall-clock on this
machine is unchanged either way, as expected (~11.04-11.10s / ~10.33-10.54s,
within the pre-SIMD noise band) — but `find-number.out`'s CPU usage dropped
at each step (from the ~135-200% baseline, to ~88-89% with SSE2-only, to
~79-80% with AVX2 dispatch), real evidence the scan keeps getting genuinely
cheaper even though it isn't wall-clock-limiting on this hardware.

## A confound worth naming: power state

Mid-session, a batch of `io_uring` timings suddenly and consistently jumped from
~11s to ~29s. The cause was the laptop running on battery — plugging it back in
immediately restored the original numbers. This is why `compare.sh` now checks
power state and warns before running, and why trials are interleaved
round-robin across binaries rather than grouped one binary at a time: grouped
trials would let this kind of drift land unevenly and masquerade as a real
difference between implementations.

## Results: rigorous cold-cache comparison

All five implementations, `compare.sh`, page cache genuinely dropped
(`sync; echo 3 > /proc/sys/vm/drop_caches`) before every single run, 3 trials
each, interleaved round-robin, plugged into AC power:

| binary | mean (s) | throughput | % of `fio` ceiling (3434 MB/s) |
|---|---|---|---|
| `find-number-hdd.out` | 25.58 | ~1343 MB/s | ~39% |
| `find-number-pipelined.out` | 11.38 | ~3019 MB/s | ~88% |
| `find-number-libaio.out` | 11.11 | ~3093 MB/s | ~90% |
| `find-number.out` | 11.04 | ~3113 MB/s | ~91% |
| **`find-number-uring.out`** | **10.23** | **~3360 MB/s** | **~98%** |

(Needle used: `8589934591` = `2^33 - 1`, the file's very last element — chosen to
exercise the end-of-file/alignment-clipping path specifically, not as a
performance stress case, since every implementation always scans the entire
file regardless of where a match falls.)

## Conclusions

- **`io_uring` wins outright**, and by a comfortable margin — the only
  implementation to land within a few percent of the hardware's own measured
  ceiling. Genuine kernel-managed queue depth, tuned to match what `fio` itself
  found to be this drive's sweet spot, is what actually closes the gap that
  thread-based concurrency couldn't.

- **Architecture must match the storage medium.** `find-number-hdd.c` is
  correctly designed for its target (a single sequential stream avoids seek
  penalties on a spinning disk) and is the worst performer here by a wide
  margin (~39% of ceiling) — proof that a well-reasoned design for the wrong
  hardware is still the wrong design.

- **`fio`'s concurrency knobs don't transfer literally to application threads.**
  `numjobs` in `fio` describes independent async submitters with their own
  explicit queue depth; a blocking `fread`/`pread` thread has no such depth of
  its own, so this project's own thread-count tuning went the *opposite*
  direction from what a naive reading of the `fio` sweep would suggest (more
  threads helped, not fewer). Block size and total-bytes-in-flight, on the
  other hand, transferred quite directly (1MB chunks, `QUEUE_DEPTH=4`).

- **This drive breaks under too much queued data, regardless of which API
  queues it.** Both async implementations show the same cliff at `qd=32`
  (32MB in flight) — a property of the hardware, not of `io_uring` vs `libaio`.

- **Rigorous, cold-cache, interleaved measurement changed the story.**
  Development-time numbers (gathered without root access to actually drop
  caches, relying only on the 32GB file exceeding 16GB of RAM) showed
  `find-number-pipelined.c` clearly ahead of plain `find-number.c`
  (~11.3s vs ~12.5s). Under this session's true cold-cache benchmark, that gap
  nearly vanished (11.38s vs 11.04s — pipelined is now *slightly slower*). The
  decoupled-scanner benefit measured in development was real under partially
  warm conditions but doesn't clearly hold up cold — a caution against trusting
  relative comparisons gathered under inconsistent cache (or power) state.

- **Diminishing returns have set in.** From the original naive implementation
  (~25-27s) to `io_uring` (~10.23s) is a ~2.5x improvement, and the last step
  (`find-number.c`'s already-tuned 91% → `io_uring`'s 98%) bought less than a
  second. At 98% of a hardware ceiling measured independently by `fio`, there
  is very little left for software to claim — the drive itself is now the
  limit.

- **Optimizing a component that isn't the bottleneck can still be worth it —
  for different hardware.** The SIMD work is the clearest example: it changes
  nothing about wall-clock time on this machine (scanning was already fully
  hidden behind I/O), but this code runs on other laptops too, and on any
  drive meaningfully faster than ~3.4 GB/s, an unvectorized scan would
  eventually become the new bottleneck. Resolving AVX2 → SSE2 → scalar via
  `__builtin_cpu_supports` at startup, rather than assuming one ISA at compile
  time, means each machine automatically gets the fastest scan its own CPU
  can actually do — without needing to know in advance what hardware it'll
  run on, and without the portability risk a hardcoded `-mavx2` build flag
  would carry on an older or lower-power chip.
