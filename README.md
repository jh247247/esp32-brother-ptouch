# esp32-brother-ptouch

ESP-IDF component for driving Brother P-touch raster label printers from an
ESP32-S3 USB host. It builds model-specific raster streams, validates the
attached printer and tape, sends one complete job, and—on profiles with a
hardware-validated completion contract—waits for confirmed mechanical
completion without issuing forbidden status commands mid-print. Other profiles
return an unvalidated result and quarantine further output for inspection.

`0.2.0-beta.1` is a public beta. The API and the 23-profile recipe catalog are
ready for community testing; the support tier for each model is deliberately
more conservative than recipe availability.

## Evidence tiers

- **PT-P710BT:** hardware-validated print, cut, completion, hot-plug, and
  five-label batching.
- **PT-1950:** inspected single-label output, orientation, length, text, and
  automatic cut. Completion remains inspection-gated; chaining is unavailable.
- **21 additional models:** host-tested experimental recipes. They are refused
  by default and require explicit `allow_experimental_profiles` opt-in.

See [the complete model matrix](docs/MODEL_SUPPORT.md). Unknown devices,
P-Lite/Editor Lite identities, unsupported raster models, incompatible media,
and malformed USB descriptors fail closed.

## Supported build surface

The beta is tested on **ESP32-S3 with ESP-IDF 6.0.2** and is intended for
ESP-IDF 6.x. Earlier metadata advertised
Arduino, ESP32-S2, ESP32-P4, and ESP-IDF 5 without equivalent CI evidence; those
claims have been removed until they are proven.

## Install

Pin the protected GitHub release tag through ESP-IDF's Component Manager in
your project's `main/idf_component.yml`:

```yaml
dependencies:
  esp32-brother-ptouch:
    git: https://github.com/jh247247/esp32-brother-ptouch.git
    version: v0.2.0-beta.1
```

Do not use a moving branch for firmware builds. Commit the generated
`dependencies.lock` so every build resolves the same immutable source commit.

## Safe quickstart

```c
#include "ptouch_print.h"
#include "ptouch_usb.h"

static void on_event(ptouch_usb_event_t event,
                     const ptouch_usb_devinfo_t *info,
                     void *context)
{
    if (event == PTOUCH_USB_EVT_ATTACHED) {
        /* Wake your application task. Do not print from the callback. */
    }
}

void start_printer(void)
{
    ptouch_usb_config_t config = PTOUCH_USB_CONFIG_DEFAULT();
    config.on_event = on_event;
    ESP_ERROR_CHECK(ptouch_usb_start(&config));
}
```

After an attach event, obtain a fresh ready snapshot, render at its geometry,
initialize model-aware options, and inspect the structured result:

```c
ptouch_usb_ready_t ready;
if (!ptouch_usb_ready_snapshot(&ready)) {
    return; /* zero output */
}

ptouch_print_opts_t options;
if (!ptouch_print_opts_init(ready.target.profile,
                            ready.status.media_width_mm,
                            &options)) {
    return; /* zero output */
}

ptouch_usb_print_result_t result =
    ptouch_usb_print_bitmap(bitmap, width, height, &options, 60000);

if (result.transfer_rc == PTOUCH_USB_TRANSFER_OK &&
    result.completion_rc == PTOUCH_USB_COMPLETION_OK) {
    /* Confirmed current-label completion. */
} else if (result.stream_submitted) {
    /* The label may have printed. Never retry automatically. Inspect the
       printer and output before deliberately clearing quarantine. */
} else {
    /* Safe rejection before any raster transfer was accepted. */
}
```

The complete buildable example is in [`examples/print_demo`](examples/print_demo).
See [API.md](docs/API.md) for lifecycle, completion, and quarantine contracts.

## Experimental hardware tests

Recipe-derived profiles are not silently enabled. A supervised test must opt in
before enumeration:

```c
ptouch_usb_config_t config = PTOUCH_USB_CONFIG_DEFAULT();
config.allow_experimental_profiles = true;
ESP_ERROR_CHECK(ptouch_usb_start(&config));
```

Only print expendable test content. Treat every result with
`stream_submitted=true` as potentially printed, even when the return code is an
error. Submit hardware evidence using the template in [CONTRIBUTING.md](CONTRIBUTING.md).

## USB and electrical requirements

- The component owns ESP-IDF's USB Host Library for the rest of the boot. It is
  incompatible with another already-installed host stack.
- ESP32-S3 has one USB PHY. Starting host mode takes it away from
  USB-Serial-JTAG, so the serial console and normal USB flashing disappear.
  Provide UART/network logging and an OTA or BOOT-mode recovery path first.
- The board or host-port circuit—not the ESP32-S3 chip alone—must provide a
  correctly current-limited 5 V VBUS session. Follow the board and printer
  electrical specifications; do not back-feed either device.
- The beta supports one directly attached printer, one claimed interface, and
  no USB hub or multi-printer routing.
- `ptouch_usb_start()` is single-shot. A terminal transport fault requires a
  reboot; it cannot be cleared in software.

## Test

```bash
bash host/build.sh
```

The sanitizer-backed host gate runs the 59-check byte oracle, completion state
machine tests, descriptor rejection tests, exact P710 regression streams, and a
valid job build for every available model recipe. CI builds the ESP-IDF example
on the pinned ESP-IDF 6.0.2 image; other 6.x releases are intended compatibility,
not yet an exhaustive tested matrix.

## License and provenance

Apache-2.0. See [LICENSE](LICENSE), [NOTICE](NOTICE),
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and
[PROVENANCE.md](docs/PROVENANCE.md).
