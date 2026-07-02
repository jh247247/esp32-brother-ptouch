/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Jack Hosemans
 *
 * ptouch_usb.h — ESP32 USB-host transport for Brother P-touch label printers.
 *
 * Drives the printer over the ESP32-S2/S3's native USB-OTG host: installs the
 * USB Host Library, enumerates the attached printer (any device exposing a
 * bulk-IN + bulk-OUT pair; verified with the PT-P710BT, VID 0x04F9 PID 0x20AF),
 * claims the interface, and provides mutex-serialized bulk I/O plus a robust
 * status transaction.
 *
 * ─── BIG CAVEAT (ESP32-S3/S2) ────────────────────────────────────────────────
 * Installing the USB host library takes the chip's single USB PHY away from
 * USB-Serial-JTAG: your USB serial console and `idf.py flash` DISAPPEAR while
 * this driver runs. Provide another console (UART / network) and another
 * update path (OTA), or plan on BOOT-button recovery.
 * ────────────────────────────────────────────────────────────────────────────
 *
 * Hard-won behaviors baked in (all reproduced on real hardware):
 *  - Status IN reads retry past stale short packets: with status notifications
 *    enabled the printer queues unsolicited print-phase messages on bulk-IN,
 *    and ~13% of idle polls would otherwise fail on a leftover ZLP.
 *  - ptouch_usb_status() papers over the mechanical print window (feed/cut
 *    outlives the bulk transfer; the printer won't answer during it) with a
 *    short-TTL last-good cache — while a real unplug bypasses the cache
 *    immediately via the DEV_GONE event.
 *  - All I/O is serialized on one mutex, so a status poll can never interleave
 *    with a raster stream mid-print.
 */
#ifndef PTOUCH_USB_H
#define PTOUCH_USB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "ptouch_raster.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PTOUCH_USB_EVT_HOST_UP,      /* host library installed, waiting for device */
    PTOUCH_USB_EVT_HOST_FAILED,  /* usb_host_install/client_register failed    */
    PTOUCH_USB_EVT_ATTACHED,     /* device enumerated + interface claimed      */
    PTOUCH_USB_EVT_DETACHED,     /* device unplugged                           */
} ptouch_usb_event_t;

typedef struct {
    uint16_t vid, pid;
    uint8_t  bulk_out_ep, bulk_in_ep;
    const char *speed;           /* "Low"/"Full"/"High"/"?" */
} ptouch_usb_devinfo_t;

/* info is non-NULL only for PTOUCH_USB_EVT_ATTACHED. Called from the USB client
 * task — keep it short, don't block. */
typedef void (*ptouch_usb_event_cb_t)(ptouch_usb_event_t evt,
                                      const ptouch_usb_devinfo_t *info,
                                      void *user_ctx);

typedef struct {
    int  boot_delay_ms;      /* delay before usb_host_install (let WiFi/OTA come
                                up first — the PHY handover is one-way). 0 = none */
    bool dump_descriptors;   /* log full device/config descriptors on attach */
    int  status_cache_ms;    /* last-good status grace window (default 15000) */
    ptouch_usb_event_cb_t on_event;   /* optional */
    void *user_ctx;
} ptouch_usb_config_t;

#define PTOUCH_USB_CONFIG_DEFAULT() (ptouch_usb_config_t){ \
    .boot_delay_ms = 0, .dump_descriptors = false,          \
    .status_cache_ms = 15000, .on_event = NULL, .user_ctx = NULL }

/* Install the USB host library (after boot_delay_ms) and start enumeration.
 * cfg may be NULL for defaults. Returns ESP_OK when the driver task is up
 * (install failures are reported via PTOUCH_USB_EVT_HOST_FAILED — by design
 * the rest of your app keeps running so an OTA path stays alive). */
esp_err_t ptouch_usb_start(const ptouch_usb_config_t *cfg);

/* True while a printer is enumerated with its interface claimed. */
bool ptouch_usb_attached(void);

/* Send a raw byte stream (e.g. an assembled raster print stream) to the
 * printer's bulk-OUT endpoint, chunked. 0 on success, negative on error. */
int ptouch_usb_bulk_out(const uint8_t *data, size_t len, int timeout_ms);

/* One raw status transaction (invalidate + init + status_request → 32-byte
 * reply). Returns NULL on success (st filled) or a short error string.
 * Non-destructive — no tape moves. */
const char *ptouch_usb_status_txn(ptouch_status_t *st);

/* Status with the grace cache (see header comment). Returns false only when
 * no printer is attached or nothing succeeded within the cache window. */
bool ptouch_usb_status(ptouch_status_t *st);

#ifdef __cplusplus
}
#endif

#endif /* PTOUCH_USB_H */
