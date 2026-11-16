/* malloc up to the end of the data group, free and reuse it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(void)
{
    static char *blocks[256];
    int          n = 0;
    while (n < 256 && (blocks[n] = malloc(1024)) != NULL) {
        memset(blocks[n], n, 1024);
        n++;
    }
    int ok = n > 16;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < 1024; j += 97)
            if (blocks[i][j] != (char)i)
                ok = 0;
    for (int i = 0; i < n; i++)
        free(blocks[i]);
    char *big = malloc((size_t)(n - 2) * 1024);
    ok = ok && big != NULL;
    big = realloc(big, 100);
    ok = ok && big != NULL;
    free(big);
    printf("%s\n", ok ? "memory ok" : "memory FAILED");
    return !ok;
}
