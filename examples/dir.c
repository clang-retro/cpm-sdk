/* List files with their sizes, with BDOS calls through <cpm.h>: search
 * for first and next (17 and 18) and compute file size (35).
 *
 *   A>dirsize *.cmd
 */
#include <cpm.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct fcb {
    uint8_t dr;
    char    name[11];
    uint8_t rest[24];
};

/* "*.c" into an FCB name with ? for each wild position. */
static void
pattern(struct fcb *f, const char *s)
{
    memset(f, 0, sizeof(*f));
    memset(f->name, ' ', 11);
    if (s[0] && s[1] == ':') {
        f->dr = (uint8_t)((s[0] | 0x20) - 'a' + 1);
        s += 2;
    }
    int i = 0;
    for (; *s && *s != '.' && i < 8; s++)
        if (*s == '*')
            while (i < 8)
                f->name[i++] = '?';
        else
            f->name[i++] = (char)(*s >= 'a' && *s <= 'z' ? *s - 32 : *s);
    while (*s && *s != '.')
        s++;
    if (*s == '.')
        s++;
    for (i = 8; *s && i < 11; s++)
        if (*s == '*')
            while (i < 11)
                f->name[i++] = '?';
        else
            f->name[i++] = (char)(*s >= 'a' && *s <= 'z' ? *s - 32 : *s);
    if (i == 8 && f->name[0] == '?')
        memset(f->name + 8, '?', 3);
}

int
main(int argc, char **argv)
{
    static uint8_t dma[128];
    static char    names[64][11];
    struct fcb     f;
    int            n = 0;

    pattern(&f, argc > 1 ? argv[1] : "*.*");
    bdos(26, (unsigned)(uintptr_t)dma); /* set the DMA address */
    for (unsigned r = bdos(17, (unsigned)(uintptr_t)&f) & 0xff; r != 0xff && n < 64;
         r = bdos(18, 0) & 0xff) {
        const uint8_t *e = dma + 32 * r;
        if (e[12] != 0 || e[14] != 0)
            continue; /* not the first extent */
        for (int i = 0; i < 11; i++)
            names[n][i] = (char)(e[1 + i] & 0x7f);
        n++;
    }
    long total = 0;
    for (int i = 0; i < n; i++) {
        struct fcb s;
        memset(&s, 0, sizeof(s));
        s.dr = f.dr;
        memcpy(s.name, names[i], 11);
        bdos(35, (unsigned)(uintptr_t)&s);
        long bytes = (s.rest[21] | ((long)s.rest[22] << 8) | ((long)s.rest[23] << 16)) * 128L;
        total += bytes;
        printf("%.8s.%.3s %8ld\n", names[i], names[i] + 8, bytes);
    }
    printf("%d files, %ld bytes\n", n, total);
    return 0;
}
