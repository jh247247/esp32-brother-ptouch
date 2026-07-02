/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Jack Hosemans
 *
 * ptouch_usb.c — ESP32 USB-host transport for Brother P-touch printers.
 * See ptouch_usb.h for the API and the USB-PHY caveat.
 */
#include "ptouch_usb.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_intr_alloc.h"
#include "esp_idf_version.h"
#include "esp_bit_defs.h"
#include "usb/usb_host.h"

static const char *TAG = "ptouch_usb";

static ptouch_usb_config_t s_cfg;
static usb_host_client_handle_t s_client;

/* Persisted device context for bulk I/O. Set at enumeration, cleared on
 * disconnect. Guarded by s_io_mtx while a transaction runs so a mid-transfer
 * unplug can't free the device out from under us. */
static usb_device_handle_t s_dev;
static uint8_t   s_intf_num;
static uint8_t   s_bulk_out_ep, s_bulk_in_ep;
static uint16_t  s_bulk_in_mps = 64;
static volatile bool s_intf_claimed;
static SemaphoreHandle_t s_io_mtx;
static SemaphoreHandle_t s_cache_mtx;

static void emit(ptouch_usb_event_t evt, const ptouch_usb_devinfo_t *info)
{
    if (s_cfg.on_event) s_cfg.on_event(evt, info, s_cfg.user_ctx);
}

bool ptouch_usb_attached(void) { return s_intf_claimed; }

/* ------------------------------------------------------- enumeration ------ */
static void report_device(uint8_t addr)
{
    usb_device_handle_t dev;
    esp_err_t err = usb_host_device_open(s_client, addr, &dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "device_open(%d) failed: %s", addr, esp_err_to_name(err));
        return;
    }

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

    int bulk_out = -1, bulk_in = -1, bulk_intf = -1, cur_intf = -1;
    uint16_t bulk_in_mps = 64;
    const usb_config_desc_t *cfg = NULL;
    if (usb_host_get_active_config_descriptor(dev, &cfg) == ESP_OK && cfg) {
        if (s_cfg.dump_descriptors) usb_print_config_descriptor(cfg, NULL);
        const uint8_t *p = (const uint8_t *)cfg;
        const uint8_t *end = p + cfg->wTotalLength;
        while (p + 2 <= end) {
            uint8_t blen = p[0], btype = p[1];
            if (blen == 0) break;
            if (btype == USB_B_DESCRIPTOR_TYPE_INTERFACE && blen >= sizeof(usb_intf_desc_t)) {
                cur_intf = ((const usb_intf_desc_t *)p)->bInterfaceNumber;
            } else if (btype == USB_B_DESCRIPTOR_TYPE_ENDPOINT && blen >= sizeof(usb_ep_desc_t)) {
                const usb_ep_desc_t *e = (const usb_ep_desc_t *)p;
                if ((e->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) ==
                    USB_BM_ATTRIBUTES_XFER_BULK) {
                    if (e->bEndpointAddress & 0x80) {
                        if (bulk_in < 0) { bulk_in = e->bEndpointAddress;
                                           bulk_in_mps = e->wMaxPacketSize & 0x07FF; }
                    } else {
                        if (bulk_out < 0) { bulk_out = e->bEndpointAddress; bulk_intf = cur_intf; }
                    }
                }
            }
            p += blen;
        }
    }

    ESP_LOGI(TAG, "device VID:0x%04X PID:0x%04X%s speed=%s bulkOUT=0x%02X bulkIN=0x%02X",
             vid, pid, vid == 0x04F9 ? " (Brother)" : "", speed_str,
             bulk_out & 0xFF, bulk_in & 0xFF);

    if (bulk_out >= 0 && bulk_in >= 0 && bulk_intf >= 0) {
        if (s_io_mtx) xSemaphoreTake(s_io_mtx, portMAX_DELAY);
        s_dev         = dev;
        s_intf_num    = (uint8_t)bulk_intf;
        s_bulk_out_ep = (uint8_t)bulk_out;
        s_bulk_in_ep  = (uint8_t)bulk_in;
        s_bulk_in_mps = bulk_in_mps ? bulk_in_mps : 64;
        esp_err_t ce = usb_host_interface_claim(s_client, dev, s_intf_num, 0);
        s_intf_claimed = (ce == ESP_OK);
        if (s_io_mtx) xSemaphoreGive(s_io_mtx);
        if (s_intf_claimed) {
            ESP_LOGI(TAG, "claimed interface %u (bulkOUT 0x%02x, bulkIN 0x%02x mps=%u)",
                     s_intf_num, s_bulk_out_ep, s_bulk_in_ep, s_bulk_in_mps);
            ptouch_usb_devinfo_t di = {
                .vid = vid, .pid = pid,
                .bulk_out_ep = s_bulk_out_ep, .bulk_in_ep = s_bulk_in_ep,
                .speed = speed_str,
            };
            emit(PTOUCH_USB_EVT_ATTACHED, &di);
        } else {
            ESP_LOGE(TAG, "interface_claim(%u) failed: %s", s_intf_num, esp_err_to_name(ce));
        }
    } else {
        ESP_LOGW(TAG, "device has no bulk IN/OUT pair — not a printer? ignoring");
        usb_host_device_close(s_client, dev);
    }
}

/* -------------------------------------------------------- bulk I/O -------- */
/* A transfer's callback runs in the USB client task; it just wakes the caller. */
static void xfer_cb(usb_transfer_t *t)
{
    xSemaphoreGive((SemaphoreHandle_t)t->context);
}

static bool submit_and_wait(usb_transfer_t *t, SemaphoreHandle_t sem, int timeout_ms)
{
    t->callback = xfer_cb;
    t->context = sem;
    t->device_handle = s_dev;
    esp_err_t e = usb_host_transfer_submit(t);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "submit ep 0x%02x: %s", t->bEndpointAddress, esp_err_to_name(e));
        return false;
    }
    if (xSemaphoreTake(sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "ep 0x%02x timeout — flushing", t->bEndpointAddress);
        usb_host_endpoint_flush(s_dev, t->bEndpointAddress);
        xSemaphoreTake(sem, pdMS_TO_TICKS(500));   /* wait for the canceled callback */
        return false;
    }
    if (t->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "ep 0x%02x transfer status=%d", t->bEndpointAddress, t->status);
        return false;
    }
    return true;
}

int ptouch_usb_bulk_out(const uint8_t *data, size_t len, int timeout_ms)
{
    if (!s_io_mtx) return -1;
    SemaphoreHandle_t sem = NULL;
    usb_transfer_t *t = NULL;
    size_t chunk = 0;
    int rc;

    xSemaphoreTake(s_io_mtx, portMAX_DELAY);
    if (!s_intf_claimed || !s_dev) { rc = -2; goto cleanup; }

    sem = xSemaphoreCreateBinary();
    chunk = (len && len < 4096) ? len : 4096;
    if (!sem || usb_host_transfer_alloc(chunk, 0, &t) != ESP_OK) { rc = -3; goto cleanup; }

    rc = 0;
    for (size_t off = 0; off < len; ) {
        size_t n = len - off;
        if (n > chunk) n = chunk;
        memcpy(t->data_buffer, data + off, n);
        t->num_bytes = (int)n;
        t->bEndpointAddress = s_bulk_out_ep;
        if (!submit_and_wait(t, sem, timeout_ms)) { rc = -4; break; }
        off += n;
    }

cleanup:
    if (t) usb_host_transfer_free(t);
    if (sem) vSemaphoreDelete(sem);
    xSemaphoreGive(s_io_mtx);
    return rc;
}

const char *ptouch_usb_status_txn(ptouch_status_t *st)
{
    SemaphoreHandle_t sem = NULL;
    usb_transfer_t *tx = NULL, *rx = NULL;
    ptouch_buf_t cmd; ptouch_buf_init(&cmd);
    const char *fail = NULL;
    int rxlen = 0;

    if (!s_io_mtx) return "USB not initialised";
    xSemaphoreTake(s_io_mtx, portMAX_DELAY);
    if (!s_intf_claimed || !s_dev) { fail = "no printer claimed (is it attached?)"; goto done; }

    sem = xSemaphoreCreateBinary();
    ptouch_build_invalidate(&cmd);
    ptouch_build_init(&cmd);
    ptouch_build_status_request(&cmd);
    if (!sem || cmd.oom) { fail = "alloc failed"; goto done; }

    if (usb_host_transfer_alloc(cmd.len, 0, &tx) != ESP_OK) { fail = "OUT alloc failed"; goto done; }
    memcpy(tx->data_buffer, cmd.data, cmd.len);
    tx->num_bytes = (int)cmd.len;
    tx->bEndpointAddress = s_bulk_out_ep;
    if (!submit_and_wait(tx, sem, 2000)) { fail = "OUT transfer failed"; goto done; }

    rxlen = ((PTOUCH_STATUS_LENGTH + s_bulk_in_mps - 1) / s_bulk_in_mps) * s_bulk_in_mps;
    if (usb_host_transfer_alloc(rxlen, 0, &rx) != ESP_OK) { fail = "IN alloc failed"; goto done; }
    /* The IN queue can hold a stale short packet (ZLP / leftover print-phase
     * notification) ahead of our reply — with status notifications enabled the
     * printer queues unsolicited messages during prints. Re-read past them
     * instead of failing on the first short packet (seen ~13% of idle polls). */
    for (int attempt = 0; ; attempt++) {
        rx->num_bytes = rxlen;
        rx->bEndpointAddress = s_bulk_in_ep;
        if (!submit_and_wait(rx, sem, 3000)) { fail = "IN transfer failed (no reply)"; goto done; }
        if (rx->actual_num_bytes >= PTOUCH_STATUS_LENGTH) break;
        if (attempt >= 2) { fail = "short status reply"; goto done; }
    }

    ptouch_parse_status(rx->data_buffer, st);

done:
    if (tx) usb_host_transfer_free(tx);
    if (rx) usb_host_transfer_free(rx);
    if (sem) vSemaphoreDelete(sem);
    ptouch_buf_free(&cmd);
    xSemaphoreGive(s_io_mtx);
    return fail;
}

/* A print's mechanical phase (feeding/cutting) outlives the bulk-OUT transfer,
 * and the printer won't answer status requests while it runs — so transient
 * txn misses while the device is still attached are papered over with the last
 * good status. A real unplug drops s_intf_claimed via DEV_GONE and bypasses
 * the cache immediately. */
bool ptouch_usb_status(ptouch_status_t *st)
{
    static ptouch_status_t s_cache;
    static int64_t s_cache_us = -1;

    memset(st, 0, sizeof *st);
    if (ptouch_usb_status_txn(st) == NULL) {
        if (s_cache_mtx) {
            xSemaphoreTake(s_cache_mtx, portMAX_DELAY);
            s_cache = *st;
            s_cache_us = esp_timer_get_time();
            xSemaphoreGive(s_cache_mtx);
        }
        return true;
    }
    bool served = false;
    if (s_intf_claimed && s_dev && s_cache_mtx) {
        int64_t ttl_us = (int64_t)(s_cfg.status_cache_ms > 0 ? s_cfg.status_cache_ms : 15000) * 1000;
        xSemaphoreTake(s_cache_mtx, portMAX_DELAY);
        if (s_cache_us >= 0 && esp_timer_get_time() - s_cache_us < ttl_us) {
            *st = s_cache;
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
        report_device(msg->new_dev.address);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGW(TAG, "USB device disconnected");
        /* Serialize against an in-flight transaction. If one is mid-transfer,
         * its sem wait times out (≤3s) and releases the mutex, then we clean up
         * — no use-after-free of the device handle. */
        if (s_io_mtx) xSemaphoreTake(s_io_mtx, portMAX_DELAY);
        if (s_dev) {
            if (s_intf_claimed) usb_host_interface_release(s_client, s_dev, s_intf_num);
            usb_host_device_close(s_client, s_dev);
        }
        s_intf_claimed = false;
        s_dev = NULL;
        if (s_io_mtx) xSemaphoreGive(s_io_mtx);
        emit(PTOUCH_USB_EVT_DETACHED, NULL);
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
    emit(PTOUCH_USB_EVT_HOST_UP, NULL);

    xTaskCreate(host_lib_task, "ptouch_usblib", 4096, NULL, 2, NULL);

    usb_host_client_config_t cc = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = { .client_event_callback = client_cb, .callback_arg = NULL },
    };
    err = usb_host_client_register(&cc, &s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_client_register failed: %s", esp_err_to_name(err));
        emit(PTOUCH_USB_EVT_HOST_FAILED, NULL);
        vTaskDelete(NULL);
        return;
    }
    for (;;) usb_host_client_handle_events(s_client, portMAX_DELAY);
}

esp_err_t ptouch_usb_start(const ptouch_usb_config_t *cfg)
{
    s_cfg = cfg ? *cfg : PTOUCH_USB_CONFIG_DEFAULT();
    if (!s_io_mtx)    s_io_mtx    = xSemaphoreCreateMutex();
    if (!s_cache_mtx) s_cache_mtx = xSemaphoreCreateMutex();
    if (!s_io_mtx || !s_cache_mtx) return ESP_ERR_NO_MEM;
    if (xTaskCreate(usb_task, "ptouch_usb", 5120, NULL, 3, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
