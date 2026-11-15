#!/usr/bin/env bash
# Builds build/coremark-wm.wasm: CoreMark (EEMBC, Apache-2.0) with the bare
# port of wasm3-coremark (imports env.clock_ms, exports run), both fetched at
# the commits below, unmodified, for WebAssembly 2.0 as this engine runs it.
#   tools/coremark.sh
# CC_WASM: a clang for wasm32-wasi; default $WASI_SDK/bin/clang, or
# Homebrew's llvm with wasi-libc.
set -euo pipefail
cd "$(dirname "$0")/.."
COREMARK_REF=1f483d5b8316753a742cbf5590caf5bd0a4e4777
PORT_REF=3935b1b42ffbaec4d80a623830c078caa3d69767
FEATURES="-mcpu=mvp -mmutable-globals -mmultivalue -msign-ext -mnontrapping-fptoint -mbulk-memory"
if [ -z "${CC_WASM:-}" ]; then
    if [ -n "${WASI_SDK:-}" ]; then
        CC_WASM="$WASI_SDK/bin/clang"
    else
        CC_WASM="/opt/homebrew/opt/llvm/bin/clang --target=wasm32-wasi --sysroot=/opt/homebrew/share/wasi-sysroot"
    fi
fi
src=build/coremark-src
fetch() { # repository commit directory
    rm -rf "$3"
    git init -q "$3"
    git -C "$3" fetch -q --depth 1 "$1" "$2"
    git -C "$3" checkout -q FETCH_HEAD
}
mkdir -p build
fetch https://github.com/eembc/coremark.git "$COREMARK_REF" "$src/coremark"
fetch https://github.com/wasm3/wasm3-coremark.git "$PORT_REF" "$src/port"
cp -r "$src/port/ports/minimal" "$src/coremark/minimal"
patch -s -d "$src/coremark" -p1 < "$src/port/ports/minimal/core_main-report-score.patch"
make -s -C "$src/coremark" compile PORT_DIR=minimal CC="$CC_WASM" PORT_CFLAGS="-O3 $FEATURES" EXE=.wasm
cp "$src/coremark/coremark.wasm" build/coremark-wm.wasm
echo "build/coremark-wm.wasm"
