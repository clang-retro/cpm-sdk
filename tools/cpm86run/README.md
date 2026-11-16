# cpm86run

Runs CP/M-86 `.CMD` programs on Linux, macOS (and, untested, Windows) for
tests: it loads the program as the CCP does (8080, small and compact
models), and emulates the BDOS: the console, and files in the current
directory through FCBs, including random access and directory searches.
There is no BIOS, disk or CCP, and no CP/M-86 code is used.

    make
    ./cpm86run [-t] [-e] [-m] program[.cmd] [arguments]

- `-e` exits with the program's return code (BDOS function 108, as CP/M-86
  Plus keeps it; the C library sets it from `exit()`), otherwise 0.
- `-m` prints the memory model and where the groups were loaded.
- `-t` traces every instruction and BDOS call to `cpm86run.log`.

Standard input is the console: lines typed with BDOS function 10 are
echoed, as CP/M does, so the output reads the same with input from a file.
The end of the input reads as a line holding `^Z`. CR is dropped from the
output.

The 8086 emulator (`i8086.cxx`, `i8086.hxx`, `djl8086d.hxx`, `djltrace.hxx`,
`djl_os.hxx`) is David Lee's, from [ntvdm](https://github.com/davidly/ntvdm)
at fa9ce5fca8815b27579b64bd0e2dd7beb609e407, unchanged, under CC0
([LICENSE.ntvdm](LICENSE.ntvdm)). `cpm86run.cxx` is this SDK's.
