/* Hello, world for CP/M-86.
 *
 *   clang --target=i8086-unknown-cpm hello.c -o hello.cmd
 */
#include <stdio.h>

int
main(void)
{
#ifdef __CPM80__
    printf("Hello, world, from clang on CP/M-80!\n");
#else
    printf("Hello, world, from clang on CP/M-86!\n");
#endif
    return 0;
}
