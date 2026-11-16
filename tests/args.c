/* argv from the command tail, printf of the integer and float kinds, and
 * the exit status. On CP/M-80 the double printf and strtod don't fit in
 * a TPA, so the float kinds are left out there (args-80.out). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv)
{
    printf("argc %d\n", argc);
    for (int i = 1; i < argc; i++)
        printf("argv[%d] %s\n", i, argv[i]);
    printf("%d %u %ld %lx %lld\n", -12345, 54321u, -1234567890L, 0xdeadbeefUL,
           -1234567890123456789LL);
#ifndef __CPM80__
    printf("%.3f %g %e\n", 3.14159, 1.0 / 3, 6.02e23);
#endif
    char buf[32];
    snprintf(buf, sizeof(buf), "%05d|%-4s|%x", 42, "ab", 255);
    puts(buf);
#ifdef __CPM80__
    printf("strtol %ld\n", strtol("-77", NULL, 10));
#else
    printf("strtol %ld atof %.2f\n", strtol("-77", NULL, 10), atof("2.5"));
#endif
    return argc == 3 ? 7 : 0;
}
