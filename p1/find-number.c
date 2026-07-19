#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <pthread.h>

#define BUFFER_INTS (16UL * 1024UL * 1024UL)  /* 64 MB buffer */
#define NTHREADS 4

typedef struct {
    const char *filename;
    int needle;
    long start_int;   /* index of first int this thread owns */
    long count_int;   /* how many ints this thread owns */
} worker_args;

void *worker(void *arg) {
    worker_args *args = arg;

    FILE *file = fopen(args->filename, "rb");
    if (file == NULL) {
        perror("Error opening file");
        return NULL;
    }

    if (fseeko(file, (off_t)args->start_int * sizeof(int), SEEK_SET) != 0) {
        perror("Error seeking file");
        fclose(file);
        return NULL;
    }

    /* Each thread reads its own slice strictly sequentially -- tell the
     * kernel so it can keep readahead pipelined ahead of demand. */
    posix_fadvise(fileno(file), (off_t)args->start_int * sizeof(int),
                  (off_t)args->count_int * sizeof(int), POSIX_FADV_SEQUENTIAL);

    int *buf = malloc(BUFFER_INTS * sizeof(int));
    if (buf == NULL) {
        perror("Error allocating buffer");
        fclose(file);
        return NULL;
    }

    long remaining = args->count_int;
    while (remaining > 0) {
        size_t want = remaining < (long)BUFFER_INTS ? (size_t)remaining : BUFFER_INTS;
        size_t count = fread(buf, sizeof(int), want, file);
        if (count == 0)
            break;

        for (size_t i = 0; i < count; i++)
            if (buf[i] == args->needle)
                printf("FOUND: %d\n", buf[i]);

        remaining -= (long)count;
    }

    if (ferror(file))
        perror("Error reading file");

    free(buf);
    fclose(file);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <filename> <number>\n", argv[0]);
        return 1;
    }

    FILE *file = fopen(argv[1], "rb");
    if (file == NULL) {
        perror("Error opening file");
        return 1;
    }

    if (fseeko(file, 0, SEEK_END) != 0) {
        perror("Error seeking file");
        fclose(file);
        return 1;
    }
    off_t size = ftello(file);
    fclose(file);

    long total_ints = size / sizeof(int);
    int needle = atoi(argv[2]);

    worker_args args[NTHREADS];
    pthread_t threads[NTHREADS];

    long base = total_ints / NTHREADS;
    long extra = total_ints % NTHREADS;
    long next_start = 0;

    for (int i = 0; i < NTHREADS; i++) {
        long count = base + (i < extra ? 1 : 0);
        args[i] = (worker_args){
            .filename = argv[1],
            .needle = needle,
            .start_int = next_start,
            .count_int = count,
        };
        next_start += count;
        pthread_create(&threads[i], NULL, worker, &args[i]);
    }

    for (int i = 0; i < NTHREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    return 0;
}
