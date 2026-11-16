#!/bin/sh
# Build the tests in tests/ for each memory model and run them in
# cpm86run, comparing their output with tests/<name>.out.
#
#   BIN=... SYSROOT=... tests/run-tests.sh [MODEL...]
#
#   BIN       directory with clang (i8086-unknown-cpm), jwlink and
#             llvm-exe2cmd
#   SYSROOT   the CP/M-86 sysroot (build-sysroot.sh)
#   CPM86RUN  the emulator (default: tools/cpm86run/cpm86run, built here)
#   CLANG_FLAGS  more flags for clang
#   QEMU=1    also run them on CP/M-86 1.1 in QEMU (tools/qemu)
#   OUT       where the programs go (default: ./tests-out)
#
# MODEL is tiny (8080 model) or small; both by default.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
: "${BIN:?set BIN}" "${SYSROOT:?set SYSROOT}"
OUT=${OUT:-$PWD/tests-out}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
if [ -z "$CPM86RUN" ]; then
  make -s -C "$root/tools/cpm86run"
  CPM86RUN=$root/tools/cpm86run/cpm86run
fi
models=${*:-tiny small}

# name, arguments (+ for a space), exit status
tests="args:one+two:7 files::0 memory::0 stdin::0 bdos:filename:0"

fail=0
for m in $models; do
  for t in $tests; do
    name=${t%%:*} rest=${t#*:}
    args=$(echo "${rest%:*}" | tr + ' ') status=${rest##*:}
    prog=$OUT/$name-$m.cmd
    if ! "$BIN/clang" --target=i8086-unknown-cpm -mcmodel=$m \
        --sysroot="$SYSROOT" -Os -Wall -Werror $CLANG_FLAGS \
        "$here/$name.c" -o "$prog"; then
      echo "FAIL $name ($m): build"
      fail=1
      continue
    fi
    dir=$OUT/run-$name-$m
    rm -rf "$dir"
    mkdir -p "$dir"
    input=/dev/null
    [ -f "$here/$name.in" ] && input=$here/$name.in
    set +e
    (cd "$dir" && "$CPM86RUN" -e "$prog" $args <"$input" >"$dir/stdout")
    got=$?
    set -e
    if [ "$got" != "$status" ]; then
      echo "FAIL $name ($m): exit status $got, expected $status"
      cat "$dir/stdout"
      fail=1
    elif ! diff -u "$here/$name.out" "$dir/stdout"; then
      echo "FAIL $name ($m): output"
      fail=1
    else
      echo "ok   $name ($m)"
    fi
  done
done

# The same programs on Digital Research's CP/M-86 1.1, in QEMU, one boot
# for each. The test that reads typed lines is left out. Each line of the
# expected output must be on the screen.
if [ -n "$QEMU" ]; then
  for m in $models; do
    for t in $tests; do
      name=${t%%:*} rest=${t#*:}
      args=$(echo "${rest%:*}" | tr + ' ')
      [ "$name" = stdin ] && continue
      [ -f "$OUT/$name-$m.cmd" ] || continue
      dir=$OUT/qemu-$name-$m
      rm -rf "$dir"
      mkdir -p "$dir"
      cp "$OUT/$name-$m.cmd" "$dir/$name.cmd"
      if ! python3 "$root/tools/qemu/cpm86-qemu.py" --png "$dir/screen.png" \
          "$dir/$name.cmd" -- "$name $args" >"$dir/screen.txt"; then
        echo "FAIL $name ($m, CP/M-86 1.1): QEMU"
        fail=1
        continue
      fi
      missing=
      while IFS= read -r line; do
        grep -qxF -- "$line" "$dir/screen.txt" || missing="$missing [$line]"
      done <"$here/$name.out"
      if [ -n "$missing" ]; then
        echo "FAIL $name ($m, CP/M-86 1.1): not on the screen:$missing"
        cat "$dir/screen.txt"
        fail=1
      else
        echo "ok   $name ($m, CP/M-86 1.1)"
      fi
    done
  done
fi
exit $fail
