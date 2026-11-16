#!/bin/sh
# Build the CP/M-80 sysroots, z80-cpm and i8080-cpm: picolibc with its
# CP/M-80 layer and the compiler-rt builtins, for the Z80 and the 8080.
#
#   BIN=... LLVM_SRC=... PICOLIBC_SRC=... ./build-sysroot-80.sh OUT
#
#   BIN           directory with clang, llvm-ar and the other LLVM tools
#                 (clang-retro/llvm, with the z80 and i8080 targets)
#   LLVM_SRC      clang-retro/llvm checkout (compiler-rt)
#   PICOLIBC_SRC  clang-retro/picolibc checkout
#   WORK          build directory (default: ./build-cpm80)
#   CPUS          processors (default: z80 i8080)
#
# OUT gets a sysroot for each processor, OUT/z80-cpm and OUT/i8080-cpm,
# each with include/ (with <cpm.h>) and lib/ with crt0.o, cpm80.ld, libc.a
# and libclang_rt.builtins.a. clang finds them with --sysroot, or at
# <clang's directory>/../z80-cpm and ../i8080-cpm, so OUT can be the
# directory that holds the toolchain's bin. meson and ninja must be on
# PATH.
set -e
out=${1:?usage: build-sysroot-80.sh OUT}
: "${BIN:?set BIN}" "${LLVM_SRC:?set LLVM_SRC}" "${PICOLIBC_SRC:?set PICOLIBC_SRC}"
WORK=${WORK:-$PWD/build-cpm80}
mkdir -p "$out" "$WORK"
out=$(cd "$out" && pwd) WORK=$(cd "$WORK" && pwd)
PATH=$BIN:$PATH
export PATH

for cpu in ${CPUS:-z80 i8080}; do
  root=$out/$cpu-cpm
  mkdir -p "$root/include" "$root/lib"

  # picolibc, with the CP/M-80 layer in libc.a. The exact float conversion
  # (Ryu) needs 64- and 128-bit arithmetic, much too big for a TPA.
  rm -rf "$WORK/picolibc-$cpu" "$WORK/picolibc-install-$cpu"
  meson setup "$WORK/picolibc-$cpu" "$PICOLIBC_SRC" \
    --cross-file "$PICOLIBC_SRC/scripts/cross-clang-$cpu-cpm.txt" \
    -Dos-cpm80=true -Dmultilib=false -Dpicocrt=false -Dsemihost=false \
    -Dtests=false -Dposix-console=true -Dsingle-thread=true \
    -Dthread-local-storage=false -Dspecsdir=none -Dio-float-exact=false \
    --prefix="$WORK/picolibc-install-$cpu" >"$WORK/picolibc-$cpu.log" 2>&1 ||
    { tail -20 "$WORK/picolibc-$cpu.log"; exit 1; }
  ninja -C "$WORK/picolibc-$cpu" install >>"$WORK/picolibc-$cpu.log" 2>&1 ||
    { grep -E 'error|FAILED' "$WORK/picolibc-$cpu.log" | head -20; exit 1; }
  for f in crt0.o cpm80.ld libc.a; do
    cp "$WORK/picolibc-install-$cpu/lib/$f" "$root/lib/"
  done
  cp -R "$WORK/picolibc-install-$cpu/include/." "$root/include/"

  # compiler-rt builtins: 16-bit multiplication and division, 32- and
  # 64-bit arithmetic and soft float. LLVM_RUNTIMES_BUILD=ON keeps CMake
  # from looking for an LLVM package.
  cmake -G Ninja -S "$LLVM_SRC/compiler-rt/lib/builtins" -B "$WORK/builtins-$cpu" \
    -DLLVM_RUNTIMES_BUILD=ON \
    -DCMAKE_SYSTEM_NAME=Generic -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DCMAKE_C_COMPILER="$BIN/clang" -DCMAKE_ASM_COMPILER="$BIN/clang" \
    -DCMAKE_C_COMPILER_TARGET=$cpu-unknown-cpm \
    -DCMAKE_ASM_COMPILER_TARGET=$cpu-unknown-cpm \
    -DCMAKE_AR="$BIN/llvm-ar" -DCMAKE_RANLIB="$BIN/llvm-ranlib" \
    -DCMAKE_NM="$BIN/llvm-nm" \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON -DCOMPILER_RT_BAREMETAL_BUILD=ON \
    -DCOMPILER_RT_OS_DIR=cpm >"$WORK/builtins-$cpu.log" 2>&1 ||
    { tail -20 "$WORK/builtins-$cpu.log"; exit 1; }
  ninja -C "$WORK/builtins-$cpu" >>"$WORK/builtins-$cpu.log" 2>&1 ||
    { grep -E 'error|FAILED' "$WORK/builtins-$cpu.log" | head -20; exit 1; }
  cp "$WORK/builtins-$cpu/lib/cpm/libclang_rt.builtins-$cpu.a" \
     "$root/lib/libclang_rt.builtins.a"
done
echo "sysroots written to $out"
