/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Jack Hosemans
 *
 * ptouch_print.c — assemble + send a full P-touch print job from a bitmap.
 * See ptouch_print.h. Command sequence follows the Brother "Raster Command
 * Reference" (PT-E550W/P750W/P710BT), verified byte-exact on hardware.
 */
#include "ptouch_print.h"
#include "ptouch_usb.h"

#include <string.h>

bool ptouch_print_build_stream(const uint8_t *px, int w, int h,
                               const ptouch_print_opts_t *opts,
                               ptouch_buf_t *out)
{
    ptouch_print_opts_t o = opts ? *opts : PTOUCH_PRINT_OPTS_DEFAULT();
    if (!px || w <= 0 || h <= 0 || h > PTOUCH_PRINT_HEAD_PINS) return false;
    if (o.trailing_pad_dots < 0) o.trailing_pad_dots = 0;

    const int offset = (PTOUCH_PRINT_HEAD_PINS - h) / 2;

    /* Mechanical minimum: content shorter than min_length_dots is centered by
     * splitting the shortfall into leading/trailing blank columns (the printer
     * would otherwise append it all as a blank tail to reach the cutter). */
    int lead_pad = 0, tail_pad = o.chain ? 0 : o.trailing_pad_dots;
    if (tail_pad < 0) tail_pad = 0;
    if (!o.chain && o.min_length_dots > 0) {
        int shortfall = o.min_length_dots - (w + tail_pad);
        if (shortfall > 0) {
            lead_pad  = shortfall / 2;
            tail_pad += shortfall - lead_pad;
        }
    }

    ptouch_build_invalidate(out);
    ptouch_build_init(out);
    ptouch_build_raster_mode(out);
    ptouch_build_enable_status_notification(out);
    ptouch_build_print_info(out, (uint8_t)o.tape_mm,
                            (uint32_t)(lead_pad + w + tail_pad));
    ptouch_build_mode(out, o.autocut ? PTOUCH_MODE_AUTO_CUT : 0);
    ptouch_build_advanced_mode(out, PTOUCH_ADV_NO_CHAIN);
    ptouch_build_margin(out, 0);
    ptouch_build_compression_mode(out);

    {   /* centering pad for short labels (see min_length_dots) */
        uint8_t blank[PTOUCH_LINE_LENGTH_BYTES] = {0};
        for (int x = 0; x < lead_pad; x++) ptouch_build_raster_line(out, blank);
    }

    for (int x = 0; x < w; x++) {
        int sx = o.mirror ? (w - 1 - x) : x;
        uint8_t line[PTOUCH_LINE_LENGTH_BYTES] = {0};
        for (int y = 0; y < h; y++) {
            if (px[(size_t)y * (size_t)w + (size_t)sx]) {
                int dot = offset + y;
                line[dot >> 3] |= (uint8_t)(0x80 >> (dot & 7));
            }
        }
        ptouch_build_raster_line(out, line);
    }

    /* End padding — cut-edge protection (trailing_pad_dots) plus the tail half
     * of the min-length centering. Chained pages have neither: there is no cut
     * to pad against, and padding would gap back-to-back labels. */
    {
        uint8_t blank[PTOUCH_LINE_LENGTH_BYTES] = {0};
        for (int x = 0; x < tail_pad; x++) ptouch_build_raster_line(out, blank);
    }

    ptouch_build_print(out, o.chain ? PTOUCH_PRINT_CHAIN : PTOUCH_PRINT_FEED);
    return !out->oom;
}

int ptouch_print_bitmap(const uint8_t *px, int w, int h,
                        const ptouch_print_opts_t *opts)
{
    ptouch_print_opts_t o = opts ? *opts : PTOUCH_PRINT_OPTS_DEFAULT();
    ptouch_buf_t stream;
    ptouch_buf_init(&stream);
    int rc;
    if (!ptouch_print_build_stream(px, w, h, &o, &stream)) {
        rc = -10;
    } else {
        rc = ptouch_usb_bulk_out(stream.data, stream.len, o.timeout_ms);
    }
    ptouch_buf_free(&stream);
    return rc;
}
