/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Jack Hosemans
 */
#include "ptouch_usb_descriptor.h"

enum {
    USB_CONFIGURATION_DESCRIPTOR = 0x02,
    USB_INTERFACE_DESCRIPTOR = 0x04,
    USB_ENDPOINT_DESCRIPTOR = 0x05,
    USB_TRANSFER_TYPE_MASK = 0x03,
    USB_TRANSFER_TYPE_BULK = 0x02,
    PTOUCH_BULK_OUT = 0x02,
    PTOUCH_BULK_IN = 0x81,
};

bool ptouch_usb_find_printer_bulk_pair(const uint8_t *descriptor,
                                       size_t available_len,
                                       ptouch_usb_bulk_pair_t *out)
{
    if (!descriptor || !out || available_len < 9 || descriptor[0] < 9 ||
        descriptor[1] != USB_CONFIGURATION_DESCRIPTOR) {
        return false;
    }
    size_t total_len = (size_t)descriptor[2] |
                       ((size_t)descriptor[3] << 8);
    if (total_len < descriptor[0] || total_len > available_len) return false;

    ptouch_usb_bulk_pair_t candidate = {0};
    bool found = false;
    bool have_interface = false;
    bool current_recorded = false;
    uint8_t current_interface = 0;
    uint8_t current_alt = 0;
    uint8_t current_out = 0;
    uint8_t current_in = 0;
    uint16_t current_out_mps = 0;
    uint16_t current_in_mps = 0;

    for (size_t offset = 0; offset < total_len; ) {
        if (offset + 2 > total_len) return false;
        uint8_t length = descriptor[offset];
        uint8_t type = descriptor[offset + 1];
        if (length < 2 || offset + length > total_len) return false;

        if (type == USB_INTERFACE_DESCRIPTOR) {
            if (length < 9) return false;
            have_interface = true;
            current_recorded = false;
            current_interface = descriptor[offset + 2];
            current_alt = descriptor[offset + 3];
            current_out = 0;
            current_in = 0;
            current_out_mps = 0;
            current_in_mps = 0;
        } else if (type == USB_ENDPOINT_DESCRIPTOR) {
            if (length < 7 || !have_interface) return false;
            uint8_t address = descriptor[offset + 2];
            uint8_t attributes = descriptor[offset + 3];
            uint16_t mps = (uint16_t)(
                (uint16_t)descriptor[offset + 4] |
                (uint16_t)((uint16_t)descriptor[offset + 5] << 8));
            mps &= 0x07FFU;
            if ((attributes & USB_TRANSFER_TYPE_MASK) ==
                USB_TRANSFER_TYPE_BULK) {
                if (mps == 0 || mps > 1024) return false;
                if (address == PTOUCH_BULK_OUT) {
                    if (current_out) return false;
                    current_out = address;
                    current_out_mps = mps;
                } else if (address == PTOUCH_BULK_IN) {
                    if (current_in) return false;
                    current_in = address;
                    current_in_mps = mps;
                }
                if (!current_recorded && current_alt == 0 &&
                    current_out == PTOUCH_BULK_OUT &&
                    current_in == PTOUCH_BULK_IN) {
                    if (found) return false;
                    candidate = (ptouch_usb_bulk_pair_t) {
                        .interface_number = current_interface,
                        .alternate_setting = current_alt,
                        .bulk_out_ep = current_out,
                        .bulk_in_ep = current_in,
                        .bulk_out_mps = current_out_mps,
                        .bulk_in_mps = current_in_mps,
                    };
                    found = true;
                    current_recorded = true;
                }
            }
        }
        offset += length;
    }

    if (!found) return false;
    *out = candidate;
    return true;
}
