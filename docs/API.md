# API and safety contract

## Lifecycle

`ptouch_usb_start()` installs and owns the ESP-IDF USB Host Library, creates the
client/event tasks, and begins enumeration. It is intentionally single-shot:
the first call starts the driver; later calls return `ESP_ERR_INVALID_STATE`.
There is no stop/reinstall operation in the `0.2` beta.

Events are copied to a dedicated callback task. The callback runs outside the
USB I/O mutex and may query device/target snapshots. Keep callbacks bounded and
move rendering or printing to an application task. Events are edge
notifications and may be dropped if a callback does not keep up; snapshots are
the authoritative device state.

## Model policy

`ptouch_model_has_print_recipe()` means a side-effect-free byte stream can be
built. `ptouch_model_can_print()` means the profile is enabled by the default
evidence policy. `ptouch_model_output_allowed(profile, true)` additionally
admits experimental recipes.

Physical output for experimental recipes requires
`ptouch_usb_config_t.allow_experimental_profiles = true`. Unknown,
incompatible, and P-Lite identities cannot be enabled by this flag.

## Building a job

Use `ptouch_print_opts_init(profile, tape_mm, &options)`. It applies the
profile's trailing padding, mechanical minimum, auto-cut behavior, and timeout.
`PTOUCH_PRINT_OPTS_DEFAULT()` remains only for byte-compatible PT-P710BT `0.1`
callers.

The bitmap is row-major, one byte per pixel, with non-zero pixels printed. Its
width runs along the tape; its height runs across the print head at the
profile's native DPI. Dimensions are checked before indexing, and height must
fit `ptouch_media_geometry_t.printable_dots`.

## Printing and interpreting results

Prefer `ptouch_usb_print_bitmap()` or `ptouch_usb_print_job_and_wait()`.
`ptouch_usb_print_bitmap()` requires options initialized for the detected
profile and tape; it does not guess a 12 mm default.
Both return `ptouch_usb_print_result_t`:

- `stream_submitted == false`: no raster OUT transfer was accepted; a retry may
  be safe after correcting the reported preflight/build problem.
- `stream_submitted == true` and both result enums are `OK`: the current label's
  validated completion contract succeeded.
- `stream_submitted == true` with any non-OK result: the label may have printed.
  Do not retry automatically. The transport latches output quarantine.
- `PTOUCH_USB_COMPLETION_UNVALIDATED`: the printer emitted its nominal status,
  but this model's completion behavior has not been hardware-validated. Inspect
  the physical output before clearing quarantine.

`ptouch_print_bitmap()` and `ptouch_usb_print_and_wait()` remain deprecated
compatibility APIs. The former flattens physically important state and must not
be used in new code.

## Quarantine and status commands

Status traffic can cancel or confuse a Brother printer during mechanical
completion. A print therefore owns the I/O lease from fresh idle preflight
through the final completion notification.

Ambiguous output latches quarantine. While quarantined, ordinary status and all
raw/output paths are blocked. After physical inspection, a caller may use the
purpose-bound `ptouch_usb_status_txn_after_inspection()` to prove fresh idle
state, then call `ptouch_usb_clear_output_quarantine_after_inspection()`.
Clearing quarantine is a deliberate application workflow; it is never automatic.

If a timed-out ESP-IDF transfer callback does not arrive within the bounded
reclaim window, the driver retains that one in-flight allocation and enters a
terminal transport fault. No further command is accepted. Reboot before reuse.

## Concurrency

All device access is serialized. Attachment generations bind a built job to the
exact USB device that supplied its status. Unplug/replug or target changes reject
the old job before output. One direct printer is supported; hubs and concurrent
printers are outside the beta contract.
