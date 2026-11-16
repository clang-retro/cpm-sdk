# cpm-sdk

An SDK for CP/M programs built with clang from
[clang-retro/llvm](https://github.com/clang-retro/llvm):

- CP/M-86 (CP/M-86 1.1 for the IBM PC and others, CP/M-86 Plus,
  Concurrent CP/M), with `--target=i8086-unknown-cpm`;
- CP/M-80 (CP/M 2.2, CP/M 3 and compatibles) on the Z80, with
  `--target=z80-unknown-cpm`, and on the 8080 and 8085, with
  `--target=i8080-unknown-cpm`. See [CP/M-80](#cpm-80).

For CP/M-86 it has:

- the C library: [picolibc](https://github.com/clang-retro/picolibc) with
  a CP/M-86 layer (startup code, the command line, `malloc`, files through
  FCBs, the console and the other CP/M devices), and the compiler-rt
  builtins, for the tiny (8080) and small memory models;
- `<cpm.h>`: `bdos()` for any BDOS call, and conio-style console functions;
- `cpm86run`, a CP/M-86 emulator for running programs in tests on Linux
  and macOS;
- scripts to run programs on Digital Research's CP/M-86 1.1 in QEMU;
- examples.

## Using it

    clang --target=i8086-unknown-cpm -Os hello.c -o hello.cmd
    clang --target=i8086-unknown-cpm -mcmodel=tiny -Os hello.c -o hello.cmd

The sysroot is `--sysroot=DIR`, or `i8086-cpm/` next to clang's `bin/`.
Build it with [`build-sysroot.sh`](build-sysroot.sh) (it needs meson,
ninja, and checkouts of clang-retro/llvm and clang-retro/picolibc).

The driver compiles to OMF objects, links them with JWlink as a DOS
`.COM` file (tiny) or `.EXE` file (small), and converts that to a `.CMD`
file with `llvm-exe2cmd`:

- **small** (the default): a code group of up to 64 KiB and a data group
  of up to 64 KiB holding the data, the BSS, the stack and the heap. The
  base page is at `DS:0000`.
- **tiny**: the 8080 model, one group of up to 64 KiB for everything, with
  `CS = DS = ES`, the base page at offset 0 and the program at `0100h`,
  just as in a `.COM` file.

CP/M-86 1.1 cannot relocate segment addresses in a program, so the other
memory models (and `__far` data with a segment fixed at link time) are not
available; `llvm-exe2cmd` refuses a program with segment relocations.
Code is for the 8086, which runs on every PC.

### C programs

- `main(argc, argv)` gets the command tail, split at spaces. CP/M has
  made it upper case already. `argv[0]` is `""`.
- The heap is the rest of the data group (64 KiB at most), as CP/M-86
  allocated it. `malloc` and `sbrk` work in it. The stack is 4 KiB.
- `exit(status)` returns to the CCP, and sets the program return code
  (BDOS 108) on CP/M-86 Plus: 0, or `FF00h | status`.
- Files: `fopen`, `open`, `read`, `write`, `lseek`, `stat`, `rename`,
  `remove`/`unlink` and `access` work on CP/M files (`B:NAME.TYP`, any
  case) through FCBs and random records. Text mode is the default
  (`_fmode = O_TEXT`): `\n` is written as CR LF, CR LF reads as `\n`, and
  `^Z` ends a text file. Open with `"rb"`/`"wb"` or `O_BINARY` for binary
  files. CP/M keeps sizes in 128-byte records, so a binary file's size is
  a multiple of 128.
- The device names `CON:`, `LST:` (or `PRN:`), `PUN:` (or `AUX:`, the
  auxiliary output), `RDR:` (the auxiliary input) and `NUL:` open the
  CP/M devices.
- `stdin` reads lines with BDOS function 10 (with CP/M's line editing); a
  line holding `^Z` is the end of the input. `stdout` and `stderr` are the
  console.
- `chdir("B:")` changes the default drive. There are no directories:
  `mkdir` fails with `ENOSYS`.
- `time` and `clock_gettime` read the clock on CP/M-86 Plus (BDOS 105);
  CP/M-86 1.1 has none, and they fail with `ENOSYS`.
- `<cpm.h>` declares `bdos(fn, dx)` (as in BDS C and Aztec C), the BDOS
  version, the base page, and `getch`, `getche`, `kbhit`, `putch` and
  `cputs`.
- C++ is compiled without exceptions and RTTI by default.

## Running programs

[`tools/cpm86run`](tools/cpm86run) runs `.CMD` programs on the host, with
the console and files in the current directory:

    make -C tools/cpm86run
    tools/cpm86run/cpm86run hello.cmd

[`tools/qemu/cpm86-qemu.py`](tools/qemu/cpm86-qemu.py) runs them on the
real CP/M-86 1.1 for the IBM PC, in QEMU. It makes a boot floppy with the
programs on it, types the commands at the `A>` prompt and prints the
screen:

    tools/qemu/cpm86-qemu.py --png hello.png hello.cmd -- dir hello

It needs `qemu-system-i386`, cpmtools and clang. CP/M-86 is not part of
this SDK: the script downloads CP/M-86 1.1 from
[the Unofficial CP/M Web Site](http://www.cpm.z80.de/), which may
distribute it, and a formatted boot floppy from
[tsupplis/cpm86-kernel](https://github.com/tsupplis/cpm86-kernel).

## Tests

    BIN=.../bin SYSROOT=.../i8086-cpm tests/run-tests.sh
    QEMU=1 BIN=.../bin SYSROOT=.../i8086-cpm tests/run-tests.sh

builds the tests in [`tests/`](tests) in both models and checks their
output in cpm86run, and with `QEMU=1` on CP/M-86 1.1 too.

## CP/M-80

    clang --target=z80-unknown-cpm -Os hello.c -o hello.com
    clang --target=i8080-unknown-cpm -Os hello.c -o hello.com

The sysroots are `z80-cpm/` and `i8080-cpm/` next to clang's `bin/` (or
`--sysroot=DIR`). [`build-sysroot-80.sh`](build-sysroot-80.sh) builds both
from the same checkouts. The C library is the same as for CP/M-86, with
picolibc's CP/M-80 startup code and BDOS call (`libos/cpm80`), so
everything under [C programs](#c-programs) holds, with these differences:

- clang compiles to ELF objects, links them with `ld.lld` at `0100h`
  (`cpm80.ld` in the sysroot: the code, the read-only data and the data,
  then the BSS) and writes the `.COM` file with `llvm-objcopy`. There are
  no memory models.
- The stack starts at the top of the TPA (the BDOS address at `0006h`);
  the heap is everything from the end of the BSS to 2 KiB below it.
- `exit(status)` sets the program return code on CP/M 3 (BDOS 108).
  `time` reads the clock on CP/M 3 and MP/M (BDOS 105); CP/M 2.2 has none.
- `<cpm.h>`'s `bdos(fn, de)` calls `0005h` with the function in C and the
  parameter in DE, and returns BA (HL).
- `int` is 16 bits, `long` 32 and `long long` 64; `float` and `double` are
  IEEE single and double, in software. The Z80 code uses IX as the frame
  pointer; the 8080 code runs on the Z80 too.
- Programs are big: a `printf("%d")` hello is about 17 KiB on the Z80, as
  the compiler calls the integer printf when the format allows. Printing
  `double`s with the full printf and `strtod` takes about 57 KiB on the
  Z80, and does not fit in 64 KiB on the 8080 yet.

[`tools/ntvcm`](tools/ntvcm) is David Lee's CP/M 2.2 emulator, with the
Z80 and (`-8`) the 8080:

    make -C tools/ntvcm
    tools/ntvcm/ntvcm hello.com
    tools/ntvcm/ntvcm -8 hello.com

and the tests run in it on both processors:

    BIN=.../bin SYSROOTS=... tests/run-tests-80.sh

## License

The SDK is under the GNU Affero General Public License v3 or later
([LICENSE](LICENSE)). The C library's CP/M-86 layer is in picolibc, under
its BSD license. The 8086 emulator in `tools/cpm86run` is from
[ntvdm](https://github.com/davidly/ntvdm), under CC0, `tools/ntvcm` is
[ntvcm](https://github.com/davidly/ntvcm), under CC0, and
`tools/qemu/chainboot.S` is CC0.
