#include <stdio.h>
#include <stdlib.h>

#define BUFFER_INTS (16UL * 1024UL * 1024UL)  /* 64 MB buffer */

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <filename>\n", argv[0]);
        return 1;
    }

    FILE *file = fopen(argv[1], "rb");
    if (file == NULL) {
        perror("Error opening file");
        return 1;
    }

    int *buf = malloc(BUFFER_INTS * sizeof(int));
    if (buf == NULL) {
        perror("Error allocating buffer");
        fclose(file);
        return 1;
    }

    size_t count;
    while ((count = fread(buf, sizeof(int), BUFFER_INTS, file)) > 0) {
        for (size_t i = 0; i < count; i++)
            printf("%d\n", buf[i]);
    }

    if (ferror(file)) {
        perror("Error reading file");
        free(buf);
        fclose(file);
        return 1;
    }

    free(buf);
    fclose(file);
    return 0;
}
