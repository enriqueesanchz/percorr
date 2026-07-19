#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <pthread.h>

/* Tuned for spinning disks: exactly one thread ever touches the file,
 * reading it strictly sequentially in large chunks. Multiple worker
 * threads consume the chunks from a bounded queue to parallelize the
 * scan without ever causing a seek. */

#define BUFFER_INTS (16UL * 1024UL * 1024UL)  /* 64 MB per chunk */
#define QUEUE_CAPACITY 4                       /* chunks in flight */
#define NCONSUMERS 4

typedef struct {
    int *buf;
    size_t count;
} job;

typedef struct {
    job jobs[QUEUE_CAPACITY];
    int head, tail, size;
    int done;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} queue;

void queue_init(queue *q) {
    q->head = q->tail = q->size = 0;
    q->done = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

void queue_push(queue *q, int *buf, size_t count) {
    pthread_mutex_lock(&q->mutex);
    while (q->size == QUEUE_CAPACITY)
        pthread_cond_wait(&q->not_full, &q->mutex);

    q->jobs[q->tail] = (job){ .buf = buf, .count = count };
    q->tail = (q->tail + 1) % QUEUE_CAPACITY;
    q->size++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}

/* Returns 1 and fills *out on success, 0 once the queue is drained and done. */
int queue_pop(queue *q, job *out) {
    pthread_mutex_lock(&q->mutex);
    while (q->size == 0 && !q->done)
        pthread_cond_wait(&q->not_empty, &q->mutex);

    if (q->size == 0 && q->done) {
        pthread_mutex_unlock(&q->mutex);
        return 0;
    }

    *out = q->jobs[q->head];
    q->head = (q->head + 1) % QUEUE_CAPACITY;
    q->size--;

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return 1;
}

void queue_mark_done(queue *q) {
    pthread_mutex_lock(&q->mutex);
    q->done = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}

typedef struct {
    const char *filename;
    queue *q;
} producer_args;

void *producer(void *arg) {
    producer_args *args = arg;

    FILE *file = fopen(args->filename, "rb");
    if (file == NULL) {
        perror("Error opening file");
        queue_mark_done(args->q);
        return NULL;
    }

    /* Tell the kernel this fd will be read sequentially so it can
     * be aggressive about readahead -- the main win on a spinning disk. */
    posix_fadvise(fileno(file), 0, 0, POSIX_FADV_SEQUENTIAL);

    for (;;) {
        int *buf = malloc(BUFFER_INTS * sizeof(int));
        if (buf == NULL) {
            perror("Error allocating buffer");
            break;
        }

        size_t count = fread(buf, sizeof(int), BUFFER_INTS, file);
        if (count == 0) {
            free(buf);
            break;
        }

        queue_push(args->q, buf, count);
    }

    if (ferror(file))
        perror("Error reading file");

    fclose(file);
    queue_mark_done(args->q);
    return NULL;
}

typedef struct {
    queue *q;
    int needle;
} consumer_args;

void *consumer(void *arg) {
    consumer_args *args = arg;
    job j;

    while (queue_pop(args->q, &j)) {
        for (size_t i = 0; i < j.count; i++)
            if (j.buf[i] == args->needle)
                printf("FOUND: %d\n", j.buf[i]);
        free(j.buf);
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <filename> <number>\n", argv[0]);
        return 1;
    }

    int needle = atoi(argv[2]);

    queue q;
    queue_init(&q);

    producer_args pargs = { .filename = argv[1], .q = &q };
    pthread_t producer_thread;
    pthread_create(&producer_thread, NULL, producer, &pargs);

    consumer_args cargs = { .q = &q, .needle = needle };
    pthread_t consumers[NCONSUMERS];
    for (int i = 0; i < NCONSUMERS; i++)
        pthread_create(&consumers[i], NULL, consumer, &cargs);

    pthread_join(producer_thread, NULL);
    for (int i = 0; i < NCONSUMERS; i++)
        pthread_join(consumers[i], NULL);

    return 0;
}
