/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Jack Hosemans
 *
 * ptouch_usb.c — ESP32 USB-host transport for Brother P-touch printers.
 * See ptouch_usb.h for the API and the USB-PHY caveat.
 */
#include "ptouch_usb.h"
#include "ptouch_completion.h"
#include "ptouch_print.h"
#include "ptouch_usb_descriptor.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_intr_alloc.h"
#include "esp_idf_version.h"
#include "esp_bit_defs.h"
#include "usb/usb_host.h"

static const char *TAG = "ptouch_usb";

static ptouch_usb_config_t s_cfg;
static usb_host_client_handle_t s_client;
static QueueHandle_t s_event_queue;
static TaskHandle_t s_event_task_handle;
static portMUX_TYPE s_start_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_gate_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_start_called;

/* Persisted device context for bulk I/O. Set at enumeration, cleared on
 * disconnect. Guarded by s_io_mtx while a transaction runs so a mid-transfer
 * unplug can't free the device out from under us. */
static usb_device_handle_t s_dev;
static usb_device_handle_t s_observed_dev;
static uint8_t   s_intf_num;
static uint8_t   s_bulk_out_ep, s_bulk_in_ep;
static uint16_t  s_bulk_in_mps = 64;
static volatile bool s_intf_claimed;
static volatile bool s_device_detected;
static const ptouch_model_profile_t *s_profile;
static ptouch_usb_devinfo_t s_device_info;
static volatile uint32_t s_attachment_generation;
static volatile bool s_dev_gone_pending;
static volatile int s_new_dev_addr_pending = -1;
/* The application gate is temporary (boot recovery, queue synchronization).
 * The output quarantine is transport-owned and latches whenever bytes may
 * have reached the printer without a confirmed mechanical completion. Keeping
 * them separate lets a proven pre-output CLAIMED receipt run its idle
 * preflight without accidentally clearing a same-boot ambiguous print. */
static bool s_status_app_blocked;
static bool s_output_quarantined;
static bool s_transport_faulted;
static SemaphoreHandle_t s_io_mtx;
static SemaphoreHandle_t s_cache_mtx;
static ptouch_status_t s_status_cache;
static int64_t s_status_cache_us = -1;
static uint32_t s_status_cache_generation;

/* Caller owns the I/O lease, which pins `generation` to the status source. */
static void publish_status_cache_locked(const ptouch_status_t *status,
                                        uint32_t generation)
{
    if (!status || !status->valid || !s_cache_mtx) return;
    xSemaphoreTake(s_cache_mtx, portMAX_DELAY);
    s_status_cache = *status;
    s_status_cache_us = esp_timer_get_time();
    s_status_cache_generation = generation;
    xSemaphoreGive(s_cache_mtx);
}

typedef struct {
    ptouch_usb_event_t evt;
    bool has_info;
    ptouch_usb_devinfo_t info;
} ptouch_usb_event_message_t;

static void event_task(void *arg)
{
    (void)arg;
    ptouch_usb_event_message_t message;
    for (;;) {
        if (xQueueReceive(s_event_queue, &message, portMAX_DELAY) == pdTRUE &&
            s_cfg.on_event) {
            s_cfg.on_event(message.evt,
                           message.has_info ? &message.info : NULL,
                           s_cfg.user_ctx);
        }
    }
}

/* Queueing is safe while s_io_mtx is held because user code runs only on the
 * dedicated event task. Device information is copied before the lock is
 * released, so callbacks never observe a later attachment generation. */
static void emit(ptouch_usb_event_t evt, const ptouch_usb_devinfo_t *info)
{
    if (!s_cfg.on_event || !s_event_queue) return;
    ptouch_usb_event_message_t message = {
        .evt = evt,
        .has_info = info != NULL,
    };
    if (info) message.info = *info;
    if (xQueueSend(s_event_queue, &message, 0) != pdTRUE) {
        ESP_LOGE(TAG, "dropping USB event %d: callback queue full", (int)evt);
    }
}

static bool profile_output_allowed(const ptouch_model_profile_t *profile)
{
    return ptouch_model_output_allowed(
        profile, s_cfg.allow_experimental_profiles);
}

static bool status_commands_blocked_locked(void)
{
    taskENTER_CRITICAL(&s_gate_lock);
    bool blocked =
        s_status_app_blocked || s_output_quarantined || s_transport_faulted;
    taskEXIT_CRITICAL(&s_gate_lock);
    return blocked;
}

static bool transport_faulted(void)
{
    taskENTER_CRITICAL(&s_gate_lock);
    bool faulted = s_transport_faulted;
    taskEXIT_CRITICAL(&s_gate_lock);
    return faulted;
}

static bool output_quarantined(void)
{
    taskENTER_CRITICAL(&s_gate_lock);
    bool quarantined = s_output_quarantined;
    taskEXIT_CRITICAL(&s_gate_lock);
    return quarantined;
}

static void quarantine_output(void)
{
    taskENTER_CRITICAL(&s_gate_lock);
    s_output_quarantined = true;
    taskEXIT_CRITICAL(&s_gate_lock);
}

static void fault_transport(bool quarantine)
{
    taskENTER_CRITICAL(&s_gate_lock);
    s_transport_faulted = true;
    if (quarantine) s_output_quarantined = true;
    taskEXIT_CRITICAL(&s_gate_lock);
}

bool ptouch_usb_attached(void)
{
    if (!s_io_mtx) return false;
    xSemaphoreTake(s_io_mtx, portMAX_DELAY);
    bool attached = !s_dev_gone_pending && s_intf_claimed &&
                    profile_output_allowed(s_profile);
    xSemaphoreGive(s_io_mtx);
    return attached;
}

bool ptouch_usb_device_info(ptouch_usb_devinfo_t *out)
{
    if (!out || !s_io_mtx) return false;
    xSemaphoreTake(s_io_mtx, portMAX_DELAY);
    bool present = s_device_detected && !s_dev_gone_pending;
    if (present) *out = s_device_info;
    xSemaphoreGive(s_io_mtx);
    return present;
}

bool ptouch_usb_target_snapshot(ptouch_usb_target_t *out)
{
    if (!out || !s_io_mtx) return false;
    xSemaphoreTake(s_io_mtx, portMAX_DELAY);
    bool ready = !s_dev_gone_pending && s_intf_claimed && s_dev &&
                 profile_output_allowed(s_profile);
    if (ready) {
        out->profile = s_profile;
        out->vid = s_device_info.vid;
        out->pid = s_device_info.pid;
        out->generation = s_attachment_generation;
    }
    xSemaphoreGive(s_io_mtx);
    return ready;
}

void ptouch_usb_set_status_commands_blocked(bool blocked)
{
    taskENTER_CRITICAL(&s_gate_lock);
    s_status_app_blocked = blocked;
    taskEXIT_CRITICAL(&s_gate_lock);
}

bool ptouch_usb_status_commands_blocked(void)
{
    return status_commands_blocked_locked();
}

void ptouch_usb_clear_output_quarantine_after_inspection(void)
{
    taskENTER_CRITICAL(&s_gate_lock);
    s_output_quarantined = false;
    taskEXIT_CRITICAL(&s_gate_lock);
}

bool ptouch_usb_output_quarantined(void)
{
    return output_quarantined();
}

bool ptouch_usb_transport_faulted(void)
{
    return transport_faulted();
}

/* ------------------------------------------------------- enumeration ------ */
/* Called only by usb_task after it has acquired s_io_mtx. */
static void report_device_locked(uint8_t addr)
{
    usb_device_handle_t dev;
    esp_err_t err = usb_host_device_open(s_client, addr, &dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "device_open(%d) failed: %s", addr, esp_err_to_name(err));
        return;
    }

    /* One physical target at a time. A hub or second device must not overwrite
     * the identity/generation of the active printer. */
    if (s_observed_dev) {
        ESP_LOGW(TAG, "ignoring additional USB device at address %u while a target is active",
                 addr);
        usb_host_device_close(s_client, dev);
        return;
    }
    s_observed_dev = dev;

    const char *speed_str = "?";
    usb_device_info_t info;
    if (usb_host_device_info(dev, &info) == ESP_OK) {
        speed_str = (info.speed <= USB_SPEED_HIGH)
                    ? (const char *[]){"Low", "Full", "High"}[info.speed] : "?";
    }

    const usb_device_desc_t *dd = NULL;
    uint16_t vid = 0, pid = 0;
    if (usb_host_get_device_descriptor(dev, &dd) == ESP_OK && dd) {
        vid = dd->idVendor; pid = dd->idProduct;
        if (s_cfg.dump_descriptors) usb_print_device_descriptor(dd);
    }

    const ptouch_model_profile_t *profile = ptouch_model_lookup(vid, pid);
    ptouch_support_t support = profile ? profile->support : PTOUCH_SUPPORT_UNKNOWN;
    ++s_attachment_generation;
    s_device_detected = true;
    s_profile = profile;
    s_device_info = (ptouch_usb_devinfo_t) {
        .vid = vid,
        .pid = pid,
        .speed = speed_str,
        .profile = profile,
        .support = support,
        .claimed = false,
        .generation = s_attachment_generation,
    };
    if (s_cache_mtx) {
        xSemaphoreTake(s_cache_mtx, portMAX_DELAY);
        s_status_cache_us = -1;
        s_status_cache_generation = 0;
        xSemaphoreGive(s_cache_mtx);
    }

    if (!profile_output_allowed(profile)) {
        ESP_LOGW(TAG, "refusing USB device VID:0x%04X PID:0x%04X model=%s support=%s",
                 vid, pid, profile ? profile->name : "unknown",
                 ptouch_support_name(support));
        emit(PTOUCH_USB_EVT_UNSUPPORTED, &s_device_info);
        return;
    }

    ptouch_usb_bulk_pair_t pair = {0};
    bool pair_valid = false;
    const usb_config_desc_t *cfg = NULL;
    if (usb_host_get_active_config_descriptor(dev, &cfg) == ESP_OK && cfg) {
        if (s_cfg.dump_descriptors) usb_print_config_descriptor(cfg, NULL);
        pair_valid = ptouch_usb_find_printer_bulk_pair(
            (const uint8_t *)cfg, cfg->wTotalLength, &pair);
    }

    ESP_LOGI(TAG, "device VID:0x%04X PID:0x%04X model=%s speed=%s bulkOUT=0x%02X bulkIN=0x%02X",
             vid, pid, profile->name, speed_str,
             pair_valid ? pair.bulk_out_ep : 0,
             pair_valid ? pair.bulk_in_ep : 0);

    if (pair_valid) {
        s_dev         = dev;
        s_intf_num    = pair.interface_number;
        s_bulk_out_ep = pair.bulk_out_ep;
        s_bulk_in_ep  = pair.bulk_in_ep;
        s_bulk_in_mps = pair.bulk_in_mps;
        esp_err_t ce = usb_host_interface_claim(
            s_client, dev, s_intf_num, pair.alternate_setting);
        s_intf_claimed = (ce == ESP_OK);
        if (s_intf_claimed) {
            ESP_LOGI(TAG, "claimed interface %u (bulkOUT 0x%02x, bulkIN 0x%02x mps=%u)",
                     s_intf_num, s_bulk_out_ep, s_bulk_in_ep, s_bulk_in_mps);
            s_device_info.bulk_out_ep = s_bulk_out_ep;
            s_device_info.bulk_in_ep = s_bulk_in_ep;
            s_device_info.claimed = true;
            emit(PTOUCH_USB_EVT_ATTACHED, &s_device_info);
        } else {
            ESP_LOGE(TAG, "interface_claim(%u) failed: %s", s_intf_num, esp_err_to_name(ce));
            s_dev = NULL;
            emit(PTOUCH_USB_EVT_ATTACH_FAILED, &s_device_info);
        }
    } else {
        ESP_LOGW(TAG, "supported model has no same-interface bulk IN/OUT pair");
        emit(PTOUCH_USB_EVT_ATTACH_FAILED, &s_device_info);
    }
}

/* -------------------------------------------------------- bulk I/O -------- */
/* A transfer's callback runs in the USB client task; it just wakes the caller. */
static void xfer_cb(usb_transfer_t *t)
{
    xSemaphoreGive((SemaphoreHandle_t)t->context);
}

typedef enum {
    XFER_OK = 0,
    XFER_TIMEOUT,
    XFER_ERROR,
} xfer_result_t;

#define PTOUCH_TRANSFER_RECLAIM_GRACE_MS 1000

/* ESP-IDF requires an endpoint to be halted before it can be flushed. A
 * transfer must also remain allocated until its cancellation callback runs.
 * Waiting for that callback here avoids freeing an in-flight transfer and
 * leaving the endpoint in ESP_ERR_INVALID_STATE after a timeout. */
static xfer_result_t cancel_timed_out_transfer(usb_transfer_t *t,
                                               SemaphoreHandle_t sem,
                                               bool quarantine_transport,
                                               bool *retain_resources)
{
    if (retain_resources) *retain_resources = false;
    if (quarantine_transport) {
        ESP_LOGE(TAG,
                 "retaining timed-out print transfer until callback resolves ownership");
    }
    esp_err_t halt = usb_host_endpoint_halt(s_dev, t->bEndpointAddress);
    if (halt != ESP_OK && halt != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "halt ep 0x%02x after timeout: %s",
                 t->bEndpointAddress, esp_err_to_name(halt));
    }

    esp_err_t flush = usb_host_endpoint_flush(s_dev, t->bEndpointAddress);
    if (flush != ESP_OK && flush != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "flush ep 0x%02x after timeout: %s",
                 t->bEndpointAddress, esp_err_to_name(flush));
    }

    /* A successful flush cancels queued transfers and delivers their callback
     * through usb_host_client_handle_events(). If halt/flush raced a just-
     * completed transfer, that normal callback is delivered instead. In both
     * cases ownership does not return to the caller until the transfer is no
     * longer in flight. */
    if (xSemaphoreTake(sem, pdMS_TO_TICKS(PTOUCH_TRANSFER_RECLAIM_GRACE_MS)) !=
        pdTRUE) {
        /* ESP-IDF forbids freeing an in-flight transfer. Retain this one
         * transfer and its semaphore for the rest of the boot, then fault the
         * transport so no second leak or unsafe command can occur. */
        if (retain_resources) *retain_resources = true;
        fault_transport(quarantine_transport);
        ESP_LOGE(TAG,
                 "transfer callback did not arrive within %d ms; transport faulted until reboot",
                 PTOUCH_TRANSFER_RECLAIM_GRACE_MS);
        return XFER_TIMEOUT;
    }

    if (halt == ESP_OK || flush == ESP_OK) {
        esp_err_t clear = usb_host_endpoint_clear(s_dev, t->bEndpointAddress);
        if (clear != ESP_OK && clear != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "clear ep 0x%02x after timeout: %s",
                     t->bEndpointAddress, esp_err_to_name(clear));
        }
    }

    if (t->status == USB_TRANSFER_STATUS_COMPLETED) return XFER_OK;
    if (quarantine_transport) {
        quarantine_output();
        ESP_LOGE(TAG, "timed-out print transfer did not complete; transport quarantined");
    }
    return XFER_TIMEOUT;
}

static xfer_result_t submit_and_wait(usb_transfer_t *t, SemaphoreHandle_t sem,
                                     int timeout_ms, bool *submitted,
                                     bool quarantine_on_timeout,
                                     bool *retain_resources)
{
    if (submitted) *submitted = false;
    if (retain_resources) *retain_resources = false;
    t->callback = xfer_cb;
    t->context = sem;
    t->device_handle = s_dev;
    esp_err_t e = usb_host_transfer_submit(t);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "submit ep 0x%02x: %s", t->bEndpointAddress, esp_err_to_name(e));
        return XFER_ERROR;
    }
    if (submitted) *submitted = true;
    if (xSemaphoreTake(sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "ep 0x%02x timeout — halting and canceling",
                 t->bEndpointAddress);
        return cancel_timed_out_transfer(t, sem, quarantine_on_timeout,
                                         retain_resources);
    }
    if (t->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "ep 0x%02x transfer status=%d", t->bEndpointAddress, t->status);
        return XFER_ERROR;
    }
    return XFER_OK;
}

static ptouch_usb_transfer_result_t bulk_out_locked(
    const uint8_t *data, size_t len, int timeout_ms, bool *stream_submitted)
{
    SemaphoreHandle_t sem = NULL;
    usb_transfer_t *t = NULL;
    size_t chunk = 0;
    bool retain_resources = false;
    ptouch_usb_transfer_result_t rc;

    if (stream_submitted) *stream_submitted = false;

    if (!data || len == 0 || timeout_ms <= 0) {
        rc = PTOUCH_USB_TRANSFER_INVALID_ARGUMENT;
        goto cleanup;
    }
    if (transport_faulted()) {
        rc = PTOUCH_USB_TRANSFER_TRANSPORT_FAULT;
        goto cleanup;
    }
    if (output_quarantined()) {
        rc = PTOUCH_USB_TRANSFER_BLOCKED;
        goto cleanup;
    }
    if (!s_intf_claimed || !s_dev) {
        rc = PTOUCH_USB_TRANSFER_NOT_ATTACHED;
        goto cleanup;
    }

    sem = xSemaphoreCreateBinary();
    chunk = (len && len < 4096) ? len : 4096;
    if (!sem || usb_host_transfer_alloc(chunk, 0, &t) != ESP_OK) {
        rc = PTOUCH_USB_TRANSFER_ALLOCATION_FAILED;
        goto cleanup;
    }

    rc = PTOUCH_USB_TRANSFER_OK;
    for (size_t off = 0; off < len; ) {
        size_t n = len - off;
        if (n > chunk) n = chunk;
        memcpy(t->data_buffer, data + off, n);
        t->num_bytes = (int)n;
        t->bEndpointAddress = s_bulk_out_ep;
        bool submitted = false;
        if (submit_and_wait(t, sem, timeout_ms, &submitted, true,
                            &retain_resources) != XFER_OK) {
            if (submitted && stream_submitted) *stream_submitted = true;
            rc = PTOUCH_USB_TRANSFER_IO_ERROR;
            break;
        }
        if (stream_submitted) *stream_submitted = true;
        off += n;
    }

cleanup:
    if (!retain_resources) {
        if (t) usb_host_transfer_free(t);
        if (sem) vSemaphoreDelete(sem);
    }
    return rc;
}

int ptouch_usb_bulk_out(const uint8_t *data, size_t len, int timeout_ms)
{
    if (!s_io_mtx) return -1;
    xSemaphoreTake(s_io_mtx, portMAX_DELAY);
    int rc = bulk_out_locked(data, len, timeout_ms, NULL);
    xSemaphoreGive(s_io_mtx);
    return rc;
}

static ptouch_usb_transfer_result_t print_job_out_locked(
    const ptouch_print_job_t *job, int timeout_ms, bool *stream_submitted)
{
    if (stream_submitted) *stream_submitted = false;
    if (!ptouch_print_job_frames_valid(job)) {
        return PTOUCH_USB_TRANSFER_INVALID_ARGUMENT;
    }
    if (job->profile->transfer_policy == PTOUCH_TRANSFER_COALESCED) {
        return bulk_out_locked(job->stream.data, job->stream.len, timeout_ms,
                               stream_submitted);
    }
    /* Validate the complete frame table before the first byte can leave the
     * host. A malformed middle offset must be a zero-output rejection. */
    size_t start = 0;
    size_t max_frame = 0;
    for (size_t i = 0; i < job->frame_count; ++i) {
        size_t end = job->frame_ends[i];
        if (end <= start || end > job->stream.len) {
            return PTOUCH_USB_TRANSFER_INVALID_ARGUMENT;
        }
        size_t frame_len = end - start;
        if (frame_len > max_frame) max_frame = frame_len;
        start = end;
    }

    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    usb_transfer_t *transfer = NULL;
    bool retain_resources = false;
    if (!sem || max_frame == 0 ||
        usb_host_transfer_alloc(max_frame, 0, &transfer) != ESP_OK) {
        if (sem) vSemaphoreDelete(sem);
        return PTOUCH_USB_TRANSFER_ALLOCATION_FAILED;
    }

    ptouch_usb_transfer_result_t rc = PTOUCH_USB_TRANSFER_OK;
    bool any_submitted = false;
    start = 0;
    for (size_t i = 0; i < job->frame_count; ++i) {
        size_t end = job->frame_ends[i];
        size_t frame_len = end - start;
        memcpy(transfer->data_buffer, job->stream.data + start, frame_len);
        transfer->num_bytes = (int)frame_len;
        transfer->bEndpointAddress = s_bulk_out_ep;
        bool submitted = false;
        if (submit_and_wait(transfer, sem, timeout_ms, &submitted, true,
                            &retain_resources) !=
            XFER_OK) {
            any_submitted = any_submitted || submitted;
            rc = PTOUCH_USB_TRANSFER_IO_ERROR;
            break;
        }
        any_submitted = true;
        start = end;
    }
    if (!retain_resources) {
        usb_host_transfer_free(transfer);
        vSemaphoreDelete(sem);
    }
    if (stream_submitted) *stream_submitted = any_submitted;
    return rc;
}

static const char *status_txn_locked(ptouch_status_t *st,
                                     bool after_inspection);

static ptouch_usb_completion_result_t wait_for_print_completion_locked(
    int timeout_ms, ptouch_usb_print_result_t *result,
    ptouch_completion_tracker_t *tracker)
{
    if (!result || !tracker || timeout_ms <= 0) {
        return PTOUCH_USB_COMPLETION_IO_ERROR;
    }
    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    usb_transfer_t *rx = NULL;
    int rxlen = ((PTOUCH_STATUS_LENGTH + s_bulk_in_mps - 1) /
                 s_bulk_in_mps) * s_bulk_in_mps;
    if (!sem || usb_host_transfer_alloc(rxlen, 0, &rx) != ESP_OK) {
        if (sem) vSemaphoreDelete(sem);
        return PTOUCH_USB_COMPLETION_IO_ERROR;
    }

    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    int invalid_frames = 0;
    bool retain_resources = false;
    ptouch_usb_completion_result_t completion = PTOUCH_USB_COMPLETION_TIMEOUT;

    while (esp_timer_get_time() < deadline) {
        int64_t remaining_us = deadline - esp_timer_get_time();
        int remaining_ms = (int)((remaining_us + 999) / 1000);
        if (remaining_ms < 1) remaining_ms = 1;

        rx->num_bytes = rxlen;
        rx->bEndpointAddress = s_bulk_in_ep;
        xfer_result_t xr = submit_and_wait(rx, sem, remaining_ms, NULL, true,
                                           &retain_resources);
        if (xr == XFER_TIMEOUT) {
            completion = PTOUCH_USB_COMPLETION_TIMEOUT;
            break;
        }
        if (xr != XFER_OK) {
            completion = PTOUCH_USB_COMPLETION_IO_ERROR;
            break;
        }
        if (rx->actual_num_bytes < PTOUCH_STATUS_LENGTH) {
            continue; /* ZLP or a short stale USB packet */
        }

        for (int off = 0;
             off + PTOUCH_STATUS_LENGTH <= rx->actual_num_bytes;
             off += PTOUCH_STATUS_LENGTH) {
            ptouch_status_t status;
            ptouch_parse_status(rx->data_buffer + off, &status);
            if (!status.valid) {
                if (++invalid_frames >= 4) {
                    completion = PTOUCH_USB_COMPLETION_PROTOCOL_ERROR;
                    goto done;
                }
                continue;
            }

            result->has_status = true;
            result->status = status;
            ESP_LOGI(TAG,
                     "print status type=0x%02x phase=0x%02x/%u mode=0x%02x errors=0x%02x/0x%02x",
                     status.status_type, status.phase_type, status.phase_number,
                     status.mode, status.error_information_1,
                     status.error_information_2);

            ptouch_completion_observation_t observation =
                ptouch_completion_observe(tracker, &status);
            if (observation == PTOUCH_COMPLETION_PRINTER_ERROR) {
                completion = PTOUCH_USB_COMPLETION_PRINTER_ERROR;
                goto done;
            }
            if (observation == PTOUCH_COMPLETION_CONFIRMED) {
                completion = PTOUCH_USB_COMPLETION_OK;
                goto done;
            }
        }
    }

done:
    if (!retain_resources) {
        usb_host_transfer_free(rx);
        vSemaphoreDelete(sem);
    }
    return completion;
}

ptouch_usb_print_result_t ptouch_usb_print_and_wait(
    const uint8_t *data, size_t len,
    int transfer_timeout_ms, int completion_timeout_ms)
{
    ptouch_usb_print_result_t result = {
        .transfer_rc = PTOUCH_USB_TRANSFER_INVALID_ARGUMENT,
        .completion_rc = PTOUCH_USB_COMPLETION_NOT_RUN,
    };
    if (!s_io_mtx) return result;

    xSemaphoreTake(s_io_mtx, portMAX_DELAY);
    if (transport_faulted()) {
        result.transfer_rc = PTOUCH_USB_TRANSFER_TRANSPORT_FAULT;
        xSemaphoreGive(s_io_mtx);
        return result;
    }
    if (status_commands_blocked_locked()) {
        result.transfer_rc = PTOUCH_USB_TRANSFER_BLOCKED;
        xSemaphoreGive(s_io_mtx);
        return result;
    }
    if (s_dev_gone_pending || s_profile != ptouch_model_p710bt()) {
        result.transfer_rc = s_dev_gone_pending
                                 ? PTOUCH_USB_TRANSFER_NOT_ATTACHED
                                 : PTOUCH_USB_TRANSFER_UNSUPPORTED;
        xSemaphoreGive(s_io_mtx);
        return result;
    }
    ptouch_completion_tracker_t tracker;
    ptouch_completion_tracker_init(&tracker);

    /* Establish an RX epoch before the first raster byte. The status request
     * drains every automatic frame queued by an earlier job and returns only
     * after a fresh idle reply. Holding s_io_mtx across this preflight, OUT,
     * and completion wait prevents another caller from creating ambiguity. */
    ptouch_status_t preflight;
    const char *preflight_error = status_txn_locked(&preflight, false);
    result.has_status = preflight.valid;
    if (preflight.valid) result.status = preflight;
    if (preflight_error || !ptouch_status_is_idle_ready(&preflight)) {
        result.transfer_rc = PTOUCH_USB_TRANSFER_PREFLIGHT_FAILED;
    } else {
        ptouch_completion_tracker_arm_for_strategy(
            &tracker, s_profile ? s_profile->completion_strategy
                                : PTOUCH_COMPLETION_STRICT_NOTIFICATIONS);
        result.transfer_rc = bulk_out_locked(
            data, len, transfer_timeout_ms, &result.stream_submitted);
    }
    if (result.transfer_rc == PTOUCH_USB_TRANSFER_OK) {
        result.completion_rc = wait_for_print_completion_locked(
            completion_timeout_ms, &result, &tracker);
        if (result.completion_rc == PTOUCH_USB_COMPLETION_OK &&
            !s_profile->completion_validated) {
            /* A non-completion-validated profile may produce inspected output,
             * but nominal JOB_STATUS cannot prove current-label completion. */
            result.completion_rc = PTOUCH_USB_COMPLETION_UNVALIDATED;
        }
    }
    if (result.stream_submitted &&
        (result.transfer_rc != PTOUCH_USB_TRANSFER_OK ||
         result.completion_rc != PTOUCH_USB_COMPLETION_OK)) {
        /* Latch before releasing the lease so a queued observer cannot
         * issue ESC @ into a physically ambiguous printer state. */
        quarantine_output();
    }
    xSemaphoreGive(s_io_mtx);
    return result;
}

ptouch_usb_print_result_t ptouch_usb_print_job_and_wait(
    const ptouch_print_job_t *job,
    int transfer_timeout_ms, int completion_timeout_ms)
{
    ptouch_usb_print_result_t result = {
        .transfer_rc = PTOUCH_USB_TRANSFER_INVALID_ARGUMENT,
        .completion_rc = PTOUCH_USB_COMPLETION_NOT_RUN,
    };
    if (!s_io_mtx || !ptouch_print_job_frames_valid(job)) return result;

    xSemaphoreTake(s_io_mtx, portMAX_DELAY);
    if (transport_faulted()) {
        result.transfer_rc = PTOUCH_USB_TRANSFER_TRANSPORT_FAULT;
        goto done;
    }
    if (status_commands_blocked_locked()) {
        result.transfer_rc = PTOUCH_USB_TRANSFER_BLOCKED;
        goto done;
    }
    if (s_dev_gone_pending) {
        result.transfer_rc = PTOUCH_USB_TRANSFER_NOT_ATTACHED;
        goto done;
    }
    if (!profile_output_allowed(s_profile)) {
        result.transfer_rc = PTOUCH_USB_TRANSFER_UNSUPPORTED;
        goto done;
    }
    if (!s_intf_claimed || !s_dev) {
        result.transfer_rc = PTOUCH_USB_TRANSFER_NOT_ATTACHED;
        goto done;
    }
    if (job->profile != s_profile ||
        job->attachment_generation != s_attachment_generation) {
        result.transfer_rc = PTOUCH_USB_TRANSFER_TARGET_CHANGED;
        goto done;
    }

    ptouch_status_t preflight;
    const char *preflight_error = status_txn_locked(&preflight, false);
    result.has_status = preflight.valid;
    if (preflight.valid) result.status = preflight;

    ptouch_media_geometry_t geometry;
    bool media_ok = preflight.valid &&
        preflight.media_width_mm == job->tape_mm &&
        ptouch_model_accepts_media(s_profile, preflight.media_width_mm,
                                   preflight.media_type) &&
        ptouch_media_geometry(s_profile, preflight.media_width_mm, &geometry);
    bool model_ok = !s_profile->expected_status_model ||
                    preflight.model == s_profile->expected_status_model;
    if (s_dev_gone_pending) {
        result.transfer_rc = PTOUCH_USB_TRANSFER_NOT_ATTACHED;
        goto done;
    }
    if (job->profile != s_profile ||
        job->attachment_generation != s_attachment_generation) {
        result.transfer_rc = PTOUCH_USB_TRANSFER_TARGET_CHANGED;
        goto done;
    }
    if (preflight_error || !ptouch_status_is_idle_ready(&preflight) ||
        !media_ok || !model_ok) {
        result.transfer_rc = PTOUCH_USB_TRANSFER_PREFLIGHT_FAILED;
        goto done;
    }

    ptouch_completion_tracker_t tracker;
    ptouch_completion_tracker_init(&tracker);
    ptouch_completion_tracker_arm_for_strategy(
        &tracker, s_profile->completion_strategy);
    result.transfer_rc = print_job_out_locked(
        job, transfer_timeout_ms, &result.stream_submitted);
    if (result.transfer_rc == PTOUCH_USB_TRANSFER_OK) {
        result.completion_rc = wait_for_print_completion_locked(
            completion_timeout_ms, &result, &tracker);
        if (result.completion_rc == PTOUCH_USB_COMPLETION_OK &&
            !s_profile->completion_validated) {
            result.completion_rc = PTOUCH_USB_COMPLETION_UNVALIDATED;
        }
    }

done:
    if (result.stream_submitted &&
        (result.transfer_rc != PTOUCH_USB_TRANSFER_OK ||
         result.completion_rc != PTOUCH_USB_COMPLETION_OK)) {
        quarantine_output();
    }
    xSemaphoreGive(s_io_mtx);
    return result;
}

ptouch_usb_print_result_t ptouch_usb_print_bitmap(
    const uint8_t *px, int w, int h,
    const ptouch_print_opts_t *opts,
    int completion_timeout_ms)
{
    ptouch_usb_print_result_t result = {
        .transfer_rc = PTOUCH_USB_TRANSFER_INVALID_ARGUMENT,
        .completion_rc = PTOUCH_USB_COMPLETION_NOT_RUN,
    };
    if (!px || !opts || w <= 0 || h <= 0 || completion_timeout_ms <= 0) {
        return result;
    }

    ptouch_usb_target_t target;
    if (!ptouch_usb_target_snapshot(&target)) {
        result.transfer_rc = PTOUCH_USB_TRANSFER_NOT_ATTACHED;
        return result;
    }

    ptouch_print_opts_t effective = *opts;

    ptouch_print_job_t job;
    ptouch_print_job_init(&job);
    if (!ptouch_print_build_job_for_model(target.profile, px, w, h,
                                          &effective, &job)) {
        ptouch_print_job_free(&job);
        result.transfer_rc = PTOUCH_USB_TRANSFER_BUILD_FAILED;
        return result;
    }
    job.attachment_generation = target.generation;
    result = ptouch_usb_print_job_and_wait(
        &job, effective.timeout_ms, completion_timeout_ms);
    ptouch_print_job_free(&job);
    return result;
}

static const char *status_txn_locked(ptouch_status_t *st,
                                     bool after_inspection)
{
    SemaphoreHandle_t sem = NULL;
    usb_transfer_t *tx = NULL, *rx = NULL;
    bool tx_retained = false, rx_retained = false;
    ptouch_buf_t cmd; ptouch_buf_init(&cmd);
    const char *fail = NULL;
    int rxlen = 0;
    int frames_seen = 0;

    if (!st) return "missing status output";
    memset(st, 0, sizeof *st);
    if (transport_faulted()) {
        return "USB transport faulted; reboot required";
    }
    if (status_commands_blocked_locked() && !after_inspection) {
        return "status command blocked pending tape inspection";
    }
    if (s_dev_gone_pending || !s_intf_claimed || !s_dev) {
        fail = "no printer claimed (is it attached?)";
        goto done;
    }

    sem = xSemaphoreCreateBinary();
    ptouch_build_invalidate(&cmd);
    ptouch_build_init(&cmd);
    ptouch_build_status_request(&cmd);
    if (!sem || cmd.oom) { fail = "alloc failed"; goto done; }

    if (usb_host_transfer_alloc(cmd.len, 0, &tx) != ESP_OK) { fail = "OUT alloc failed"; goto done; }
    memcpy(tx->data_buffer, cmd.data, cmd.len);
    tx->num_bytes = (int)cmd.len;
    tx->bEndpointAddress = s_bulk_out_ep;
    if (submit_and_wait(tx, sem, 2000, NULL, false, &tx_retained) != XFER_OK) {
        fail = "OUT transfer failed";
        goto done;
    }

    rxlen = ((PTOUCH_STATUS_LENGTH + s_bulk_in_mps - 1) / s_bulk_in_mps) * s_bulk_in_mps;
    if (usb_host_transfer_alloc(rxlen, 0, &rx) != ESP_OK) { fail = "IN alloc failed"; goto done; }
    /* Automatic print notifications are FIFO-queued ahead of the reply to this
     * request. Drain both short packets and full stale notifications until the
     * current reply (or an error) arrives. */
    for (;;) {
        rx->num_bytes = rxlen;
        rx->bEndpointAddress = s_bulk_in_ep;
        if (submit_and_wait(rx, sem, 3000, NULL, false, &rx_retained) != XFER_OK) {
            fail = "IN transfer failed (no reply)";
            goto done;
        }
        if (rx->actual_num_bytes < PTOUCH_STATUS_LENGTH) {
            if (++frames_seen >= 16) { fail = "status reply not received"; goto done; }
            continue;
        }
        for (int off = 0;
             off + PTOUCH_STATUS_LENGTH <= rx->actual_num_bytes;
             off += PTOUCH_STATUS_LENGTH) {
            ptouch_status_t candidate;
            ptouch_parse_status(rx->data_buffer + off, &candidate);
            frames_seen++;
            if (!candidate.valid) continue;
            if (candidate.status_type == PTOUCH_ST_REPLY_TO_REQUEST ||
                ptouch_status_reports_error(&candidate)) {
                if (s_profile && s_profile->expected_status_model &&
                    candidate.model != s_profile->expected_status_model) {
                    fail = "status model does not match USB profile";
                    goto done;
                }
                *st = candidate;
                goto done;
            }
            ESP_LOGD(TAG, "drained stale status type=0x%02x before idle reply",
                     candidate.status_type);
        }
        if (frames_seen >= 16) { fail = "status reply not received"; goto done; }
    }

done:
    if (tx && !tx_retained) usb_host_transfer_free(tx);
    if (rx && !rx_retained) usb_host_transfer_free(rx);
    if (sem && !tx_retained && !rx_retained) vSemaphoreDelete(sem);
    ptouch_buf_free(&cmd);
    return fail;
}

bool ptouch_usb_ready_snapshot(ptouch_usb_ready_t *out)
{
    if (!out || !s_io_mtx) return false;
    memset(out, 0, sizeof *out);
    xSemaphoreTake(s_io_mtx, portMAX_DELAY);
    uint32_t generation = s_attachment_generation;
    const ptouch_model_profile_t *profile = s_profile;
    bool ready = !s_dev_gone_pending && s_intf_claimed && s_dev &&
                 profile_output_allowed(profile);
    ptouch_status_t status;
    memset(&status, 0, sizeof status);
    if (ready) {
        ready = status_txn_locked(&status, false) == NULL &&
                !s_dev_gone_pending && s_intf_claimed && s_dev &&
                s_attachment_generation == generation &&
                s_profile == profile &&
                ptouch_status_is_idle_ready(&status) &&
                ptouch_model_accepts_media(profile, status.media_width_mm,
                                           status.media_type) &&
                ptouch_media_geometry(profile, status.media_width_mm,
                                      &out->geometry);
    }
    if (ready) {
        out->target.profile = profile;
        out->target.vid = s_device_info.vid;
        out->target.pid = s_device_info.pid;
        out->target.generation = generation;
        out->status = status;
        publish_status_cache_locked(&status, generation);
    }
    xSemaphoreGive(s_io_mtx);
    return ready;
}

bool ptouch_usb_cached_ready_snapshot(ptouch_usb_ready_t *out)
{
    if (!out || !s_io_mtx || !s_cache_mtx) return false;
    memset(out, 0, sizeof *out);

    xSemaphoreTake(s_io_mtx, portMAX_DELAY);
    uint32_t generation = s_attachment_generation;
    const ptouch_model_profile_t *profile = s_profile;
    bool ready = !ptouch_usb_status_commands_blocked() &&
                 !s_dev_gone_pending && s_intf_claimed && s_dev &&
                 profile_output_allowed(profile);
    ptouch_status_t status;
    memset(&status, 0, sizeof status);
    if (ready) {
        int64_t ttl_us =
            (int64_t)(s_cfg.status_cache_ms > 0 ? s_cfg.status_cache_ms : 15000) *
            1000;
        xSemaphoreTake(s_cache_mtx, portMAX_DELAY);
        if (s_status_cache_us < 0 ||
            s_status_cache_generation != generation ||
            esp_timer_get_time() - s_status_cache_us >= ttl_us) {
            ready = false;
        } else {
            status = s_status_cache;
        }
        xSemaphoreGive(s_cache_mtx);
    }
    if (ready) {
        ready = status.valid && ptouch_status_is_idle_ready(&status) &&
                ptouch_model_accepts_media(profile, status.media_width_mm,
                                           status.media_type) &&
                ptouch_media_geometry(profile, status.media_width_mm,
                                      &out->geometry);
    }
    if (ready) {
        out->target.profile = profile;
        out->target.vid = s_device_info.vid;
        out->target.pid = s_device_info.pid;
        out->target.generation = generation;
        out->status = status;
    }
    xSemaphoreGive(s_io_mtx);
    return ready;
}

const char *ptouch_usb_status_txn(ptouch_status_t *st)
{
    if (!st) return "missing status output";
    memset(st, 0, sizeof *st);
    if (!s_io_mtx) return "USB not initialised";

    xSemaphoreTake(s_io_mtx, portMAX_DELAY);
    const char *fail = status_txn_locked(st, false);
    xSemaphoreGive(s_io_mtx);
    return fail;
}

const char *ptouch_usb_status_txn_after_inspection(ptouch_status_t *st)
{
    if (!st) return "missing status output";
    memset(st, 0, sizeof *st);
    if (!s_io_mtx) return "USB not initialised";

    xSemaphoreTake(s_io_mtx, portMAX_DELAY);
    const char *fail = status_txn_locked(st, true);
    xSemaphoreGive(s_io_mtx);
    return fail;
}

/* Keep short-lived observability stable across transient idle transaction
 * failures. Print completion itself owns s_io_mtx, so this function cannot
 * issue a forbidden command during Brother's mechanical printing window. A
 * real unplug drops s_intf_claimed and bypasses the cache immediately. */
bool ptouch_usb_status(ptouch_status_t *st)
{
    if (!st) return false;
    memset(st, 0, sizeof *st);
    bool fresh = false;
    if (s_io_mtx && xSemaphoreTake(s_io_mtx, 0) == pdTRUE) {
        uint32_t generation = s_attachment_generation;
        fresh = !s_dev_gone_pending && status_txn_locked(st, false) == NULL &&
                !s_dev_gone_pending &&
                generation == s_attachment_generation;
        if (fresh) publish_status_cache_locked(st, generation);
        xSemaphoreGive(s_io_mtx);
    }
    if (fresh) return true;
    if (ptouch_usb_status_commands_blocked()) return false;
    bool served = false;
    if (!s_dev_gone_pending && s_intf_claimed && s_dev && s_cache_mtx) {
        int64_t ttl_us = (int64_t)(s_cfg.status_cache_ms > 0 ? s_cfg.status_cache_ms : 15000) * 1000;
        xSemaphoreTake(s_cache_mtx, portMAX_DELAY);
        if (s_status_cache_us >= 0 &&
            s_status_cache_generation == s_attachment_generation &&
            esp_timer_get_time() - s_status_cache_us < ttl_us) {
            *st = s_status_cache;
            served = true;
        }
        xSemaphoreGive(s_cache_mtx);
    }
    return served;
}

/* -------------------------------------------------------- host tasks ------ */
static void client_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ESP_LOGI(TAG, "USB device connected (addr %d)", msg->new_dev.address);
        /* Enumeration eventually claims the interface and therefore shares
         * the same lease as I/O. Defer it out of this callback so transfer
         * callbacks cannot be starved while another task owns that lease. */
        if (s_new_dev_addr_pending < 0) {
            s_new_dev_addr_pending = msg->new_dev.address;
        } else {
            ESP_LOGW(TAG, "preserving earlier pending USB device address");
        }
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        if (msg->dev_gone.dev_hdl != s_observed_dev) {
            ESP_LOGW(TAG, "ignoring removal of a non-target USB device");
            break;
        }
        ESP_LOGW(TAG, "USB target disconnected");
        /* This callback runs inside usb_host_client_handle_events(), which also
         * delivers transfer callbacks. Never block here on the I/O mutex: an
         * in-flight waiter may need this same client task to receive its
         * NO_DEVICE callback before it can release that mutex. Mark detached
         * immediately, then let usb_task clean up once the lease is available. */
        /* Do not mutate the profile/claim while an I/O owner may still use it.
         * The gone flag makes new snapshots fail immediately; usb_task clears
         * the full context after acquiring the transaction lease. */
        s_dev_gone_pending = true;
        break;
    default:
        break;
    }
}

static void host_lib_task(void *arg)
{
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

static void usb_task(void *arg)
{
    if (s_cfg.boot_delay_ms > 0) vTaskDelay(pdMS_TO_TICKS(s_cfg.boot_delay_ms));

    ESP_LOGW(TAG, "installing USB host library (USB-Serial-JTAG console goes away now)");
    usb_host_config_t hc = { 0 };
    hc.skip_phy_setup = false;
    hc.intr_flags = ESP_INTR_FLAG_LEVEL1;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    hc.peripheral_map = BIT0;   /* the S2/S3's single OTG peripheral (field is IDF >= 6) */
#endif
    esp_err_t err = usb_host_install(&hc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        emit(PTOUCH_USB_EVT_HOST_FAILED, NULL);
        vTaskDelete(NULL);
        return;
    }
    TaskHandle_t host_lib_handle = NULL;
    if (xTaskCreate(host_lib_task, "ptouch_usblib", 4096, NULL, 2,
                    &host_lib_handle) != pdPASS) {
        ESP_LOGE(TAG, "USB host-library task allocation failed");
        usb_host_uninstall();
        emit(PTOUCH_USB_EVT_HOST_FAILED, NULL);
        vTaskDelete(NULL);
        return;
    }

    usb_host_client_config_t cc = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = { .client_event_callback = client_cb, .callback_arg = NULL },
    };
    err = usb_host_client_register(&cc, &s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_client_register failed: %s", esp_err_to_name(err));
        vTaskDelete(host_lib_handle);
        usb_host_uninstall();
        emit(PTOUCH_USB_EVT_HOST_FAILED, NULL);
        vTaskDelete(NULL);
        return;
    }
    /* HOST_UP is the allocation barrier consumed by the application: the
     * library task and client registration both exist before callbacks run. */
    emit(PTOUCH_USB_EVT_HOST_UP, NULL);
    for (;;) {
        TickType_t wait = (s_dev_gone_pending || s_new_dev_addr_pending >= 0)
                              ? pdMS_TO_TICKS(50)
                              : portMAX_DELAY;
        usb_host_client_handle_events(s_client, wait);

        if (s_dev_gone_pending && s_io_mtx &&
            xSemaphoreTake(s_io_mtx, 0) == pdTRUE) {
            if (s_observed_dev) {
                if (s_intf_claimed && s_dev) {
                    usb_host_interface_release(s_client, s_dev, s_intf_num);
                }
                usb_host_device_close(s_client, s_observed_dev);
            }
            s_dev = NULL;
            s_observed_dev = NULL;
            s_intf_claimed = false;
            s_device_detected = false;
            s_device_info.claimed = false;
            s_profile = NULL;
            ++s_attachment_generation;
            if (s_cache_mtx) {
                xSemaphoreTake(s_cache_mtx, portMAX_DELAY);
                s_status_cache_us = -1;
                s_status_cache_generation = 0;
                xSemaphoreGive(s_cache_mtx);
            }
            s_dev_gone_pending = false;
            emit(PTOUCH_USB_EVT_DETACHED, NULL);
            xSemaphoreGive(s_io_mtx);
        }

        if (!s_dev_gone_pending && s_new_dev_addr_pending >= 0 && s_io_mtx &&
            xSemaphoreTake(s_io_mtx, 0) == pdTRUE) {
            int addr = s_new_dev_addr_pending;
            s_new_dev_addr_pending = -1;
            report_device_locked((uint8_t)addr);
            xSemaphoreGive(s_io_mtx);
        }
    }
}

esp_err_t ptouch_usb_start(const ptouch_usb_config_t *cfg)
{
    taskENTER_CRITICAL(&s_start_lock);
    if (s_start_called) {
        taskEXIT_CRITICAL(&s_start_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_start_called = true;
    taskEXIT_CRITICAL(&s_start_lock);

    s_cfg = cfg ? *cfg : PTOUCH_USB_CONFIG_DEFAULT();
    s_io_mtx = xSemaphoreCreateMutex();
    s_cache_mtx = xSemaphoreCreateMutex();
    s_event_queue = xQueueCreate(8, sizeof(ptouch_usb_event_message_t));
    if (!s_io_mtx || !s_cache_mtx || !s_event_queue) goto no_mem;
    if (xTaskCreate(event_task, "ptouch_event", 3072, NULL, 2,
                    &s_event_task_handle) != pdPASS) {
        goto no_mem;
    }
    if (xTaskCreate(usb_task, "ptouch_usb", 5120, NULL, 3, NULL) != pdPASS) {
        vTaskDelete(s_event_task_handle);
        s_event_task_handle = NULL;
        goto no_mem;
    }
    return ESP_OK;

no_mem:
    if (s_event_queue) {
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
    }
    if (s_cache_mtx) {
        vSemaphoreDelete(s_cache_mtx);
        s_cache_mtx = NULL;
    }
    if (s_io_mtx) {
        vSemaphoreDelete(s_io_mtx);
        s_io_mtx = NULL;
    }
    taskENTER_CRITICAL(&s_start_lock);
    s_start_called = false;
    taskEXIT_CRITICAL(&s_start_lock);
    return ESP_ERR_NO_MEM;
}
