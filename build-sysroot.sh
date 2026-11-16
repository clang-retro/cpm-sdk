#!/bin/sh
# Build the i8086-cpm sysroot for CP/M-86: picolibc with its CP/M-86 layer
# and the compiler-rt builtins, for the tiny (8080) and small models.
#
#   BIN=... LLVM_SRC=... PICOLIBC_SRC=... ./build-sysroot.sh OUT
#
#   BIN           directory with clang, llvm-ar and the other LLVM tools
#                 (clang-retro/llvm, with the i8086-unknown-cpm target)
#   LLVM_SRC      clang-retro/llvm checkout (compiler-rt)
#   PICOLIBC_SRC  clang-retro/picolibc checkout
#   WORK          build directory (default: ./build-cpm)
#   MODELS        memory models (default: tiny small)
#
# OUT gets include/ (with <cpm.h>) and, for each model, lib/<model>/ with
# crt0.obj, libc.a and libclang_rt.builtins.a. clang finds the sysroot with
# --sysroot=OUT, or at <clang's directory>/../i8086-cpm. meson and ninja
# must be on PATH.
set -e
out=${1:?usage: build-sysroot.sh OUT}
: "${BIN:?set BIN}" "${LLVM_SRC:?set LLVM_SRC}" "${PICOLIBC_SRC:?set PICOLIBC_SRC}"
WORK=${WORK:-$PWD/build-cpm}
mkdir -p "$out/include" "$out/lib" "$WORK"
out=$(cd "$out" && pwd) WORK=$(cd "$WORK" && pwd)
PATH=$BIN:$PATH
export PATH

for m in ${MODELS:-tiny small}; do
  # picolibc, with the CP/M-86 layer in libc.a.
  rm -rf "$WORK/picolibc-$m" "$WORK/picolibc-install-$m"
  meson setup "$WORK/picolibc-$m" "$PICOLIBC_SRC" \
    --cross-file "$PICOLIBC_SRC/scripts/cross-clang-i8086-cpm-$m.txt" \
    -Dos-cpm86=true -Dmultilib=false -Dpicocrt=false -Dsemihost=false \
    -Dtests=false -Dposix-console=true -Dsingle-thread=true \
    -Dthread-local-storage=false -Dspecsdir=none \
    --prefix="$WORK/picolibc-install-$m" >"$WORK/picolibc-$m.log" 2>&1 ||
    { tail -20 "$WORK/picolibc-$m.log"; exit 1; }
  ninja -C "$WORK/picolibc-$m" install >>"$WORK/picolibc-$m.log" 2>&1 ||
    { grep -E 'error|FAILED' "$WORK/picolibc-$m.log" | head -20; exit 1; }
  mkdir -p "$out/lib/$m"
  cp "$WORK/picolibc-install-$m/lib/crt0.obj" \
     "$WORK/picolibc-install-$m/lib/libc.a" "$out/lib/$m/"
  cp -R "$WORK/picolibc-install-$m/include/." "$out/include/"

  # compiler-rt builtins: 32- and 64-bit arithmetic and soft float.
  # LLVM_RUNTIMES_BUILD=ON keeps CMake from looking for an LLVM package.
  cmake -G Ninja -S "$LLVM_SRC/compiler-rt/lib/builtins" -B "$WORK/builtins-$m" \
    -DLLVM_RUNTIMES_BUILD=ON \
    -DCMAKE_SYSTEM_NAME=Generic -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$BIN/clang" -DCMAKE_ASM_COMPILER="$BIN/clang" \
    -DCMAKE_C_COMPILER_TARGET=i8086-unknown-cpm \
    -DCMAKE_ASM_COMPILER_TARGET=i8086-unknown-cpm \
    -DCMAKE_C_FLAGS="-mcmodel=$m" -DCMAKE_ASM_FLAGS="-mcmodel=$m" \
    -DCMAKE_AR="$BIN/llvm-ar" -DCMAKE_RANLIB="$BIN/llvm-ranlib" \
    -DCMAKE_NM="$BIN/llvm-nm" \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON -DCOMPILER_RT_BAREMETAL_BUILD=ON \
    -DCOMPILER_RT_OS_DIR=cpm >"$WORK/builtins-$m.log" 2>&1 ||
    { tail -20 "$WORK/builtins-$m.log"; exit 1; }
  ninja -C "$WORK/builtins-$m" >>"$WORK/builtins-$m.log" 2>&1 ||
    { grep -E 'error|FAILED' "$WORK/builtins-$m.log" | head -20; exit 1; }
  cp "$WORK/builtins-$m/lib/cpm/libclang_rt.builtins-i8086.a" \
     "$out/lib/$m/libclang_rt.builtins.a"
done
# The libraries' licences: a program built with them carries their code.
mkdir -p "$out/share/licenses"
cp "$PICOLIBC_SRC/COPYING.picolibc" "$out/share/licenses/"
cp "$LLVM_SRC/compiler-rt/LICENSE.TXT" "$out/share/licenses/compiler-rt-LICENSE.TXT"
echo "sysroot written to $out"
