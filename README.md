# esp32-brother-ptouch

Drive a **Brother P-touch label printer** directly from an **ESP32-S3/S2 over
USB-OTG host** — no PC, no Raspberry Pi, no Bluetooth pairing. Plug the printer
into the module's USB-C, send it a bitmap, get a cut label.

Verified end-to-end on a **PT-P710BT** (VID `0x04F9` PID `0x20AF`) with 12 mm
TZe tape, driven by an ESP32-S3 (the board sources 5 V VBUS to the printer —
no external feed needed). Other PT-series raster printers (E550W, P750W, …)
speak the same protocol but are untested here.

## What's inside

| Layer | Files | Depends on |
|---|---|---|
| **Raster protocol core** | `ptouch_raster.{c,h}` | libc only — portable, host-testable |
| **USB-OTG host transport** | `ptouch_usb.{c,h}` | ESP-IDF USB Host Library + FreeRTOS |
| **Print API** | `ptouch_print.{c,h}` | the two above |

The protocol core builds the Brother raster command stream (PackBits
compression, print info, mode/margin, raster lines, status parsing) and is
validated **byte-exact** against the hardware-verified
[brother_pt](https://github.com/treideme/brother_pt) reference — see
`host/` for the 59-check oracle test suite (`bash host/build.sh`, clang +
ASan/UBSan, no hardware needed).

The transport layers in the lessons that only show up on real hardware:

- **Status reads retry past stale IN packets** — the printer queues unsolicited
  print-phase notifications; without the retry ~13 % of idle status polls fail.
- **A short-TTL last-good status cache** covers the mechanical print window
  (feed/cut outlives the USB transfer and the printer won't answer during it);
  a real unplug bypasses the cache immediately.
- **All I/O serialized on one mutex** — a status poll can never interleave with
  a raster stream mid-print.
- **Trailing pad before the cut** — with `margin(0)` the cutter otherwise
  shaves ~1 mm off the end of the label.

## ⚠️ The one big ESP32-S3/S2 caveat

The chip has **one USB PHY**, shared between USB-Serial-JTAG (your console +
flasher) and USB-OTG. Installing the USB host **takes the PHY**: your serial
console and `idf.py flash` stop working while the driver runs. Plan for it:
log to UART0 or the network, update over OTA, and know that recovery is
holding BOOT while plugging into a PC (with the printer unplugged).

## Quickstart (ESP-IDF)

```yaml
# main/idf_component.yml
dependencies:
  esp32-brother-ptouch:
    git: "https://github.com/jh247247/esp32-brother-ptouch.git"
    version: "v0.1.0"
```

```c
#include "ptouch_usb.h"
#include "ptouch_print.h"

void app_main(void)
{
    ptouch_usb_config_t cfg = PTOUCH_USB_CONFIG_DEFAULT();
    ptouch_usb_start(&cfg);
    // ... wait for PTOUCH_USB_EVT_ATTACHED (see examples/print_demo) ...

    ptouch_status_t st;
    ptouch_usb_status(&st);                     // tape width, errors, model

    uint8_t px[360 * 76];                       // 1 byte/px, non-zero = ink
    // width = along the tape, height = across it (<= 128 head dots;
    // 76 covers 12 mm tape's printable area at 180 dpi)
    ptouch_print_opts_t opts = PTOUCH_PRINT_OPTS_DEFAULT();
    opts.tape_mm = st.media_width_mm;
    ptouch_print_bitmap(px, 360, 76, &opts);    // prints + cuts
}
```

Full example: [`examples/print_demo`](examples/print_demo) (`idf.py set-target
esp32s3 && idf.py build flash`).

## PlatformIO / Arduino

The same sources build under the Arduino framework (the Arduino-ESP32 core
bundles the IDF USB Host Library on S2/S3). `library.json` is included:

```ini
lib_deps = https://github.com/jh247247/esp32-brother-ptouch.git
```

See [`examples/pio_arduino`](examples/pio_arduino).

## Rendering text?

This driver takes bitmaps — bring your own renderer. It was extracted from a
label-printer firmware that pairs it with an stb_truetype + qrcodegen renderer;
anything that can produce a 1-byte-per-pixel buffer works (LVGL canvas,
u8g2, your own font code).

## Status / roadmap

- [x] PT-P710BT over ESP32-S3 USB-OTG — print, cut, status, hot-plug
- [x] Host test suite (oracle-verified against brother_pt)
- [ ] Chain printing (`no cut between labels`) — command exists, untested
- [ ] Other PT models — protocol-identical on paper, reports welcome
- [ ] ESP Component Registry listing

## License

Apache-2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE) (protocol reference:
brother_pt, Apache-2.0; PackBits encoder ported from the MIT-licensed
`packbits` Python package).
