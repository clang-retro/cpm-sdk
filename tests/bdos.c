/* <cpm.h>: bdos() and the base page. */
#include <cpm.h>
#include <stdint.h>
#include <stdio.h>

int
main(void)
{
    unsigned v = bdos(12, 0);
    printf("version %04x\n", v);
    printf("_cpm_version %04x\n", _cpm_version);
#ifdef __TINY__
    printf("8080 model %d\n", _cpm_base_page[5]);
#else
    printf("8080 model %d\n", _cpm_base_page[5] ^ 1);
#endif
    printf("default FCB %.8s\n", (const char *)_cpm_base_page + 0x5d);
    bdos(9, (unsigned)(uintptr_t)"print string$");
    putch('\r');
    putch('\n');
    cputs("cputs\r\n");
    return 0;
}
