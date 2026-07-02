#!/usr/bin/env bash
#
# Host build + test runner for the portable P-touch raster protocol core.
# Compiles with clang (strict warnings + UBSan/ASan) and runs the test binary,
# which prints per-check PASS/FAIL and a final summary. Exit code mirrors tests.
#
# Regenerate the oracle header first (requires the golden venv's `packbits`):
#   python3 gen_oracle.py <path-to-treideme/brother_pt-checkout> oracle_generated.h
#   (needs `pip install packbits`; the generated header is committed, so this is
#   only required when changing the protocol builders)
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/build"
mkdir -p "$OUT"

CC="${CC:-clang}"
CFLAGS=(
    -std=c11
    -Wall -Wextra -Werror
    -Wshadow -Wconversion -Wsign-conversion
    -O2 -g
    -fsanitize=address,undefined
    -fno-omit-frame-pointer
    -I"$HERE/../include"
)

if [[ ! -f "$HERE/oracle_generated.h" ]]; then
    echo "ERROR: oracle_generated.h missing — run gen_oracle.py first." >&2
    exit 2
fi

echo "== compiling =="
"$CC" "${CFLAGS[@]}" \
    "$HERE/../src/ptouch_raster.c" \
    "$HERE/test_ptouch.c" \
    -o "$OUT/test_ptouch"

echo "== running =="
"$OUT/test_ptouch"
