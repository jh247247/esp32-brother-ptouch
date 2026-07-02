/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Jack Hosemans
 *
 * Arduino-framework version of the print demo. Same driver, same flow:
 * start USB host → wait for the printer → read status → print a test label.
 *
 * NOTE: once the USB host installs, the USB-CDC/Serial-JTAG console is GONE
 * (shared PHY on the S2/S3) — Serial output stops there unless your board
 * routes Serial to a separate UART bridge chip.
 */
#include <Arduino.h>
#include "ptouch_usb.h"
#include "ptouch_print.h"

static volatile bool s_attached = false;

static void on_usb_event(ptouch_usb_event_t evt,
                         const ptouch_usb_devinfo_t *info, void *ctx)
{
    if (evt == PTOUCH_USB_EVT_ATTACHED) {
        Serial.printf("printer attached: VID %04X PID %04X\n", info->vid, info->pid);
        s_attached = true;
    } else if (evt == PTOUCH_USB_EVT_DETACHED) {
        s_attached = false;
    }
}

static void make_test_pattern(uint8_t *px, int w, int h)
{
    memset(px, 0, (size_t)w * h);
    for (int x = 0; x < w; x++) { px[x] = 1; px[(size_t)(h - 1) * w + x] = 1; }
    for (int y = 0; y < h; y++) { px[(size_t)y * w] = 1; px[(size_t)y * w + w - 1] = 1; }
    for (int y = 2; y < h - 2; y++)
        for (int x = 2; x < w - 2; x++)
            if (((x + y) % 12) < 2) px[(size_t)y * w + x] = 1;
}

static uint8_t px[360 * 76];
static bool printed = false;

void setup()
{
    Serial.begin(115200);
    delay(1000);
    Serial.println("starting USB host (serial console may stop here)");

    ptouch_usb_config_t cfg = PTOUCH_USB_CONFIG_DEFAULT();
    cfg.on_event = on_usb_event;
    ptouch_usb_start(&cfg);
}

void loop()
{
    if (s_attached && !printed) {
        delay(500);
        ptouch_status_t st;
        int tape_mm = 12;
        if (ptouch_usb_status(&st) && st.media_width_mm > 0)
            tape_mm = st.media_width_mm;

        make_test_pattern(px, 360, 76);
        ptouch_print_opts_t opts = PTOUCH_PRINT_OPTS_DEFAULT();
        opts.tape_mm = tape_mm;
        int rc = ptouch_print_bitmap(px, 360, 76, &opts);
        printed = (rc == 0);
    }
    delay(250);
}
