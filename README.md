# percorr

Experiments in pushing storage I/O to the hardware limit.

## P1

Finding an integer in a large unsorted binary file of 32-bit ints that doesn't
fit in memory. No index and no ordering means no early exit, so the whole file
is read every run and the only lever is raw read-and-scan speed.
Five C implementations (`O_DIRECT` threads, an HDD-tuned producer/consumer, a
decoupled reader/scanner pipeline, `io_uring`, `libaio`) are compared against a
drive ceiling measured independently with `fio`; See
[`p1/README.md`](p1/README.md) for usage and [`p1/problem.md`](p1/problem.md)
for the write-up.
