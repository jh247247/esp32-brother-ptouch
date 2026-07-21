# Changelog

## 0.2.0-beta.1 - 2026-07-22

- Add a fail-closed catalog of 23 Brother P-touch raster recipes.
- Hardware-validate PT-P710BT completion/batching and inspected PT-1950 output/cut.
- Add native 180/360 dpi geometry, per-model commands, framed transfers, and
  profile-aware defaults.
- Add fresh media/model preflight, attachment generations, completion tracking,
  output quarantine, and ambiguous-output reporting.
- Add explicit opt-in for 21 recipe-derived experimental profiles.
- Add structured `ptouch_usb_print_bitmap()` results and deprecate flattened
  high-level output.
- Dispatch callbacks outside the USB task/I/O lock, enforce single start, bound
  timeout reclamation, synchronize safety flags, and gate deprecated raw output.
- Move all model/completion tests and public documentation into this repository.
- Narrow the build surface to ESP32-S3, tested on ESP-IDF 6.0.2 and intended
  for ESP-IDF 6.x.
- Include the full MIT notice for the PackBits-derived encoder state machine.

## 0.1.2

- Center short PT-P710BT labels within the mechanical minimum.
