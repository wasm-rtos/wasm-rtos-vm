#!/usr/bin/env bash
# Flash and RAM of the engine on AVR: a minimal program that loads a module,
# links one host function and calls an export. Extra arguments go to avr-gcc.
#   tools/avr-size.sh [mcu] [flags...]
set -euo pipefail
cd "$(dirname "$0")/.."
AVR_BIN="${AVR_BIN:-$HOME/.platformio/packages/toolchain-atmelavr/bin}"
mcu="${1:-atmega328p}"
shift || true
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
"$AVR_BIN/avr-gcc" -mmcu="$mcu" -Os -std=gnu99 -ffunction-sections -fdata-sections -Isource "$@" \
    -Wl,--gc-sections tools/avr/size-main.c source/wm_*.c -o "$out/wm.elf"
"$AVR_BIN/avr-size" "$out/wm.elf" | awk 'NR == 2 { printf "flash %d  ram %d (data %d, bss %d)\n", $1 + $2, $2 + $3, $2, $3 }'
if [ -n "${SYMBOLS:-}" ]; then
    "$AVR_BIN/avr-nm" --size-sort -S -t d "$out/wm.elf" | tail -"$SYMBOLS" | awk '{ print $2, $3, $4 }'
fi
