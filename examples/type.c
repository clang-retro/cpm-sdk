/* Print text files, as the CCP's TYPE does, with line numbers:
 *
 *   A>numtype hello.c b:readme.txt
 */
#include <stdio.h>

int
main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: numtype file...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (f == NULL) {
            perror(argv[i]);
            continue;
        }
        char line[256];
        int  n = 0;
        while (fgets(line, sizeof(line), f))
            printf("%4d  %s", ++n, line);
        fclose(f);
    }
    return 0;
}
