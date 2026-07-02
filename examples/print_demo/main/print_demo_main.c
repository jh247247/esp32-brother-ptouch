/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Jack Hosemans
 *
 * print_demo — plug a Brother PT-P710BT into the ESP32-S3's USB-C, get one
 * test-pattern label. Demonstrates the full flow: start the USB host, wait
 * for the printer, read its status (tape width), print, report.
 *
 * NOTE: once the USB host installs, the USB-Serial-JTAG console is GONE
 * (shared PHY). This demo logs over the default console until that moment;
 * after it you're flying blind unless you route logs to UART0
 * (CONFIG_ESP_CONSOLE_UART0) or a network sink. Reflashing needs the BOOT
 * button while the printer is unplugged.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "ptouch_usb.h"
#include "ptouch_print.h"

static const char *TAG = "print_demo";

/* Draw a test pattern into a w*h 1-byte-per-pixel bitmap: border, diagonal
 * stripes, and a solid block — enough to eyeball alignment and coverage. */
static void make_test_pattern(uint8_t *px, int w, int h)
{
    memset(px, 0, (size_t)w * (size_t)h);
    for (int x = 0; x < w; x++) {
        px[x] = 1;                        /* top edge    */
        px[(size_t)(h - 1) * w + x] = 1;  /* bottom edge */
    }
    for (int y = 0; y < h; y++) {
        px[(size_t)y * w] = 1;            /* left edge  */
        px[(size_t)y * w + (w - 1)] = 1;  /* right edge */
    }
    for (int y = 2; y < h - 2; y++)       /* diagonals every 12 px */
        for (int x = 2; x < w / 2; x++)
            if (((x + y) % 12) < 2) px[(size_t)y * w + x] = 1;
    for (int y = h / 4; y < 3 * h / 4; y++)   /* solid block right of centre */
        for (int x = w / 2 + 8; x < w / 2 + 40 && x < w - 4; x++)
            px[(size_t)y * w + x] = 1;
}

static volatile bool s_attached;

static void on_usb_event(ptouch_usb_event_t evt,
                         const ptouch_usb_devinfo_t *info, void *ctx)
{
    switch (evt) {
    case PTOUCH_USB_EVT_HOST_UP:
        ESP_LOGI(TAG, "USB host up — plug in the printer");
        break;
    case PTOUCH_USB_EVT_ATTACHED:
        ESP_LOGI(TAG, "printer attached: VID 0x%04X PID 0x%04X (%s speed)",
                 info->vid, info->pid, info->speed);
        s_attached = true;
        break;
    case PTOUCH_USB_EVT_DETACHED:
        ESP_LOGW(TAG, "printer detached");
        s_attached = false;
        break;
    case PTOUCH_USB_EVT_HOST_FAILED:
        ESP_LOGE(TAG, "USB host install failed");
        break;
    }
}

void app_main(void)
{
    ptouch_usb_config_t cfg = PTOUCH_USB_CONFIG_DEFAULT();
    cfg.on_event = on_usb_event;
    cfg.boot_delay_ms = 1000;   /* last chance to read this log over USB serial */
    ESP_ERROR_CHECK(ptouch_usb_start(&cfg));

    while (!s_attached) vTaskDelay(pdMS_TO_TICKS(250));
    vTaskDelay(pdMS_TO_TICKS(500));   /* let the printer settle after claim */

    ptouch_status_t st;
    int tape_mm = 12;
    if (ptouch_usb_status(&st)) {
        char errs[96];
        int nerr = ptouch_status_error_string(&st, errs, sizeof errs);
        ESP_LOGI(TAG, "status: media %u mm, type 0x%02X, model 0x%02X, errors: %s",
                 st.media_width_mm, st.media_type, st.model, nerr ? errs : "none");
        if (st.media_width_mm > 0) tape_mm = st.media_width_mm;
        if (nerr) { ESP_LOGE(TAG, "printer reports errors — not printing"); return; }
    } else {
        ESP_LOGW(TAG, "no status reply; assuming 12mm tape");
    }

    /* 76 dots covers 12 mm tape's printable area at 180 dpi (7.087 dots/mm). */
    const int W = 360, H = (tape_mm >= 24) ? 128 : 76;
    static uint8_t px[360 * 128];
    make_test_pattern(px, W, H);

    ptouch_print_opts_t opts = PTOUCH_PRINT_OPTS_DEFAULT();
    opts.tape_mm = tape_mm;
    int rc = ptouch_print_bitmap(px, W, H, &opts);
    if (rc == 0) ESP_LOGI(TAG, "printed + cut (%dx%d on %dmm tape)", W, H, tape_mm);
    else         ESP_LOGE(TAG, "print failed rc=%d", rc);
}
