# ntvcm

David Lee's CP/M 2.2 emulator, with Z80 and 8080 (`-8`) instruction sets,
from [ntvcm](https://github.com/davidly/ntvcm) at
3f20eb5f81731c13d0546fb6130aa3ad364c403d, unchanged, under CC0 ([LICENSE.ntvcm](LICENSE.ntvcm)). Only the
emulator's sources are here; the Makefile is this SDK's. The CP/M-80 tests
(`tests/run-tests-80.sh`) run in it.

    make
    ./ntvcm [-8] program.com [arguments]

It emulates the BDOS in C++, with files in the current directory, and
exits with the program's return code (BDOS function 108, which the C
library sets from `exit()`). Console input comes from standard input; a
line ends with CR, as typed, and ^Z is echoed as itself.
