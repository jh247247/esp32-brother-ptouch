/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Jack Hosemans
 *
 * ptouch_usb.h — ESP32 USB-host transport for Brother P-touch label printers.
 *
 * Drives the printer over the ESP32-S3's native USB-OTG host: installs the
 * USB Host Library, identifies the attached printer, claims only a recognized
 * printable profile exposing a same-interface bulk-IN + bulk-OUT pair, and
 * provides mutex-serialized bulk I/O plus robust status and
 * mechanical-completion transactions.
 *
 * ─── BIG CAVEAT (ESP32-S3) ───────────────────────────────────────────────────
 * Installing the USB host library takes the chip's single USB PHY away from
 * USB-Serial-JTAG: your USB serial console and `idf.py flash` DISAPPEAR while
 * this driver runs. Provide another console (UART / network) and another
 * update path (OTA), or plan on BOOT-button recovery.
 * ────────────────────────────────────────────────────────────────────────────
 *
 * Hard-won behaviors baked in (all reproduced on real hardware):
 *  - Idle status transactions drain stale automatic notifications until the
 *    reply to the current request (or a current error) arrives.
 *  - A print holds the I/O mutex from the first raster byte through Brother's
 *    automatic printing/completed/ready notifications. No status command is
 *    sent during printing: Brother explicitly forbids that sequence.
 *  - Every print begins with a fresh idle status reply under the same I/O
 *    lease, establishing an RX epoch after all older notifications are drained.
 *  - ptouch_usb_status() smooths transient idle-poll failures with a short-TTL
 *    last-good cache, while a real unplug bypasses it immediately.
 *  - All I/O is serialized on one mutex, so a status poll can never interleave
 *    with a raster stream mid-print.
 */
#ifndef PTOUCH_USB_H
#define PTOUCH_USB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "ptouch_model.h"
#include "ptouch_raster.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PTOUCH_USB_EVT_HOST_UP,      /* host task + client registered, waiting device */
    PTOUCH_USB_EVT_HOST_FAILED,  /* usb_host_install/client_register failed    */
    PTOUCH_USB_EVT_ATTACHED,     /* device enumerated + interface claimed      */
    PTOUCH_USB_EVT_UNSUPPORTED,  /* Brother model detected; no interface claim */
    PTOUCH_USB_EVT_ATTACH_FAILED,/* supported model, unusable USB interface     */
    PTOUCH_USB_EVT_DETACHED,     /* device unplugged                           */
} ptouch_usb_event_t;

typedef struct {
    uint16_t vid, pid;
    uint8_t  bulk_out_ep, bulk_in_ep;
    const char *speed;           /* "Low"/"Full"/"High"/"?" */
    const ptouch_model_profile_t *profile; /* NULL for unknown Brother PID */
    ptouch_support_t support;
    bool claimed;
    uint32_t generation;
} ptouch_usb_devinfo_t;

typedef struct {
    const ptouch_model_profile_t *profile;
    uint16_t vid, pid;
    uint32_t generation;
} ptouch_usb_target_t;

typedef struct {
    ptouch_usb_target_t target;
    ptouch_status_t status;
    ptouch_media_geometry_t geometry;
} ptouch_usb_ready_t;

/* info is non-NULL for device-specific attach/refusal/failure events. Events
 * are copied onto a dedicated callback task, outside the USB transaction lock;
 * callbacks may safely query snapshots but should still remain bounded. */
typedef void (*ptouch_usb_event_cb_t)(ptouch_usb_event_t evt,
                                      const ptouch_usb_devinfo_t *info,
                                      void *user_ctx);

typedef struct {
    int  boot_delay_ms;      /* delay before usb_host_install (let WiFi/OTA come
                                up first — the PHY handover is one-way). 0 = none */
    bool dump_descriptors;   /* log full device/config descriptors on attach */
    int  status_cache_ms;    /* last-good status grace window (default 15000) */
    bool allow_experimental_profiles; /* explicit opt-in for recipe-only output */
    ptouch_usb_event_cb_t on_event;   /* optional */
    void *user_ctx;
} ptouch_usb_config_t;

typedef enum {
    PTOUCH_USB_COMPLETION_NOT_RUN = 1,
    PTOUCH_USB_COMPLETION_OK = 0,
    PTOUCH_USB_COMPLETION_TIMEOUT = -1,
    PTOUCH_USB_COMPLETION_IO_ERROR = -2,
    PTOUCH_USB_COMPLETION_PRINTER_ERROR = -3,
    PTOUCH_USB_COMPLETION_PROTOCOL_ERROR = -4,
    /* The printer emitted its nominal completion status, but this profile's
     * completion semantics have not yet passed hardware validation. The label
     * may have printed; inspect it and never retry automatically. */
    PTOUCH_USB_COMPLETION_UNVALIDATED = -5,
} ptouch_usb_completion_result_t;

typedef enum {
    PTOUCH_USB_TRANSFER_OK = 0,
    PTOUCH_USB_TRANSFER_INVALID_ARGUMENT = -1,
    PTOUCH_USB_TRANSFER_NOT_ATTACHED = -2,
    PTOUCH_USB_TRANSFER_ALLOCATION_FAILED = -3,
    PTOUCH_USB_TRANSFER_IO_ERROR = -4,
    PTOUCH_USB_TRANSFER_PREFLIGHT_FAILED = -5,
    PTOUCH_USB_TRANSFER_TARGET_CHANGED = -6,
    PTOUCH_USB_TRANSFER_UNSUPPORTED = -7,
    PTOUCH_USB_TRANSFER_BLOCKED = -8,
    PTOUCH_USB_TRANSFER_TRANSPORT_FAULT = -9,
    PTOUCH_USB_TRANSFER_BUILD_FAILED = -10,
} ptouch_usb_transfer_result_t;

typedef struct {
    ptouch_usb_transfer_result_t transfer_rc;
    ptouch_usb_completion_result_t completion_rc;
    bool stream_submitted; /* true once any raster OUT transfer was accepted */
    bool has_status;
    ptouch_status_t status;
} ptouch_usb_print_result_t;

#define PTOUCH_USB_CONFIG_DEFAULT() (ptouch_usb_config_t){ \
    .boot_delay_ms = 0, .dump_descriptors = false,          \
    .status_cache_ms = 15000, .allow_experimental_profiles = false, \
    .on_event = NULL, .user_ctx = NULL }

/* Install the USB host library (after boot_delay_ms) and start enumeration.
 * cfg may be NULL for defaults. Returns ESP_OK when the driver task is up
 * (install failures are reported via PTOUCH_USB_EVT_HOST_FAILED — by design
 * the rest of your app keeps running so an OTA path stays alive). This is a
 * single-start driver; every later call returns ESP_ERR_INVALID_STATE. */
esp_err_t ptouch_usb_start(const ptouch_usb_config_t *cfg);

/* True while a printer is enumerated with its interface claimed. */
bool ptouch_usb_attached(void);

/* Last detected Brother device, including refused/failed devices. The
 * returned profile pointer is immutable for the component lifetime. */
bool ptouch_usb_device_info(ptouch_usb_devinfo_t *out);

/* Snapshot a printable attachment. Revalidated under the print I/O lease. */
bool ptouch_usb_target_snapshot(ptouch_usb_target_t *out);

/* One fresh, generation-consistent readiness snapshot. The target identity,
 * status reply, admitted media and geometry are all captured under the same
 * I/O lease. This is the gate for application admission and multi-label
 * sessions. */
bool ptouch_usb_ready_snapshot(ptouch_usb_ready_t *out);

/* Read the most recent generation-consistent ready snapshot without issuing a
 * printer command. This is an admission/rendering hint only: every output
 * path must still use ptouch_usb_print_job_and_wait(), whose fresh status
 * transaction is the authoritative preflight immediately before raster OUT. */
bool ptouch_usb_cached_ready_snapshot(ptouch_usb_ready_t *out);

/* Set or clear the application's temporary status-command gate (for boot
 * recovery and queue synchronization). The component separately latches an
 * output quarantine when a submitted print does not reach confirmed
 * completion; clearing this application gate never clears that quarantine. */
void ptouch_usb_set_status_commands_blocked(bool blocked);
bool ptouch_usb_status_commands_blocked(void);

/* Remove only the transport-owned ambiguous-output quarantine. The caller must
 * keep every print path closed and first accept the explicit per-fault
 * physical-inspection confirmation plus a fresh idle-ready transaction. */
void ptouch_usb_clear_output_quarantine_after_inspection(void);
bool ptouch_usb_output_quarantined(void);

/* A transfer that cannot be reclaimed within the bounded cancellation window
 * faults the transport. No further USB commands are accepted; reboot before
 * printing again. This terminal state is intentionally not clearable. */
bool ptouch_usb_transport_faulted(void);

/* Low-level bulk-OUT for non-print maintenance commands. It cannot confirm
 * mechanical output and must never carry a raster print stream; use
 * ptouch_usb_print_job_and_wait() for printing. Quarantine and terminal
 * transport faults are enforced on this path too. */
#if defined(__GNUC__)
__attribute__((deprecated("use ptouch_usb_print_job_and_wait for raster printing")))
#endif
int ptouch_usb_bulk_out(const uint8_t *data, size_t len, int timeout_ms);

/* Establish a fresh idle status epoch, transfer a complete raster stream, then
 * consume the printer's automatic status notifications under one I/O lease
 * until the current label is confirmed ready or fails. `stream_submitted`
 * distinguishes a safe preflight rejection from a physically ambiguous OUT.
 * `completion_timeout_ms` covers the mechanical phase after the final chunk. */
#if defined(__GNUC__)
__attribute__((deprecated("P710-only compatibility API; use ptouch_usb_print_job_and_wait")))
#endif
ptouch_usb_print_result_t ptouch_usb_print_and_wait(
    const uint8_t *data, size_t len,
    int transfer_timeout_ms, int completion_timeout_ms);

struct ptouch_print_job;
ptouch_usb_print_result_t ptouch_usb_print_job_and_wait(
    const struct ptouch_print_job *job,
    int transfer_timeout_ms, int completion_timeout_ms);

struct ptouch_print_opts;
/* High-level structured print API. A non-OK result with stream_submitted=true
 * is physically ambiguous: do not retry; inspect the printer and label first.
 * `opts` is required; initialize it from the detected profile and tape with
 * ptouch_print_opts_init(). */
ptouch_usb_print_result_t ptouch_usb_print_bitmap(
    const uint8_t *px, int w, int h,
    const struct ptouch_print_opts *opts,
    int completion_timeout_ms);

/* One ordinary idle status transaction (invalidate + init + status_request →
 * 32-byte reply). Stale automatic notifications are drained first. This is
 * refused while output is physically ambiguous because ESC @ can cancel a
 * print. Returns NULL on success or a short error string. */
const char *ptouch_usb_status_txn(ptouch_status_t *st);

/* Purpose-bound uncached transaction used only after the operator has recorded
 * physical inspection and requested a supported clear. It bypasses the status
 * command quarantine while the print gate itself remains closed. */
const char *ptouch_usb_status_txn_after_inspection(ptouch_status_t *st);

/* Non-blocking status with the grace cache. If a print owns the lease, returns
 * the recent cache instead of stalling a web/LCD task. Returns false when no
 * printer is attached, quarantine is active, or no fresh cache exists. */
bool ptouch_usb_status(ptouch_status_t *st);

#ifdef __cplusplus
}
#endif

#endif /* PTOUCH_USB_H */
