/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Jack Hosemans
 */
#include "ptouch_print.h"
#ifndef PTOUCH_HOST_BUILD
#include "ptouch_usb.h"
#endif

#include <limits.h>
#include <stdlib.h>
#include <string.h>

bool ptouch_print_opts_init(const ptouch_model_profile_t *profile,
                            int tape_mm, ptouch_print_opts_t *out)
{
    if (!out || !ptouch_model_has_print_recipe(profile) ||
        !ptouch_model_accepts_width(profile, tape_mm)) {
        return false;
    }
    *out = (ptouch_print_opts_t) {
        .tape_mm = tape_mm,
        .mirror = false,
        .autocut = true,
        .chain = false,
        .trailing_pad_dots = profile->trailing_pad_dots_180,
        .min_length_dots = profile->minimum_cut_dots_180,
        .timeout_ms = 15000,
    };
    return true;
}

void ptouch_print_job_init(ptouch_print_job_t *job)
{
    if (!job) return;
    memset(job, 0, sizeof *job);
    ptouch_buf_init(&job->stream);
}

void ptouch_print_job_free(ptouch_print_job_t *job)
{
    if (!job) return;
    ptouch_buf_free(&job->stream);
    free(job->frame_ends);
    memset(job, 0, sizeof *job);
}

bool ptouch_print_job_frames_valid(const ptouch_print_job_t *job)
{
    if (!job || !job->profile || !job->stream.data || job->stream.len == 0 ||
        !job->frame_ends || job->frame_count == 0 ||
        job->frame_capacity < job->frame_count ||
        job->frame_ends[job->frame_count - 1] != job->stream.len) {
        return false;
    }
    size_t previous = 0;
    for (size_t i = 0; i < job->frame_count; ++i) {
        size_t end = job->frame_ends[i];
        if (end <= previous || end > job->stream.len) return false;
        previous = end;
    }
    return true;
}

static bool mark_frame(ptouch_print_job_t *job)
{
    if (!job || job->stream.oom) return false;
    if (job->frame_count == job->frame_capacity) {
        size_t next = job->frame_capacity ? job->frame_capacity * 2U : 64U;
        if (next < job->frame_capacity || next > SIZE_MAX / sizeof(size_t)) {
            job->stream.oom = true;
            return false;
        }
        size_t *p = (size_t *)realloc(job->frame_ends, next * sizeof *p);
        if (!p) {
            job->stream.oom = true;
            return false;
        }
        job->frame_ends = p;
        job->frame_capacity = next;
    }
    job->frame_ends[job->frame_count++] = job->stream.len;
    return true;
}

#define EMIT(job_, expression_)                 \
    do {                                        \
        if (!(expression_) || !mark_frame(job_)) \
            goto fail;                          \
    } while (0)

static bool emit_blank_row(ptouch_print_job_t *job, size_t row_bytes,
                           bool packbits, bool blank_shortcut)
{
    uint8_t row[PTOUCH_MAX_RASTER_BYTES] = {0};
    if (!ptouch_build_raster_line_ex(&job->stream, row, row_bytes, packbits,
                                     blank_shortcut)) {
        return false;
    }
    return mark_frame(job);
}

static bool emit_profile_mode_setup(ptouch_print_job_t *job,
                                    const ptouch_model_profile_t *profile,
                                    const ptouch_print_opts_t *opts)
{
    if (profile->autocut_strategy == PTOUCH_AUTOCUT_ESC_I_M) {
        uint8_t mode = profile->mode_feed_code;
        if (opts->autocut) mode = (uint8_t)(mode | PTOUCH_MODE_AUTO_CUT);
        if (!ptouch_build_mode(&job->stream, mode) || !mark_frame(job)) {
            return false;
        }
    }
    if (profile->emit_no_chain_mode) {
        if (!ptouch_build_advanced_mode(
                &job->stream, opts->chain ? 0 : PTOUCH_ADV_NO_CHAIN) ||
            !mark_frame(job)) {
            return false;
        }
    } else if (opts->chain &&
               profile->finalize_variant == PTOUCH_FINALIZE_D460) {
        if (!ptouch_build_advanced_mode(&job->stream, 0x00) ||
            !mark_frame(job)) {
            return false;
        }
    }
    return true;
}

bool ptouch_print_build_job_for_model(
    const ptouch_model_profile_t *profile,
    const uint8_t *px, int w, int h,
    const ptouch_print_opts_t *opts,
    ptouch_print_job_t *out)
{
    if (!out || out->stream.data || out->frame_ends || out->frame_count ||
        !ptouch_model_has_print_recipe(profile) || !px || w <= 0 || h <= 0 ||
        (size_t)w > SIZE_MAX / (size_t)h) {
        return false;
    }

    ptouch_print_opts_t o;
    if (opts) {
        o = *opts;
    } else {
        if (!ptouch_print_opts_init(profile, 12, &o)) return false;
    }
    if (o.timeout_ms <= 0 || o.tape_mm <= 0 || o.trailing_pad_dots < 0 ||
        o.min_length_dots < 0 || (o.chain && !profile->chain_validated) ||
        (!o.autocut &&
         profile->autocut_strategy != PTOUCH_AUTOCUT_ESC_I_M)) {
        return false;
    }

    ptouch_media_geometry_t geometry;
    if (!ptouch_model_accepts_width(profile, o.tape_mm) ||
        !ptouch_media_geometry(profile, o.tape_mm, &geometry) ||
        h > geometry.printable_dots) {
        return false;
    }

    const int pad_scale = profile->feed_dpi / 180;
    if ((pad_scale != 1 && pad_scale != 2)) {
        return false;
    }
    const int physical_w = w;
    const int physical_h = h;
    const int wire_top = ((int)profile->head_dots - physical_h) / 2 +
                         profile->wire_offset_dots;
    if (wire_top < 0 || wire_top + physical_h > profile->head_dots) {
        return false;
    }

    int logical_lead = 0;
    int logical_tail = o.chain ? 0 : o.trailing_pad_dots;
    if (!o.chain && o.min_length_dots > 0) {
        if (w > INT_MAX - logical_tail) return false;
        int shortfall = o.min_length_dots - (w + logical_tail);
        if (shortfall > 0) {
            logical_lead = shortfall / 2;
            logical_tail += shortfall - logical_lead;
        }
    }
    if (logical_lead > INT_MAX / pad_scale ||
        logical_tail > INT_MAX / pad_scale) {
        return false;
    }
    const int lead_rows = logical_lead * pad_scale;
    const int tail_rows = logical_tail * pad_scale;
    if (physical_w > INT_MAX - lead_rows ||
        physical_w + lead_rows > INT_MAX - tail_rows) {
        return false;
    }
    const uint32_t raster_rows =
        (uint32_t)(lead_rows + physical_w + tail_rows);
    const bool packbits = profile->encoding == PTOUCH_ENCODING_PACKBITS;
    const bool blank_shortcut = profile->blank_line_shortcut;

    out->profile = profile;
    out->tape_mm = o.tape_mm;

    if (!ptouch_build_invalidate(&out->stream) ||
        !ptouch_build_init(&out->stream) || !mark_frame(out)) {
        goto fail;
    }

    if (profile->info_variant == PTOUCH_INFO_P710_RECOVER) {
        /* This exact ordering is the hardware-verified P710BT v0.1.x plan. */
        EMIT(out, ptouch_build_raster_mode(&out->stream));
        EMIT(out, ptouch_build_enable_status_notification(&out->stream));
        EMIT(out, ptouch_build_print_info(&out->stream, (uint8_t)o.tape_mm,
                                          raster_rows));
        EMIT(out, ptouch_build_mode(&out->stream,
                                    o.autocut ? PTOUCH_MODE_AUTO_CUT : 0));
        EMIT(out, ptouch_build_advanced_mode(&out->stream,
                                             PTOUCH_ADV_NO_CHAIN));
        EMIT(out, ptouch_build_margin(&out->stream, 0));
        EMIT(out, ptouch_build_compression_mode(&out->stream));
    } else {
        if (profile->mode_before_compression &&
            !emit_profile_mode_setup(out, profile, &o)) {
            goto fail;
        }
        if (packbits) {
            EMIT(out, ptouch_build_compression_mode(&out->stream));
        }
        if (profile->raster_select == PTOUCH_RASTER_DYNAMIC_A) {
            EMIT(out, ptouch_build_raster_mode(&out->stream));
        } else if (profile->raster_select == PTOUCH_RASTER_LEGACY_R) {
            EMIT(out, ptouch_build_legacy_raster_mode(&out->stream));
        }
        if (profile->info_variant == PTOUCH_INFO_BASIC ||
            profile->info_variant == PTOUCH_INFO_D460) {
            uint8_t starting_page =
                profile->info_variant == PTOUCH_INFO_D460 ? 0x02 : 0x00;
            EMIT(out, ptouch_build_print_info_ex(
                &out->stream, 0x00, (uint8_t)o.tape_mm, raster_rows,
                starting_page));
        }
        if (profile->finalize_variant == PTOUCH_FINALIZE_D460) {
            EMIT(out, ptouch_build_extended_margin(&out->stream, 1,
                                                    0x4D, 0x00));
        }
        if (!profile->mode_before_compression &&
            !emit_profile_mode_setup(out, profile, &o)) {
            goto fail;
        }
    }

    for (int x = 0; x < lead_rows; ++x) {
        if (!emit_blank_row(out, geometry.row_bytes, packbits,
                            blank_shortcut)) goto fail;
    }

    for (int x = 0; x < physical_w; ++x) {
        int source_x = o.mirror ? (w - 1 - x) : x;
        uint8_t row[PTOUCH_MAX_RASTER_BYTES] = {0};
        for (int y = 0; y < physical_h; ++y) {
            if (px[(size_t)y * (size_t)w + (size_t)source_x]) {
                int dot = wire_top + y;
                row[dot >> 3] |= (uint8_t)(0x80U >> (dot & 7));
            }
        }
        if (!ptouch_build_raster_line_ex(&out->stream, row,
                                         geometry.row_bytes, packbits,
                                         blank_shortcut) ||
            !mark_frame(out)) {
            goto fail;
        }
    }

    for (int x = 0; x < tail_rows; ++x) {
        if (!emit_blank_row(out, geometry.row_bytes, packbits,
                            blank_shortcut)) goto fail;
    }

    uint8_t final_command = PTOUCH_PRINT_FEED;
    if (o.chain && profile->finalize_variant == PTOUCH_FINALIZE_STANDARD) {
        final_command = PTOUCH_PRINT_CHAIN;
    }
    EMIT(out, ptouch_build_print(&out->stream, final_command));
    return !out->stream.oom;

fail:
    ptouch_print_job_free(out);
    ptouch_print_job_init(out);
    return false;
}

bool ptouch_print_build_stream_for_model(
    const ptouch_model_profile_t *profile,
    const uint8_t *px, int w, int h,
    const ptouch_print_opts_t *opts,
    ptouch_buf_t *out)
{
    if (!out) return false;
    ptouch_print_job_t job;
    ptouch_print_job_init(&job);
    if (!ptouch_print_build_job_for_model(profile, px, w, h, opts, &job)) {
        ptouch_print_job_free(&job);
        return false;
    }
    bool ok = ptouch_buf_append(out, job.stream.data, job.stream.len);
    ptouch_print_job_free(&job);
    return ok && !out->oom;
}

bool ptouch_print_build_stream(const uint8_t *px, int w, int h,
                               const ptouch_print_opts_t *opts,
                               ptouch_buf_t *out)
{
    /* The legacy wrapper's NULL contract was the public DEFAULT options, not
     * the model profile's physical minimum. Preserve that byte stream exactly. */
    ptouch_print_opts_t defaults;
    if (!opts) {
        defaults = PTOUCH_PRINT_OPTS_DEFAULT();
        opts = &defaults;
    }
    return ptouch_print_build_stream_for_model(ptouch_model_p710bt(),
                                                px, w, h, opts, out);
}

int ptouch_print_bitmap(const uint8_t *px, int w, int h,
                        const ptouch_print_opts_t *opts)
{
#ifdef PTOUCH_HOST_BUILD
    (void)px; (void)w; (void)h; (void)opts;
    return -2;
#else
    ptouch_print_opts_t defaults;
    if (!opts) {
        defaults = PTOUCH_PRINT_OPTS_DEFAULT();
        opts = &defaults;
    }
    ptouch_usb_print_result_t result =
        ptouch_usb_print_bitmap(px, w, h, opts, 60000);
    if (result.transfer_rc != PTOUCH_USB_TRANSFER_OK) return result.transfer_rc;
    return result.completion_rc == PTOUCH_USB_COMPLETION_OK
               ? 0 : -20 + result.completion_rc;
#endif
}
