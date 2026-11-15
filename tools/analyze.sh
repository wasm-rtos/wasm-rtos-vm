#!/usr/bin/env bash
# The static analyzer over the engine in the builds that compile different
# code: any warning fails. What it cannot see for itself is stated with
# WM_ASSERT. Needs clang (cc, or $CLANG).
set -uo pipefail
cd "$(dirname "$0")/.."
CLANG="${CLANG:-cc}"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
found=0
for flags in "" "-DWM_PREDECODE=0" "-DWM_HAS_MMU=0" "-DWM_COMPUTED_GOTO=0" \
    "-DWM_HAS_VALIDATION=0 -DWM_HAS_I64=0 -DWM_HAS_REF_TYPES=0 -DWM_PREDECODE=0"; do
    for f in source/*.c; do
        # shellcheck disable=SC2086
        warnings="$(cd "$out" && "$CLANG" --analyze -std=gnu99 $flags -I "$OLDPWD/source" "$OLDPWD/$f" 2>&1 | grep 'warning:')"
        if [ -n "$warnings" ]; then
            echo "[$flags] $warnings"
            found=1
        fi
    done
done
exit $found
