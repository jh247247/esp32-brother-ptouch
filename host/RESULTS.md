# PT-P710BT raster protocol core — host validation results

Portable, device-independent Brother P-touch raster protocol logic (PackBits
compression + command-stream builders + 32-byte status parsing) for the
PT-P710BT. **No ESP-IDF / hardware dependencies.** The USB *transport* (esp-idf
`usb_host` bulk in/out) is intentionally **not** implemented here — see
`ptouch_driver.h` for the interface the ESP-IDF layer will fill in later.

## Files

| File | Purpose |
|------|---------|
| `ptouch_raster.h` / `.c` | Portable core: growable buffer, PackBits enc/dec, command builders, `parse_status`. |
| `ptouch_driver.h` | USB-host driver **interface only** (opaque handle, transport vtable `{bulk_out,bulk_in}`, `ptouch_open/close/print_bitmap/read_status`). Transport implemented later on ESP-IDF. |
| `host/test_ptouch.c` | Host test suite (round-trip, oracle vectors, byte-layout, status, full-stream). |
| `host/gen_oracle.py` | Generates `oracle_generated.h` from the reference impls. |
| `host/oracle_generated.h` | Auto-generated oracle vectors (committed so tests build without the venv). |
| `host/build.sh` | clang build (`-Werror -Wconversion` + ASan/UBSan) + test runner. |

## Oracle used

Byte-for-byte oracle from the authoritative open-source reference stack that
drives **real PT-P710BT hardware**:

- **`packbits` PyPI 0.6** — the exact PackBits (TIFF) implementation
  `treideme/brother_pt` calls for the `G` raster transfer. Our
  `ptouch_packbits_encode` is a direct port of its state machine, so encoded
  bytes match exactly (including its 1-byte-input and run-boundary quirks).
- **`treideme/brother_pt` `cmd.py`** — pure command builders (no USB). We
  concatenate its `invalidate → init → status_request → raster_mode →
  status_notification → print_information → mode → advanced_mode → margin →
  compression → G/Z raster lines → print` output for a 24 mm, 3-line test raster
  (line 0 empty → `Z`) and diff our full stream against it.

`gen_oracle.py` needs Python 3 with `packbits` installed (`pip install packbits`),
pointed at a shallow clone of `brother_pt`. No hardware, no USB. The generated
`oracle_generated.h` is committed, so this only matters when changing builders.

## Results

```
$ bash host/build.sh
PASS: 59  FAIL: 0
```

Compiled clang `-std=c11 -Wall -Wextra -Werror -Wshadow -Wconversion
-Wsign-conversion -O2 -fsanitize=address,undefined`. Clean — no sanitizer
reports.

Coverage:

- **PackBits round-trip** — `decode(encode(x)) == x` over 2000 deterministic
  pseudo-random + structured (all-0x00 / all-0xFF / alternating) buffers up to
  300 bytes, plus explicit empty / 1-byte / 2-byte edge cases.
- **PackBits oracle** — 9 known vectors incl. the classic Wikipedia/TIFF
  example, all-same runs, single byte (`00 5A`), 2-byte run (`FF 42`),
  alternating, a raster-like line, a 129-byte run crossing the 128 boundary,
  and empty. Both our `encode` == oracle bytes **and** our `decode(oracle)` ==
  input.
- **Command byte-layout** — each builder asserted against exact spec bytes
  (invalidate, init, status_request, raster_mode, status_notification,
  print_info incl. little-endian line count, mode, advanced_mode, margin incl.
  LE, compression, `Z` empty line, `G` line, print feed/chain).
- **`parse_status`** — synthetic 32-byte reply (24 mm laminated, no-media +
  cover-open errors, big-endian phase number, colors), a clean 12 mm reply, a
  bad-header reply (fields still populated, `valid=false`), and NULL guard;
  plus `ptouch_status_error_string` → `"no media|cover open"`.
- **Full-stream oracle** — our complete 172-byte print-job stream is
  **byte-identical** to the `brother_pt` reference output.

## Spec-confirmed vs. needs-hardware-validation

**Confirmed byte-exact against the hardware-verified `brother_pt` reference**
(these bytes are what a working PT-P710BT is driven with today):

- invalidate (100×`00`), init (`1B 40`), status request (`1B 69 53`)
- raster mode (`1B 69 61 01`), status notification (`1B 69 21 00`)
- print information (`1B 69 7A 84 00 <w> 00 <lines LE32> 00 00`)
- mode (`1B 69 4D 40`), advanced mode (`1B 69 4B 08`), margin (`1B 69 64 <LE16>`)
- compression select (`4D 02`), `G`/`Z` raster lines with PackBits, print+feed (`1A`)
- status offsets 8/9/10/11/15/17/18/19/20-21/22/24/25/26 (== brother_pt `StatusOffsets`)

**Needs hardware validation (Phase 0/1) — from the Brother spec but NOT
exercised by the `brother_pt` reference:**

1. `PTOUCH_PRINT_CHAIN = 0x0C` (print, no-feed / chain). `brother_pt` only ever
   emits `0x1A`. `0x0C` is per the raster reference but unproven on-device.
2. Semantic interpretation (not bytes) of `print_info` n1 flags (`0x84`) and
   `advanced_mode` bit meanings — the bytes match the reference, the field
   meanings are spec-derived.
3. Status **model code** at byte 4 — spec-derived offset; `brother_pt` does not
   read it (it uses width/type/colors), so the exact model code value for a
   PT-P710BT is not confirmed here.
4. Everything above the byte layer — tape-margin padding, bitmap rotation to the
   print head, and the print/ack handshake loop — lives in the (unimplemented)
   `ptouch_driver` layer and will be validated when the ESP-IDF transport lands.
