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
    bool chain;              /* end with 0x0C (print, NO feed/cut) instead of
                                0x1A — the next job prints back-to-back on the
                                same strip, saving the inter-label leader.
                                Use on every label of a batch except the last.
                                Skips the trailing pad (there is no cut to pad
                                against). */
    int  trailing_pad_dots;  /* blank columns before the cut (default 12) */
    int  min_length_dots;    /* mechanical minimum cut length: the cutter sits
                                ~24.5 mm past the print head, so no cut piece
                                can be shorter (the printer appends blank tape
                                to reach the cutter). When > 0 and the content
                                is shorter, blank columns are split evenly
                                before/after the content so short labels come
                                out CENTERED instead of content-then-long-tail.
                                Suggested value: PTOUCH_MIN_CUT_DOTS. 0 = off
                                (the printer still pads, just lopsidedly).
                                Ignored for chain pages (no cut). */
    int  timeout_ms;         /* per-chunk USB timeout (default 15000) */
} ptouch_print_opts_t;

/* ~24.5 mm at 180 dpi (7.087 dots/mm) — PT-P710BT head-to-cutter distance. */
#define PTOUCH_MIN_CUT_DOTS 174

#define PTOUCH_PRINT_OPTS_DEFAULT() (ptouch_print_opts_t){  \
    .tape_mm = 12, .mirror = false, .autocut = true,         \
    .chain = false, .trailing_pad_dots = 12,                 \
    .min_length_dots = 0, .timeout_ms = 15000 }

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
