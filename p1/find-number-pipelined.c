#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>

/* Same static file split and O_DIRECT reads as find-number.c, but readers
 * and scanning are decoupled: NREADERS threads do nothing but pread() into
 * a pool of recycled buffers, and a separate scanner thread drains them.
 * A reader never stalls waiting for a scan to finish, so it can keep
 * issuing the next read immediately -- this is the double-buffering idea:
 * overlap I/O wait with the (cheap, but non-zero) needle check. */

#define BUFFER_INTS (256UL * 1024UL)  /* 1 MB buffer -- fio's best-measured seq. read size */
#define NREADERS 8
#define NSCANNERS 1        /* scanning is cheap; one thread easily keeps up with 8 readers */
#define POOL_SIZE (NREADERS * 3)
#define ALIGN_BYTES 4096UL
#define ALIGN_INTS (ALIGN_BYTES / sizeof(int))  /* 1024 ints */

/* Bounded ring buffer, used two ways: a "free" ring of empty aligned
 * buffers and a "work" ring of filled jobs. No allocation happens once
 * the pool is seeded, so readers never stall on malloc/mmap. */
typedef struct {
    void *items[POOL_SIZE + 1];   /* +1 slot to distinguish empty from full */
    int head, tail;
    int active_producers;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty, not_full;
} ring;

void ring_init(ring *r, int active_producers) {
    r->head = r->tail = 0;
    r->active_producers = active_producers;
    pthread_mutex_init(&r->mutex, NULL);
    pthread_cond_init(&r->not_empty, NULL);
    pthread_cond_init(&r->not_full, NULL);
}

static int ring_size(ring *r) {
    return (r->tail - r->head + (POOL_SIZE + 1)) % (POOL_SIZE + 1);
}

void ring_push(ring *r, void *item) {
    pthread_mutex_lock(&r->mutex);
    while (ring_size(r) == POOL_SIZE)
        pthread_cond_wait(&r->not_full, &r->mutex);
    r->items[r->tail] = item;
    r->tail = (r->tail + 1) % (POOL_SIZE + 1);
    pthread_cond_signal(&r->not_empty);
    pthread_mutex_unlock(&r->mutex);
}

/* Returns 1 and fills *out, or 0 once closed (no active producers) and drained. */
int ring_pop(ring *r, void **out) {
    pthread_mutex_lock(&r->mutex);
    while (ring_size(r) == 0 && r->active_producers > 0)
        pthread_cond_wait(&r->not_empty, &r->mutex);
    if (ring_size(r) == 0 && r->active_producers == 0) {
        pthread_mutex_unlock(&r->mutex);
        return 0;
    }
    *out = r->items[r->head];
    r->head = (r->head + 1) % (POOL_SIZE + 1);
    pthread_cond_signal(&r->not_full);
    pthread_mutex_unlock(&r->mutex);
    return 1;
}

void ring_producer_done(ring *r) {
    pthread_mutex_lock(&r->mutex);
    r->active_producers--;
    if (r->active_producers == 0)
        pthread_cond_broadcast(&r->not_empty);
    pthread_mutex_unlock(&r->mutex);
}

typedef struct {
    int *buf;
    size_t count;
} filled_job;

typedef struct {
    const char *filename;
    long start_int;
    long count_int;
    ring *free_ring;   /* holds int* (empty aligned buffers) */
    ring *work_ring;    /* holds filled_job* */
} reader_args;

void *reader(void *arg) {
    reader_args *args = arg;

    /* O_DIRECT bypasses the page cache: no double-copy through kernel
     * buffers, and it doesn't evict other processes' cached data just to
     * stream this file through once. */
    int fd = open(args->filename, O_RDONLY | O_DIRECT);
    if (fd < 0) {
        perror("Error opening file");
        ring_producer_done(args->work_ring);
        return NULL;
    }

    off_t offset = (off_t)args->start_int * sizeof(int);
    long remaining = args->count_int;
    while (remaining > 0) {
        void *raw;
        ring_pop(args->free_ring, &raw);   /* free ring never closes, always succeeds */
        int *buf = raw;

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
            ring_push(args->free_ring, buf);
            break;
        }

        size_t count = (size_t)n / sizeof(int);
        if (count > (size_t)remaining)
            count = (size_t)remaining;

        offset += (off_t)count * sizeof(int);
        remaining -= (long)count;

        filled_job *j = malloc(sizeof(filled_job));  /* tiny fixed-size struct, not the data */
        j->buf = buf;
        j->count = count;
        ring_push(args->work_ring, j);
    }

    close(fd);
    ring_producer_done(args->work_ring);
    return NULL;
}

typedef struct {
    ring *work_ring;
    ring *free_ring;
    int needle;
} scanner_args;

void *scanner(void *arg) {
    scanner_args *args = arg;
    void *raw;
    while (ring_pop(args->work_ring, &raw)) {
        filled_job *j = raw;
        for (size_t i = 0; i < j->count; i++)
            if (j->buf[i] == args->needle)
                printf("FOUND: %d\n", j->buf[i]);
        ring_push(args->free_ring, j->buf);
        free(j);
    }
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

    ring free_ring, work_ring;
    ring_init(&free_ring, 1);          /* "1 producer": main seeds it once, then never closes */
    ring_init(&work_ring, NREADERS);

    /* Pre-allocate every buffer once; readers/scanner just recycle these,
     * so no malloc/mmap ever happens on the hot path. */
    int *buffers[POOL_SIZE];
    for (int i = 0; i < POOL_SIZE; i++) {
        if (posix_memalign((void **)&buffers[i], ALIGN_BYTES, BUFFER_INTS * sizeof(int)) != 0) {
            perror("Error allocating buffer pool");
            return 1;
        }
        ring_push(&free_ring, buffers[i]);
    }

    reader_args rargs[NREADERS];
    pthread_t readers[NREADERS];
    scanner_args sargs = { .work_ring = &work_ring, .free_ring = &free_ring, .needle = needle };
    pthread_t scanners[NSCANNERS];

    /* Every thread's start/length must be a multiple of the alignment
     * O_DIRECT requires, except the very last read of the whole file,
     * which the kernel allows to be short. */
    long base_aligned = (total_ints / NREADERS / ALIGN_INTS) * ALIGN_INTS;
    long next_start = 0;

    for (int i = 0; i < NREADERS; i++) {
        long count = (i < NREADERS - 1) ? base_aligned : (total_ints - next_start);
        rargs[i] = (reader_args){
            .filename = argv[1],
            .start_int = next_start,
            .count_int = count,
            .free_ring = &free_ring,
            .work_ring = &work_ring,
        };
        next_start += count;
        pthread_create(&readers[i], NULL, reader, &rargs[i]);
    }

    for (int i = 0; i < NSCANNERS; i++)
        pthread_create(&scanners[i], NULL, scanner, &sargs);

    for (int i = 0; i < NREADERS; i++)
        pthread_join(readers[i], NULL);
    for (int i = 0; i < NSCANNERS; i++)
        pthread_join(scanners[i], NULL);

    return 0;
}
