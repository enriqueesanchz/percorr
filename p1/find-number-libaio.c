#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <libaio.h>

/* Same single-submitter, steady-state-queue-depth architecture as
 * find-number-uring.c, but through the older Linux AIO interface
 * (io_setup/io_submit/io_getevents) instead of io_uring -- this is
 * literally the engine fio itself used (--ioengine=libaio) to produce
 * the throughput numbers this project has been chasing all along, so
 * it's the closest possible reproduction of that specific ceiling. */

#define BUFFER_INTS (256UL * 1024UL)  /* 1 MB buffer -- fio's best-measured seq. read size */
#ifndef QUEUE_DEPTH
#define QUEUE_DEPTH 4      /* matches fio's bs=1m qd=4 nj=1, ~3428 MB/s -- near the overall ceiling */
#endif
#define NSCANNERS 1        /* scanning is cheap; one thread easily keeps up */
#define POOL_SIZE (QUEUE_DEPTH * 3)
#define ALIGN_BYTES 4096UL
#define ALIGN_INTS (ALIGN_BYTES / sizeof(int))

/* Bounded ring buffer, used two ways: a "free" ring of empty aligned
 * buffers and a "work" ring of filled jobs. No allocation happens once
 * the pool is seeded, so the submitter and scanner never stall on
 * malloc/mmap. Verbatim from find-number-uring.c / find-number-pipelined.c. */
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

/* Per-in-flight-request context, carried through the iocb's user "data"
 * field. want_ints is the logical number of ints this chunk represents --
 * needed to clip the byte count an out-of-order completion reports back
 * to, independent of submission order. */
typedef struct {
    int *buf;
    long want_ints;
} read_ctx;

static size_t align_up_bytes(size_t bytes) {
    return (bytes + ALIGN_BYTES - 1) / ALIGN_BYTES * ALIGN_BYTES;
}

/* Pops a free buffer, computes the alignment-safe request size for the
 * next sequential chunk, and builds (but does not submit) one iocb.
 * Returns 1 and fills *out_iocb and advances next_offset_int, or 0 if
 * there was no file left to claim. */
static int prepare_chunk(ring *free_ring, int fd, long *next_offset_int,
                          long total_ints, struct iocb **out_iocb) {
    if (*next_offset_int >= total_ints)
        return 0;

    void *raw;
    ring_pop(free_ring, &raw);   /* free ring never closes, always succeeds */
    int *buf = raw;

    long remaining = total_ints - *next_offset_int;
    long want_ints = remaining < (long)BUFFER_INTS ? remaining : (long)BUFFER_INTS;
    size_t want_bytes = align_up_bytes((size_t)want_ints * sizeof(int));

    read_ctx *ctx = malloc(sizeof(read_ctx));
    ctx->buf = buf;
    ctx->want_ints = want_ints;

    struct iocb *cb = malloc(sizeof(struct iocb));
    io_prep_pread(cb, fd, buf, want_bytes, (off_t)*next_offset_int * sizeof(int));
    cb->data = ctx;

    *next_offset_int += want_ints;
    *out_iocb = cb;
    return 1;
}

/* Handles one completed read: on error or true EOF, just recycles the
 * buffer without producing a job. Otherwise trusts the returned byte
 * count -- the same "requested length must be alignment-sized, but the
 * actual return can be a short count at true EOF" contract as the
 * O_DIRECT pread/io_uring versions -- clips it to this chunk's logical
 * want_ints, and hands the buffer off to the scanner. */
static void handle_completion(struct io_event *ev, ring *work_ring, ring *free_ring) {
    read_ctx *ctx = ev->data;
    struct iocb *cb = ev->obj;
    long res = (long)ev->res;

    if (res < 0) {
        fprintf(stderr, "Error reading file: %s\n", strerror(-res));
        ring_push(free_ring, ctx->buf);
    } else if (res == 0) {
        ring_push(free_ring, ctx->buf);
    } else {
        size_t count = (size_t)res / sizeof(int);
        if (count > (size_t)ctx->want_ints)
            count = (size_t)ctx->want_ints;

        filled_job *j = malloc(sizeof(filled_job));
        j->buf = ctx->buf;
        j->count = count;
        ring_push(work_ring, j);
    }

    free(ctx);
    free(cb);
}

/* Fills up to QUEUE_DEPTH slots of cbs[] starting from scratch (priming)
 * or refilling after a batch of completions, submitting once for the
 * whole batch. Returns the number actually prepared. */
static int prepare_and_submit_batch(io_context_t aioctx, ring *free_ring, int fd,
                                     long *next_offset_int, long total_ints,
                                     int max_batch, int *no_more_work) {
    struct iocb *cbs[QUEUE_DEPTH];
    int nprep = 0;

    for (int i = 0; i < max_batch; i++) {
        struct iocb *cb;
        if (!prepare_chunk(free_ring, fd, next_offset_int, total_ints, &cb)) {
            *no_more_work = 1;
            break;
        }
        cbs[nprep++] = cb;
    }

    if (nprep > 0) {
        int submitted = io_submit(aioctx, nprep, cbs);
        if (submitted < 0)
            fprintf(stderr, "Error submitting reads: %s\n", strerror(-submitted));
    }

    return nprep;
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

    int fd = open(argv[1], O_RDONLY | O_DIRECT);
    if (fd < 0) {
        perror("Error opening file");
        return 1;
    }

    io_context_t aioctx = 0;
    int rc = io_setup(QUEUE_DEPTH, &aioctx);
    if (rc < 0) {
        fprintf(stderr, "Error initializing libaio context: %s\n", strerror(-rc));
        close(fd);
        return 1;
    }

    ring free_ring, work_ring;
    ring_init(&free_ring, 1);   /* "1 producer": main seeds it once, then never closes */
    ring_init(&work_ring, 1);   /* the single submitter loop below is the sole producer */

    /* Pre-allocate every buffer once; the submitter/scanner just recycle
     * these, so no malloc/mmap ever happens on the hot path. */
    int *buffers[POOL_SIZE];
    for (int i = 0; i < POOL_SIZE; i++) {
        if (posix_memalign((void **)&buffers[i], ALIGN_BYTES, BUFFER_INTS * sizeof(int)) != 0) {
            perror("Error allocating buffer pool");
            return 1;
        }
        ring_push(&free_ring, buffers[i]);
    }

    scanner_args sargs = { .work_ring = &work_ring, .free_ring = &free_ring, .needle = needle };
    pthread_t scanners[NSCANNERS];
    for (int i = 0; i < NSCANNERS; i++)
        pthread_create(&scanners[i], NULL, scanner, &sargs);

    long next_offset_int = 0;
    int in_flight = 0;
    int no_more_work = 0;

    /* Priming: fill the queue depth before waiting on anything. */
    in_flight += prepare_and_submit_batch(aioctx, &free_ring, fd, &next_offset_int,
                                           total_ints, QUEUE_DEPTH, &no_more_work);

    /* Steady state: io_getevents blocks until >=1 completion is ready but
     * hands back up to QUEUE_DEPTH in one call, so there's no separate
     * wait-then-peek dance like io_uring needs -- one call drains the
     * whole ready batch directly. */
    struct io_event events[QUEUE_DEPTH];
    while (in_flight > 0) {
        int n = io_getevents(aioctx, 1, QUEUE_DEPTH, events, NULL);
        if (n < 0) {
            fprintf(stderr, "Error waiting for completions: %s\n", strerror(-n));
            break;
        }

        for (int i = 0; i < n; i++)
            handle_completion(&events[i], &work_ring, &free_ring);
        in_flight -= n;

        if (!no_more_work)
            in_flight += prepare_and_submit_batch(aioctx, &free_ring, fd, &next_offset_int,
                                                   total_ints, n, &no_more_work);
    }

    ring_producer_done(&work_ring);
    for (int i = 0; i < NSCANNERS; i++)
        pthread_join(scanners[i], NULL);

    io_destroy(aioctx);
    close(fd);
    for (int i = 0; i < POOL_SIZE; i++)
        free(buffers[i]);

    return 0;
}
