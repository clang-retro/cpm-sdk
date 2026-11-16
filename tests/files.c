/* Files through FCBs: text and binary modes, seeking, appending, renaming,
 * removing and stat. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            printf("FAIL line %d: %s\n", __LINE__, #c);             \
            failures++;                                             \
        }                                                           \
    } while (0)

int
main(void)
{
    char line[80];

    /* Text: LF is written as CR LF and the file ends at Ctrl-Z. */
    FILE *f = fopen("text.txt", "w");
    CHECK(f != NULL);
    for (int i = 0; i < 100; i++)
        fprintf(f, "line %d of the text file\n", i);
    CHECK(fclose(f) == 0);
    f = fopen("TEXT.TXT", "r");
    CHECK(f != NULL);
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        char want[40];
        snprintf(want, sizeof(want), "line %d of the text file\n", n);
        CHECK(strcmp(line, want) == 0);
        n++;
    }
    CHECK(n == 100);
    fclose(f);
    printf("text lines %d\n", n);

    /* The raw bytes: CR LF, then Ctrl-Z to the end of the last record. */
    f = fopen("text.txt", "rb");
    size_t got = fread(line, 1, 25, f);
    CHECK(got == 25 && line[23] == '\r' && line[24] == '\n');
    CHECK(fseek(f, 0, SEEK_END) == 0);
    long size = ftell(f);
    CHECK(size % 128 == 0);
    fclose(f);
    struct stat st;
    CHECK(stat("text.txt", &st) == 0 && st.st_size == size);
    printf("text size %ld\n", size);

    /* Appending. */
    f = fopen("text.txt", "a");
    fputs("appended\n", f);
    fclose(f);
    f = fopen("text.txt", "r");
    n = 0;
    while (fgets(line, sizeof(line), f))
        n++;
    CHECK(n == 101 && strcmp(line, "appended\n") == 0);
    fclose(f);

    /* Binary: every byte value, then random access. */
    int fd = open("bin.dat", O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);
    CHECK(fd >= 0);
    for (int i = 0; i < 1000; i++) {
        unsigned char c = (unsigned char)(i * 7);
        CHECK(write(fd, &c, 1) == 1);
    }
    CHECK(close(fd) == 0);
    fd = open("bin.dat", O_RDWR | O_BINARY);
    CHECK(fd >= 0);
    CHECK(lseek(fd, 0, SEEK_END) == 1024);
    unsigned char b[4];
    CHECK(lseek(fd, 517, SEEK_SET) == 517);
    CHECK(read(fd, b, 4) == 4);
    CHECK(b[0] == (unsigned char)(517 * 7) && b[3] == (unsigned char)(520 * 7));
    CHECK(lseek(fd, 300, SEEK_SET) == 300);
    CHECK(write(fd, "XYZ", 3) == 3);
    /* Past the end: a hole of zeros. */
    CHECK(lseek(fd, 20000, SEEK_SET) == 20000);
    CHECK(write(fd, "end", 3) == 3);
    CHECK(close(fd) == 0);
    fd = open("bin.dat", O_RDONLY | O_BINARY);
    CHECK(lseek(fd, 299, SEEK_SET) == 299);
    CHECK(read(fd, b, 4) == 4);
    CHECK(b[0] == (unsigned char)(299 * 7) && memcmp(b + 1, "XYZ", 3) == 0);
    CHECK(lseek(fd, 10000, SEEK_SET) == 10000);
    CHECK(read(fd, b, 4) == 4 && b[0] == 0 && b[3] == 0);
    CHECK(lseek(fd, 20000, SEEK_SET) == 20000);
    CHECK(read(fd, b, 3) == 3 && memcmp(b, "end", 3) == 0);
    close(fd);
    printf("binary ok\n");

    /* Rename, remove and missing files. */
    CHECK(rename("bin.dat", "p:moved.dat") < 0 && errno == EXDEV);
    CHECK(rename("bin.dat", "moved.dat") == 0);
    CHECK(access("bin.dat", F_OK) < 0 && errno == ENOENT);
    CHECK(access("moved.dat", F_OK) == 0);
    CHECK(remove("moved.dat") == 0);
    CHECK(fopen("moved.dat", "r") == NULL && errno == ENOENT);
    CHECK(open("bad*name", O_RDONLY) < 0);
    CHECK(remove("text.txt") == 0);
    printf("%s\n", failures ? "FAILED" : "files ok");
    return failures != 0;
}
