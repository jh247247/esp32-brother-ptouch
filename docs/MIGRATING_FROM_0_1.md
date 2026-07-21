# Migrating from 0.1

`0.2` preserves the PT-P710BT raster byte stream but changes the transport,
result, callback, build, and support contracts.

## Printing and results

New code should replace `ptouch_print_bitmap()` with
`ptouch_usb_print_bitmap()` and retain the complete
`ptouch_usb_print_result_t`. The structured API requires non-NULL options;
initialize them with `ptouch_print_opts_init(profile, tape_mm, &options)`.

The deprecated `ptouch_print_bitmap()` compatibility wrapper now waits for
mechanical completion. Its return value is therefore no longer only the bulk
OUT result. A non-zero return after output began may still describe a physically
printed label, which is why new code must use the structured result.

Never retry automatically when `stream_submitted` is true. A non-OK completion
may still describe a physically printed label. Handle
`PTOUCH_USB_COMPLETION_UNVALIDATED` with physical inspection and a deliberate
quarantine-clear flow.

Raw `ptouch_usb_bulk_out()` is deprecated for print data and now enforces
quarantine and terminal transport faults. Use the complete-job APIs for raster
output.

## Callbacks and lifecycle

Event callbacks now run on a dedicated callback task instead of the USB client
task, outside the I/O mutex. They may query snapshots, but should remain bounded
and must hand printing to an application task. Handle the new
`PTOUCH_USB_EVT_UNSUPPORTED` and `PTOUCH_USB_EVT_ATTACH_FAILED` cases. Events are
edge notifications and may be dropped if a callback does not keep up; device
and readiness snapshots are authoritative.

`ptouch_usb_start()` is single-shot. A terminal transport fault requires a
reboot.

## Models and build surface

Use `ptouch_model_has_print_recipe()` for side-effect-free offline construction
and `ptouch_model_can_print()` for hardware enabled by default. Recipe-derived
models require `allow_experimental_profiles = true`; the default fails closed.

The beta is tested on ESP32-S3 with ESP-IDF 6.0.2 and intended for ESP-IDF 6.x.
The earlier ESP32-S2, ESP32-P4, ESP-IDF 5, and Arduino/PlatformIO claims were
removed because they did not have equivalent CI evidence.
