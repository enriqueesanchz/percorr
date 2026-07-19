#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#define BUFFER_INTS (256UL * 1024UL)  /* 1 MB buffer -- fio's best-measured seq. read size */
#define NTHREADS 8
#define ALIGN_BYTES 4096UL
#define ALIGN_INTS (ALIGN_BYTES / sizeof(int))  /* 1024 ints */

/* Scanning is currently fully hidden behind I/O wait on this hardware (see
 * problem.md), so this doesn't move wall-clock time on this machine -- it's
 * here for portability to faster storage (other laptops) where scanning
 * could become the bottleneck, and different laptops can have very
 * different CPUs. Rather than hardcode one ISA, pick the best one this
 * process's CPU actually supports at startup and hand that choice down --
 * resolved once, before any threads exist, so there's no data race around
 * a lazily-picked function pointer.
 *
 * AVX2 (8 ints/instruction) and SSE2 (4 ints/instruction) are hand-written
 * separately rather than relying on the compiler to auto-widen one body,
 * since GCC's function-multiversioning-by-redefinition (repeating the same
 * function name under different `target` attributes) is a C++-only
 * feature -- in C it's a redefinition error. SSE2 itself is part of the
 * mandatory x86-64 ABI baseline (every x86-64 CPU has it), so the only
 * real fallback case is non-x86 architectures, which get the plain scalar
 * version. */
typedef void (*scan_fn)(const int *buf, size_t count, int needle);

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2")))
static void scan_buffer_avx2(const int *buf, size_t count, int needle) {
    __m256i needle_vec = _mm256_set1_epi32(needle);
    size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        __m256i data = _mm256_loadu_si256((const __m256i *)&buf[i]);
        __m256i cmp = _mm256_cmpeq_epi32(data, needle_vec);
        if (_mm256_movemask_epi8(cmp)) {
            for (int j = 0; j < 8; j++)
                if (buf[i + j] == needle)
                    printf("FOUND: %d\n", buf[i + j]);
        }
    }
    for (; i < count; i++)
        if (buf[i] == needle)
            printf("FOUND: %d\n", buf[i]);
}

__attribute__((target("sse2")))
static void scan_buffer_sse2(const int *buf, size_t count, int needle) {
    __m128i needle_vec = _mm_set1_epi32(needle);
    size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        __m128i data = _mm_loadu_si128((const __m128i *)&buf[i]);
        __m128i cmp = _mm_cmpeq_epi32(data, needle_vec);
        if (_mm_movemask_epi8(cmp)) {
            for (int j = 0; j < 4; j++)
                if (buf[i + j] == needle)
                    printf("FOUND: %d\n", buf[i + j]);
        }
    }
    for (; i < count; i++)
        if (buf[i] == needle)
            printf("FOUND: %d\n", buf[i]);
}
#endif

static void scan_buffer_scalar(const int *buf, size_t count, int needle) {
    for (size_t i = 0; i < count; i++)
        if (buf[i] == needle)
            printf("FOUND: %d\n", buf[i]);
}

static scan_fn resolve_scan_buffer(void) {
#if defined(__x86_64__) || defined(__i386__)
    if (__builtin_cpu_supports("avx2"))
        return scan_buffer_avx2;
    if (__builtin_cpu_supports("sse2"))
        return scan_buffer_sse2;
#endif
    return scan_buffer_scalar;
}

typedef struct {
    const char *filename;
    int needle;
    long start_int;   /* index of first int this thread owns */
    long count_int;   /* how many ints this thread owns */
    scan_fn scan;
} worker_args;

void *worker(void *arg) {
    worker_args *args = arg;

    /* O_DIRECT bypasses the page cache: no double-copy through kernel
     * buffers, and it doesn't evict other processes' cached data just to
     * stream this file through once. */
    int fd = open(args->filename, O_RDONLY | O_DIRECT);
    if (fd < 0) {
        perror("Error opening file");
        return NULL;
    }

    int *buf;
    if (posix_memalign((void **)&buf, ALIGN_BYTES, BUFFER_INTS * sizeof(int)) != 0) {
        perror("Error allocating aligned buffer");
        close(fd);
        return NULL;
    }

    off_t offset = (off_t)args->start_int * sizeof(int);
    long remaining = args->count_int;
    while (remaining > 0) {
        size_t want_ints = remaining < (long)BUFFER_INTS ? (size_t)remaining : BUFFER_INTS;
        /* O_DIRECT requires the requested length itself to be a multiple of
         * the alignment, even for the final read at true EOF -- the kernel
         * is only allowed to return fewer bytes than requested, not accept
         * an unaligned request. Round up; a short count from the actual
         * read still tells us exactly how much real data came back. */
        size_t want_bytes = want_ints * sizeof(int);
        want_bytes = (want_bytes + ALIGN_BYTES - 1) / ALIGN_BYTES * ALIGN_BYTES;

        ssize_t n = pread(fd, buf, want_bytes, offset);
        if (n <= 0) {
            if (n < 0)
                perror("Error reading file");
            break;
        }

        size_t count = (size_t)n / sizeof(int);
        if (count > (size_t)remaining)
            count = (size_t)remaining;

        args->scan(buf, count, args->needle);

        offset += (off_t)count * sizeof(int);
        remaining -= (long)count;
    }

    free(buf);
    close(fd);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <filename> <number>\n", argv[0]);
        return 1;
    }

    struct stat st;
    if (stat(argv[1], &st) != 0) {
        perror("Error stating file");
        return 1;
    }

    long total_ints = st.st_size / sizeof(int);
    int needle = atoi(argv[2]);

    worker_args args[NTHREADS];
    pthread_t threads[NTHREADS];
    scan_fn scan = resolve_scan_buffer();

    /* Every thread's start/length must be a multiple of the alignment
     * O_DIRECT requires, except the very last read of the whole file,
     * which the kernel allows to be short. */
    long base_aligned = (total_ints / NTHREADS / ALIGN_INTS) * ALIGN_INTS;
    long next_start = 0;

    for (int i = 0; i < NTHREADS; i++) {
        long count = (i < NTHREADS - 1) ? base_aligned : (total_ints - next_start);
        args[i] = (worker_args){
            .filename = argv[1],
            .needle = needle,
            .start_int = next_start,
            .count_int = count,
            .scan = scan,
        };
        next_start += count;
        pthread_create(&threads[i], NULL, worker, &args[i]);
    }

    for (int i = 0; i < NTHREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    return 0;
}
