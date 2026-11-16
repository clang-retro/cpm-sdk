#!/bin/sh
# Build the tests in tests/ for the Z80 and the 8080 and run them in ntvcm,
# comparing their output with tests/<name>.out (or <name>-80.out).
#
#   BIN=... SYSROOTS=... tests/run-tests-80.sh [CPU...]
#
#   BIN       directory with clang (z80-unknown-cpm, i8080-unknown-cpm),
#             ld.lld and llvm-objcopy
#   SYSROOTS  the directory with the z80-cpm and i8080-cpm sysroots
#             (build-sysroot-80.sh)
#   NTVCM     the emulator (default: tools/ntvcm/ntvcm, built here)
#   CLANG_FLAGS  more flags for clang
#   OUT       where the programs go (default: ./tests-out)
#
# CPU is z80 or i8080 (ntvcm -8); both by default. ntvcm reads the console
# from standard input with lines ending in CR, as typed, and echoes ^Z as
# itself, so the input's LFs become CRs, a ^Z line ends it, and the ^Z in
# the output is shown as CP/M shows it.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
: "${BIN:?set BIN}" "${SYSROOTS:?set SYSROOTS}"
OUT=${OUT:-$PWD/tests-out}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
if [ -z "$NTVCM" ]; then
  make -s -C "$root/tools/ntvcm"
  NTVCM=$root/tools/ntvcm/ntvcm
fi
cpus=${*:-z80 i8080}

# name, arguments (+ for a space), exit status
tests="args:one+two:7 files::0 memory::0 stdin::0 bdos:filename:0"

fail=0
for cpu in $cpus; do
  flag=
  [ "$cpu" = i8080 ] && flag=-8
  for t in $tests; do
    name=${t%%:*} rest=${t#*:}
    args=$(echo "${rest%:*}" | tr + ' ') status=${rest##*:}
    prog=$OUT/$name-$cpu.com
    if ! "$BIN/clang" --target=$cpu-unknown-cpm \
        --sysroot="$SYSROOTS/$cpu-cpm" -Os -Wall -Werror $CLANG_FLAGS \
        "$here/$name.c" -o "$prog"; then
      echo "FAIL $name ($cpu): build"
      fail=1
      continue
    fi
    dir=$OUT/run-$name-$cpu
    rm -rf "$dir"
    mkdir -p "$dir"
    cp "$prog" "$dir/$name.com"
    if [ -f "$here/$name.in" ]; then
      { tr '\n' '\r' <"$here/$name.in"; printf '\032\r'; } >"$dir/input"
    else
      : >"$dir/input"
    fi
    expect=$here/$name.out
    [ -f "$here/$name-80.out" ] && expect=$here/$name-80.out
    set +e
    (cd "$dir" && "$NTVCM" $flag "$name.com" $args <input >raw)
    got=$?
    set -e
    tr -d '\r' <"$dir/raw" | sed 's/\x1a/^Z/g' >"$dir/stdout"
    if [ "$got" != "$status" ]; then
      echo "FAIL $name ($cpu): exit status $got, expected $status"
      cat "$dir/stdout"
      fail=1
    elif ! diff -u "$expect" "$dir/stdout"; then
      echo "FAIL $name ($cpu): output"
      fail=1
    else
      echo "ok   $name ($cpu)"
    fi
  done
done
exit $fail
