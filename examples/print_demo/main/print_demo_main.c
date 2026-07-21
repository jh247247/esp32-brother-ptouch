/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Jack Hosemans
 *
 * print_demo — plug an enabled Brother P-touch profile into an ESP32-S3 USB
 * host, get one test-pattern label. Demonstrates the safe structured result:
 * a submitted-but-unconfirmed label is never retried automatically.
 *
 * NOTE: once the USB host installs, the USB-Serial-JTAG console is GONE
 * (shared PHY). This demo logs over the default console until that moment;
 * after it you're flying blind unless you route logs to UART0
 * (CONFIG_ESP_CONSOLE_UART0) or a network sink. Reflashing needs the BOOT
 * button while the printer is unplugged.
 */
#include <string.h>
#include <stdlib.h>
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
        ESP_LOGI(TAG, "printer attached: %s (VID 0x%04X PID 0x%04X, %s)",
                 info->profile->name, info->vid, info->pid,
                 ptouch_support_name(info->support));
        s_attached = true;
        break;
    case PTOUCH_USB_EVT_UNSUPPORTED:
        ESP_LOGE(TAG, "printer refused: %s (%s)",
                 info && info->profile ? info->profile->name : "unknown model",
                 info ? ptouch_support_name(info->support) : "unknown");
        break;
    case PTOUCH_USB_EVT_ATTACH_FAILED:
        ESP_LOGE(TAG, "printer USB interface could not be claimed");
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
    /* Recipe-derived profiles are deliberately refused by default. For a
     * supervised community hardware test only, explicitly set:
     * cfg.allow_experimental_profiles = true; */
    ESP_ERROR_CHECK(ptouch_usb_start(&cfg));

    while (!s_attached) vTaskDelay(pdMS_TO_TICKS(250));
    vTaskDelay(pdMS_TO_TICKS(500));   /* let the printer settle after claim */

    ptouch_usb_ready_t ready;
    if (!ptouch_usb_ready_snapshot(&ready)) {
        ESP_LOGE(TAG, "printer is not idle-ready with supported media");
        return;
    }
    char errors[96];
    int error_count = ptouch_status_error_string(
        &ready.status, errors, sizeof errors);
    if (error_count) {
        ESP_LOGE(TAG, "printer reports errors: %s", errors);
        return;
    }

    const int W = 180;
    const int H = ready.geometry.printable_dots;
    uint8_t *px = calloc((size_t)W, (size_t)H);
    if (!px) {
        ESP_LOGE(TAG, "bitmap allocation failed");
        return;
    }
    make_test_pattern(px, W, H);

    ptouch_print_opts_t opts;
    if (!ptouch_print_opts_init(ready.target.profile,
                                ready.status.media_width_mm, &opts)) {
        ESP_LOGE(TAG, "profile-aware print options rejected the media");
        free(px);
        return;
    }
    ptouch_usb_print_result_t result =
        ptouch_usb_print_bitmap(px, W, H, &opts, 60000);
    free(px);

    if (result.transfer_rc == PTOUCH_USB_TRANSFER_OK &&
        result.completion_rc == PTOUCH_USB_COMPLETION_OK) {
        ESP_LOGI(TAG, "printed and mechanically completed");
    } else if (result.stream_submitted) {
        ESP_LOGE(TAG,
                 "label may have printed (transfer=%d completion=%d); DO NOT RETRY — inspect output, then clear quarantine deliberately",
                 result.transfer_rc, result.completion_rc);
    } else {
        ESP_LOGE(TAG, "safe zero-output rejection (transfer=%d completion=%d)",
                 result.transfer_rc, result.completion_rc);
    }
}
