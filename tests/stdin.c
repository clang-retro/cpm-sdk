/* Lines from the console (BDOS function 10), up to Ctrl-Z. */
#include <stdio.h>
#include <string.h>

int
main(void)
{
    char line[100];
    int  n = 0;
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = '\0';
        printf("got [%s]\n", line);
        n++;
    }
    printf("%d lines\n", n);
    return 0;
}
