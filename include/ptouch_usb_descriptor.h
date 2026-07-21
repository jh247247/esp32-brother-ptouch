/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Jack Hosemans
 *
 * Portable, fail-closed USB descriptor selection for Brother printers.
 */
#ifndef PTOUCH_USB_DESCRIPTOR_H
#define PTOUCH_USB_DESCRIPTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t interface_number;
    uint8_t alternate_setting;
    uint8_t bulk_out_ep;
    uint8_t bulk_in_ep;
    uint16_t bulk_out_mps;
    uint16_t bulk_in_mps;
} ptouch_usb_bulk_pair_t;

/* Select the single alternate-setting-zero interface containing Brother's
 * exact bulk OUT 0x02 and bulk IN 0x81 endpoints. The complete configuration
 * descriptor must be structurally valid; malformed, split-interface,
 * zero-packet-size, or ambiguous pairs are rejected. */
bool ptouch_usb_find_printer_bulk_pair(const uint8_t *descriptor,
                                       size_t available_len,
                                       ptouch_usb_bulk_pair_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PTOUCH_USB_DESCRIPTOR_H */
