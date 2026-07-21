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
    "-fsanitize=address,undefined"
    -fno-omit-frame-pointer
    -I"$HERE/../include"
)

if [[ ! -f "$HERE/oracle_generated.h" ]]; then
    echo "ERROR: oracle_generated.h missing — run gen_oracle.py first." >&2
    exit 2
fi

echo "== compiling raster oracle =="
"$CC" "${CFLAGS[@]}" \
    "$HERE/../src/ptouch_raster.c" \
    "$HERE/test_ptouch.c" \
    -o "$OUT/test_ptouch"

echo "== compiling model/profile suite =="
"$CC" "${CFLAGS[@]}" \
    -DPTOUCH_HOST_BUILD \
    "$HERE/../src/ptouch_model.c" \
    "$HERE/../src/ptouch_usb_descriptor.c" \
    "$HERE/../src/ptouch_raster.c" \
    "$HERE/../src/ptouch_print.c" \
    "$HERE/test_models.c" \
    -o "$OUT/test_models"

echo "== compiling completion suite =="
"$CC" "${CFLAGS[@]}" \
    "$HERE/../src/ptouch_completion.c" \
    "$HERE/../src/ptouch_raster.c" \
    "$HERE/test_completion.c" \
    -o "$OUT/test_completion"

echo "== running raster oracle =="
"$OUT/test_ptouch"

echo "== running model/profile suite =="
ASAN_OPTIONS="detect_leaks=0:halt_on_error=1" \
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
    "$OUT/test_models"

echo "== running completion suite =="
ASAN_OPTIONS="detect_leaks=0:halt_on_error=1" \
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
    "$OUT/test_completion"

echo "== running USB source-contract suite =="
python3 "$HERE/test_usb_source_contract.py"
