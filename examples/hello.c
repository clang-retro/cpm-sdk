/* Hello, world for CP/M-86.
 *
 *   clang --target=i8086-unknown-cpm hello.c -o hello.cmd
 */
#include <stdio.h>

int
main(void)
{
    printf("Hello, world, from clang on CP/M-86!\n");
    return 0;
}
