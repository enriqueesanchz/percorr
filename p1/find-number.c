#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#define BUFFER_INTS (16UL * 1024UL * 1024UL)  /* 64 MB buffer */

typedef struct {
    FILE *file;
    int needle;
} worker_args;

void *worker(void *arg) {
    worker_args *args = arg;
    FILE *file = args->file;
    int needle = args->needle;

    int *buf = malloc(BUFFER_INTS * sizeof(int));
    if (buf == NULL) {
        perror("Error allocating buffer");
        return NULL;
    }

    size_t count;
    while ((count = fread(buf, sizeof(int), BUFFER_INTS, file)) > 0) {
        for (size_t i = 0; i < count; i++)
            if (buf[i] == needle)
                printf("FOUND: %d\n", buf[i]);
    }

    free(buf);
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

    worker_args args = { .file = file, .needle = atoi(argv[2]) };
    const int nthreads = 1;
    pthread_t threads[nthreads];

    for (int i = 0; i < nthreads; i++) {
        pthread_create(&threads[i], NULL, worker, &args);
    }

    for (int i = 0; i < nthreads; i++) {
        pthread_join(threads[i], NULL);
    }

    if (ferror(file)) {
        perror("Error reading file");
        fclose(file);
        return 1;
    }

    fclose(file);
    return 0;
}
