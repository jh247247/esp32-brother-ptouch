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

    ptouch_build_invalidate(out);
    ptouch_build_init(out);
    ptouch_build_raster_mode(out);
    ptouch_build_enable_status_notification(out);
    ptouch_build_print_info(out, (uint8_t)o.tape_mm,
                            (uint32_t)(w + o.trailing_pad_dots));
    ptouch_build_mode(out, o.autocut ? PTOUCH_MODE_AUTO_CUT : 0);
    ptouch_build_advanced_mode(out, PTOUCH_ADV_NO_CHAIN);
    ptouch_build_margin(out, 0);
    ptouch_build_compression_mode(out);

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

    /* End padding — mirror-safe: the trailing (cut) edge is padded either way.
     * Chained pages skip it: there is no cut to pad against, and padding would
     * put a gap between back-to-back labels. */
    if (!o.chain && o.trailing_pad_dots > 0) {
        uint8_t blank[PTOUCH_LINE_LENGTH_BYTES] = {0};
        for (int x = 0; x < o.trailing_pad_dots; x++)
            ptouch_build_raster_line(out, blank);
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
