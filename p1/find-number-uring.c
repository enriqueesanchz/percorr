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
#include <liburing.h>

/* Single O_DIRECT fd + single io_uring ring, driven by one submitter that
 * keeps exactly QUEUE_DEPTH reads in flight at all times -- this is the
 * direct analog of fio's own "one job, iodepth N" measurement, which is
 * what actually produced the throughput ceiling this project has been
 * chasing (fio's own sweep shows a single queue/job saturates this drive;
 * more concurrent jobs don't help). A separate scanner thread drains
 * completed buffers so the submitter is never blocked on the (cheap, but
 * non-zero) needle check -- same reasoning as find-number-pipelined.c. */

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
 * malloc/mmap. Verbatim from find-number-pipelined.c. */
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

/* Per-in-flight-request context, carried through the ring via
 * io_uring_sqe_set_data/io_uring_cqe_get_data. want_ints is the logical
 * number of ints this chunk represents -- needed to clip the byte count
 * an out-of-order completion reports back to, independent of submission
 * order. */
typedef struct {
    int *buf;
    long want_ints;
} read_ctx;

static size_t align_up_bytes(size_t bytes) {
    return (bytes + ALIGN_BYTES - 1) / ALIGN_BYTES * ALIGN_BYTES;
}

static struct io_uring_sqe *get_sqe(struct io_uring *ring) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe == NULL) {
        io_uring_submit(ring);
        sqe = io_uring_get_sqe(ring);
    }
    return sqe;
}

/* Pops a free buffer, computes the alignment-safe request size for the
 * next sequential chunk, and prepares (but does not submit) a read SQE.
 * Returns 1 and advances next_offset_int and in_flight, or 0 if there was
 * no file left to claim. */
static int submit_chunk(struct io_uring *uring, ring *free_ring, int fd,
                         long *next_offset_int, long total_ints, int *in_flight) {
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

    struct io_uring_sqe *sqe = get_sqe(uring);
    io_uring_prep_read(sqe, fd, buf, want_bytes, (off_t)*next_offset_int * sizeof(int));
    io_uring_sqe_set_data(sqe, ctx);

    *next_offset_int += want_ints;
    (*in_flight)++;
    return 1;
}

/* Handles one completed read: on error or true EOF, just recycles the
 * buffer without producing a job. Otherwise trusts the returned byte
 * count -- the same "requested length must be alignment-sized, but the
 * actual return can be a short count at true EOF" contract as the
 * O_DIRECT pread versions -- clips it to this chunk's logical want_ints,
 * and hands the buffer off to the scanner. */
static void handle_completion(struct io_uring_cqe *cqe, ring *work_ring, ring *free_ring) {
    read_ctx *ctx = io_uring_cqe_get_data(cqe);
    int res = cqe->res;

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

    struct io_uring uring;
    int rc = io_uring_queue_init(QUEUE_DEPTH, &uring, 0);
    if (rc < 0) {
        fprintf(stderr, "Error initializing io_uring: %s\n", strerror(-rc));
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
    for (int i = 0; i < QUEUE_DEPTH; i++) {
        if (!submit_chunk(&uring, &free_ring, fd, &next_offset_int, total_ints, &in_flight)) {
            no_more_work = 1;
            break;
        }
    }
    io_uring_submit(&uring);

    /* Steady state: one blocking wait per batch, drain everything already
     * ready non-blockingly, then refill one-for-one before the next wait. */
    while (in_flight > 0) {
        struct io_uring_cqe *cqe;
        int wrc = io_uring_wait_cqe(&uring, &cqe);
        if (wrc < 0) {
            fprintf(stderr, "Error waiting for completion: %s\n", strerror(-wrc));
            break;
        }

        struct io_uring_cqe *completed[QUEUE_DEPTH];
        int ncompleted = 0;
        completed[ncompleted++] = cqe;
        io_uring_cqe_seen(&uring, cqe);

        while (ncompleted < QUEUE_DEPTH && io_uring_peek_cqe(&uring, &cqe) == 0) {
            completed[ncompleted++] = cqe;
            io_uring_cqe_seen(&uring, cqe);
        }

        for (int i = 0; i < ncompleted; i++) {
            handle_completion(completed[i], &work_ring, &free_ring);
            in_flight--;
        }

        if (!no_more_work) {
            for (int i = 0; i < ncompleted; i++) {
                if (!submit_chunk(&uring, &free_ring, fd, &next_offset_int, total_ints, &in_flight)) {
                    no_more_work = 1;
                    break;
                }
            }
            io_uring_submit(&uring);
        }
    }

    ring_producer_done(&work_ring);
    for (int i = 0; i < NSCANNERS; i++)
        pthread_join(scanners[i], NULL);

    io_uring_queue_exit(&uring);
    close(fd);
    for (int i = 0; i < POOL_SIZE; i++)
        free(buffers[i]);

    return 0;
}
