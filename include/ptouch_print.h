/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Jack Hosemans
 *
 * Profile-aware Brother P-touch raster job assembly.
 */
#ifndef PTOUCH_PRINT_H
#define PTOUCH_PRINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ptouch_model.h"
#include "ptouch_raster.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ptouch_print_opts {
    int  tape_mm;            /* installed tape width from a fresh status reply */
    bool mirror;             /* reverse feed-direction pixels */
    bool autocut;            /* request a cut after the job (default true) */
    bool chain;              /* suppress final feed only when profile-validated */
    int  trailing_pad_dots;  /* 180-dpi logical blank columns before final feed */
    int  min_length_dots;    /* 180-dpi logical minimum; 0 disables centering */
    int  timeout_ms;         /* per-transfer timeout */
} ptouch_print_opts_t;

#define PTOUCH_MIN_CUT_DOTS 174

/* v0.1 compatibility defaults for PT-P710BT. New model-aware code should use
 * ptouch_print_opts_init(), which applies the selected profile's cut geometry. */
#define PTOUCH_PRINT_OPTS_DEFAULT() (ptouch_print_opts_t){  \
    .tape_mm = 12, .mirror = false, .autocut = true,        \
    .chain = false,                                          \
    .trailing_pad_dots = 12, .min_length_dots = 0,          \
    .timeout_ms = 15000 }

/* Initialize safe, profile-aware defaults for one installed tape width. */
bool ptouch_print_opts_init(const ptouch_model_profile_t *profile,
                            int tape_mm, ptouch_print_opts_t *out);

/* A flattened command stream plus exact command/raster frame boundaries.
 * Legacy profiles use these boundaries as individual USB bulk transfers;
 * P710 retains its verified coalesced transfer policy. */
typedef struct ptouch_print_job {
    ptouch_buf_t stream;
    size_t *frame_ends;       /* monotonically increasing offsets into stream */
    size_t frame_count;
    size_t frame_capacity;
    const ptouch_model_profile_t *profile;
    int tape_mm;
    uint32_t attachment_generation;
} ptouch_print_job_t;

void ptouch_print_job_init(ptouch_print_job_t *job);
void ptouch_print_job_free(ptouch_print_job_t *job);
bool ptouch_print_job_frames_valid(const ptouch_print_job_t *job);

/* The bitmap is at the profile's native head/feed DPI, row-major, one
 * byte/pixel. Height must not exceed geometry.printable_dots. Padding and
 * minimum-length options remain 180-dpi logical values for API compatibility. */
bool ptouch_print_build_job_for_model(
    const ptouch_model_profile_t *profile,
    const uint8_t *px, int w, int h,
    const ptouch_print_opts_t *opts,
    ptouch_print_job_t *out);

/* Host-testable flattened form. Frame metadata is discarded. */
bool ptouch_print_build_stream_for_model(
    const ptouch_model_profile_t *profile,
    const uint8_t *px, int w, int h,
    const ptouch_print_opts_t *opts,
    ptouch_buf_t *out);

/* Backward-compatible wrapper: deliberately remains PT-P710BT-only and must
 * continue producing the v0.1.x stream byte-for-byte. */
bool ptouch_print_build_stream(const uint8_t *px, int w, int h,
                               const ptouch_print_opts_t *opts,
                               ptouch_buf_t *out);

/* Legacy flattened result. Prefer ptouch_usb_print_bitmap(), whose structured
 * result distinguishes zero-output rejection from a possibly printed label. */
#if defined(__GNUC__)
__attribute__((deprecated("use ptouch_usb_print_bitmap for a structured result")))
#endif
int ptouch_print_bitmap(const uint8_t *px, int w, int h,
                        const ptouch_print_opts_t *opts);

#ifdef __cplusplus
}
#endif

#endif /* PTOUCH_PRINT_H */
