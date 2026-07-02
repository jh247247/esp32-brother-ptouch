/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Jack Hosemans
 *
 * ptouch_print.h — assemble + send a full P-touch print job from a bitmap.
 *
 * Takes a plain row-major bitmap (1 byte per pixel, non-zero = ink, width =
 * label length along the tape, height = across the tape ≤ 128) and produces
 * the complete raster command stream — invalidate → init → raster mode →
 * status notifications → print info → auto-cut mode → margin(0) → compression
 * → one PackBits raster line per bitmap COLUMN (the print head is
 * perpendicular to feed) with the rows centered on the 128-dot head — then
 * prints it over ptouch_usb.
 *
 * Behaviors verified on a PT-P710BT with 12 mm TZe tape:
 *  - trailing_pad_dots blank columns are appended before the cut; with
 *    margin(0) the cutter otherwise shaves ~1 mm off the last glyph
 *    (12 dots ≈ 1.7 mm at 180 dpi / 7.087 dots/mm).
 *  - mirror reverses the columns (for wrap-around / wire labels).
 */
#ifndef PTOUCH_PRINT_H
#define PTOUCH_PRINT_H

#include <stdbool.h>
#include <stdint.h>
#include "ptouch_raster.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  tape_mm;            /* installed tape width in mm (from the status reply) */
    bool mirror;             /* horizontal mirror (reverse feed-direction columns) */
    bool autocut;            /* cut after the job (default true) */
    int  trailing_pad_dots;  /* blank columns before the cut (default 12) */
    int  timeout_ms;         /* per-chunk USB timeout (default 15000) */
} ptouch_print_opts_t;

#define PTOUCH_PRINT_OPTS_DEFAULT() (ptouch_print_opts_t){ \
    .tape_mm = 12, .mirror = false, .autocut = true,        \
    .trailing_pad_dots = 12, .timeout_ms = 15000 }

/* Assemble the raster command stream into `out` (caller ptouch_buf_free()s it)
 * WITHOUT touching USB — usable with any transport, and host-testable.
 * px is w*h bytes, row-major, non-zero = ink. Requires 0 < h <= 128.
 * Returns false on bad args or allocation failure. */
bool ptouch_print_build_stream(const uint8_t *px, int w, int h,
                               const ptouch_print_opts_t *opts,
                               ptouch_buf_t *out);

/* Build the stream and send it via ptouch_usb_bulk_out().
 * 0 = printed; -10 = build failed (args/alloc); other negatives = USB error
 * codes from ptouch_usb_bulk_out (-2 = no printer attached). */
int ptouch_print_bitmap(const uint8_t *px, int w, int h,
                        const ptouch_print_opts_t *opts);

#ifdef __cplusplus
}
#endif

#endif /* PTOUCH_PRINT_H */
