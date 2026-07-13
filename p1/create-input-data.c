#include <stdio.h>
#include <stdlib.h>

#define BUFFER_INTS (16UL * 1024UL * 1024UL)  /* 64 MB buffer */

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <filename> <size_in_MB>\n", argv[0]);
        return 1;
    }

    const char *filename = argv[1];
    size_t size_mb = (size_t)atol(argv[2]);
    size_t target_bytes = size_mb * 1024UL * 1024UL;
    /* Round down to a multiple of sizeof(int) for alignment */
    size_t num_ints = target_bytes / sizeof(int);

    int *buf = malloc(BUFFER_INTS * sizeof(int));
    if (buf == NULL) {
        perror("Error allocating buffer");
        return 1;
    }

    FILE *file = fopen(filename, "wb");
    if (file == NULL) {
        perror("Error opening file");
        free(buf);
        return 1;
    }

    size_t next_val = 0;
    while (next_val < num_ints) {
        size_t count = num_ints - next_val;
        if (count > BUFFER_INTS)
            count = BUFFER_INTS;

        for (size_t i = 0; i < count; i++)
            buf[i] = (int)(next_val + i);

        if (fwrite(buf, sizeof(int), count, file) != count) {
            perror("Error writing to file");
            fclose(file);
            free(buf);
            return 1;
        }
        next_val += count;
    }

    fclose(file);
    free(buf);
    printf("Wrote %zu ints (%zu bytes) to %s\n", num_ints, num_ints * sizeof(int), filename);
    return 0;
}
