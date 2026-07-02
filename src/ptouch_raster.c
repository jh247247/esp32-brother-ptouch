/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Jack Hosemans
 *
 * ptouch_raster.c — Portable Brother P-touch raster protocol core.
 * See ptouch_raster.h for the API contract and protocol references.
 *
 * No ESP-IDF / hardware dependencies. Uses only the C standard library.
 */
#include "ptouch_raster.h"

#include <stdlib.h>
#include <string.h>

/* ===========================================================================
 * Growable byte buffer
 * ========================================================================= */

void ptouch_buf_init(ptouch_buf_t *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->oom = false;
}

void ptouch_buf_free(ptouch_buf_t *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->oom = false;
}

bool ptouch_buf_reserve(ptouch_buf_t *b, size_t extra)
{
    if (b->oom) {
        return false;
    }
    if (b->len + extra <= b->cap) {
        return true;
    }
    size_t need = b->len + extra;
    size_t newcap = b->cap ? b->cap : 128;
    while (newcap < need) {
        newcap *= 2;
    }
    uint8_t *p = (uint8_t *)realloc(b->data, newcap);
    if (!p) {
        b->oom = true;
        return false;
    }
    b->data = p;
    b->cap = newcap;
    return true;
}

bool ptouch_buf_append(ptouch_buf_t *b, const uint8_t *src, size_t n)
{
    if (!ptouch_buf_reserve(b, n)) {
        return false;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return true;
}

bool ptouch_buf_append_byte(ptouch_buf_t *b, uint8_t v)
{
    if (!ptouch_buf_reserve(b, 1)) {
        return false;
    }
    b->data[b->len++] = v;
    return true;
}

/* ===========================================================================
 * PackBits (TIFF) compression
 *
 * ptouch_packbits_encode is a byte-for-byte port of the state machine in the
 * `packbits` PyPI package (0.6) used by brother_pt, so a generated raster
 * stream matches the reference oracle exactly. Encoding is lossless regardless.
 * ========================================================================= */

size_t ptouch_packbits_max_encoded_size(size_t src_len)
{
    if (src_len == 0) {
        return 0;
    }
    /* Strict upper bound. Every input byte contributes at most 2 output bytes:
     * a 1-byte literal run emits (header + byte) = 2 bytes/byte (the worst
     * ratio; isolated singles must be separated by >=2-byte RLE runs, so the
     * sustained worst case is ~4/3, but 2n is a safe simple ceiling), and any
     * RLE run of length R>=2 emits only 2 bytes for R inputs (<=1/byte). The
     * len==1 special case emits exactly 2 bytes. */
    return 2 * src_len + 2;
}

int ptouch_packbits_encode(const uint8_t *data, size_t len,
                           uint8_t *dst, size_t dst_cap)
{
    size_t out = 0;

/* Append one byte to dst with bounds checking. */
#define PB_PUT(v)                                   \
    do {                                            \
        if (out >= dst_cap) return -1;              \
        dst[out++] = (uint8_t)(v);                  \
    } while (0)

    if (len == 0) {
        return 0;
    }
    if (len == 1) {
        PB_PUT(0x00);
        PB_PUT(data[0]);
        return (int)out;
    }

    uint8_t buf[128];   /* pending literal run */
    size_t  buflen = 0;
    size_t  pos = 0;
    int     repeat_count = 0;
    const int MAX_LENGTH = 127;
    enum { RAW, RLE } state = RAW;

    while (pos < len - 1) {
        uint8_t current_byte = data[pos];
        if (data[pos] == data[pos + 1]) {
            if (state == RAW) {
                /* finish_raw */
                if (buflen > 0) {
                    PB_PUT(buflen - 1);
                    for (size_t k = 0; k < buflen; k++) {
                        PB_PUT(buf[k]);
                    }
                    buflen = 0;
                }
                state = RLE;
                repeat_count = 1;
            } else { /* RLE */
                if (repeat_count == MAX_LENGTH) {
                    /* finish_rle */
                    PB_PUT(256 - (repeat_count - 1));
                    PB_PUT(data[pos]);
                    repeat_count = 0;
                }
                repeat_count += 1;
            }
        } else {
            if (state == RLE) {
                repeat_count += 1;
                /* finish_rle */
                PB_PUT(256 - (repeat_count - 1));
                PB_PUT(data[pos]);
                state = RAW;
                repeat_count = 0;
            } else { /* RAW */
                if (buflen == (size_t)MAX_LENGTH) {
                    /* finish_raw */
                    PB_PUT(buflen - 1);
                    for (size_t k = 0; k < buflen; k++) {
                        PB_PUT(buf[k]);
                    }
                    buflen = 0;
                }
                buf[buflen++] = current_byte;
            }
        }
        pos += 1;
    }

    if (state == RAW) {
        buf[buflen++] = data[pos];
        /* finish_raw */
        if (buflen > 0) {
            PB_PUT(buflen - 1);
            for (size_t k = 0; k < buflen; k++) {
                PB_PUT(buf[k]);
            }
            buflen = 0;
        }
    } else {
        repeat_count += 1;
        /* finish_rle */
        PB_PUT(256 - (repeat_count - 1));
        PB_PUT(data[pos]);
    }

    return (int)out;
#undef PB_PUT
}

int ptouch_packbits_decode(const uint8_t *src, size_t len,
                           uint8_t *dst, size_t dst_cap)
{
    size_t pos = 0, out = 0;
    while (pos < len) {
        int h = src[pos++];
        if (h > 127) {
            h -= 256;
        }
        if (h >= 0) {
            int count = h + 1;
            for (int i = 0; i < count; i++) {
                if (pos >= len || out >= dst_cap) return -1;
                dst[out++] = src[pos++];
            }
        } else if (h == -128) {
            /* no-op per PackBits spec */
        } else {
            int count = 1 - h;
            if (pos >= len) return -1;
            uint8_t v = src[pos++];
            for (int i = 0; i < count; i++) {
                if (out >= dst_cap) return -1;
                dst[out++] = v;
            }
        }
    }
    return (int)out;
}

/* ===========================================================================
 * Command builders
 * ========================================================================= */

bool ptouch_build_invalidate(ptouch_buf_t *b)
{
    if (!ptouch_buf_reserve(b, 100)) {
        return false;
    }
    memset(b->data + b->len, 0x00, 100);
    b->len += 100;
    return true;
}

bool ptouch_build_init(ptouch_buf_t *b)
{
    static const uint8_t cmd[] = { 0x1B, 0x40 };
    return ptouch_buf_append(b, cmd, sizeof cmd);
}

bool ptouch_build_status_request(ptouch_buf_t *b)
{
    static const uint8_t cmd[] = { 0x1B, 0x69, 0x53 };
    return ptouch_buf_append(b, cmd, sizeof cmd);
}

bool ptouch_build_raster_mode(ptouch_buf_t *b)
{
    static const uint8_t cmd[] = { 0x1B, 0x69, 0x61, 0x01 };
    return ptouch_buf_append(b, cmd, sizeof cmd);
}

bool ptouch_build_enable_status_notification(ptouch_buf_t *b)
{
    static const uint8_t cmd[] = { 0x1B, 0x69, 0x21, 0x00 };
    return ptouch_buf_append(b, cmd, sizeof cmd);
}

bool ptouch_build_print_info(ptouch_buf_t *b, uint8_t media_width_mm,
                             uint32_t num_lines)
{
    uint8_t cmd[13] = {
        0x1B, 0x69, 0x7A,   /* ESC i z */
        0x84,               /* n1: valid flags (media width + recover) */
        0x00,               /* n2: media type */
        media_width_mm,     /* n3: media width (mm) */
        0x00,               /* n4: media length */
        (uint8_t)(num_lines & 0xFF),          /* n5..n8: raster line count LE32 */
        (uint8_t)((num_lines >> 8) & 0xFF),
        (uint8_t)((num_lines >> 16) & 0xFF),
        (uint8_t)((num_lines >> 24) & 0xFF),
        0x00,               /* n9: starting page */
        0x00,               /* n10 */
    };
    return ptouch_buf_append(b, cmd, sizeof cmd);
}

bool ptouch_build_mode(ptouch_buf_t *b, uint8_t mode)
{
    uint8_t cmd[4] = { 0x1B, 0x69, 0x4D, mode };
    return ptouch_buf_append(b, cmd, sizeof cmd);
}

bool ptouch_build_advanced_mode(ptouch_buf_t *b, uint8_t adv)
{
    uint8_t cmd[4] = { 0x1B, 0x69, 0x4B, adv };
    return ptouch_buf_append(b, cmd, sizeof cmd);
}

bool ptouch_build_margin(ptouch_buf_t *b, uint16_t dots)
{
    uint8_t cmd[5] = {
        0x1B, 0x69, 0x64,
        (uint8_t)(dots & 0xFF),
        (uint8_t)((dots >> 8) & 0xFF),
    };
    return ptouch_buf_append(b, cmd, sizeof cmd);
}

bool ptouch_build_compression_mode(ptouch_buf_t *b)
{
    static const uint8_t cmd[] = { 0x4D, 0x02 };
    return ptouch_buf_append(b, cmd, sizeof cmd);
}

bool ptouch_build_raster_line(ptouch_buf_t *b,
                              const uint8_t line[PTOUCH_LINE_LENGTH_BYTES])
{
    bool all_zero = true;
    for (int i = 0; i < PTOUCH_LINE_LENGTH_BYTES; i++) {
        if (line[i]) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) {
        return ptouch_buf_append_byte(b, 0x5A); /* 'Z' empty line */
    }

    uint8_t packed[PTOUCH_LINE_LENGTH_BYTES * 2 + 4];
    int n = ptouch_packbits_encode(line, PTOUCH_LINE_LENGTH_BYTES,
                                   packed, sizeof packed);
    if (n < 0) {
        return false;
    }
    if (!ptouch_buf_append_byte(b, 0x47)) {          /* 'G' */
        return false;
    }
    if (!ptouch_buf_append_byte(b, (uint8_t)(n & 0xFF))) {
        return false;
    }
    if (!ptouch_buf_append_byte(b, (uint8_t)((n >> 8) & 0xFF))) {
        return false;
    }
    return ptouch_buf_append(b, packed, (size_t)n);
}

bool ptouch_build_print(ptouch_buf_t *b, uint8_t feed_or_chain)
{
    return ptouch_buf_append_byte(b, feed_or_chain);
}

/* ===========================================================================
 * Status parsing
 * ========================================================================= */

bool ptouch_parse_status(const uint8_t buf[PTOUCH_STATUS_LENGTH],
                         ptouch_status_t *out)
{
    if (!buf || !out) {
        return false;
    }
    out->valid               = (buf[0] == 0x80 && buf[1] == 0x20);
    out->model               = buf[4];
    out->error_information_1  = buf[8];
    out->error_information_2  = buf[9];
    out->media_width_mm      = buf[10];
    out->media_type          = buf[11];
    out->mode                = buf[15];
    out->media_length        = buf[17];
    out->status_type         = buf[18];
    out->phase_type          = buf[19];
    out->phase_number        = (uint16_t)((buf[20] << 8) | buf[21]);
    out->notification_number = buf[22];
    out->tape_color          = buf[24];
    out->text_color          = buf[25];
    out->hardware_settings   = buf[26];
    return true;
}

int ptouch_status_error_string(const ptouch_status_t *st, char *dst, size_t dst_cap)
{
    if (dst_cap == 0) {
        return 0;
    }
    dst[0] = '\0';
    int count = 0;
    size_t used = 0;

    struct { uint8_t reg; uint8_t bit; const char *msg; } tbl[] = {
        { 1, PTOUCH_ERR1_NO_MEDIA,          "no media" },
        { 1, PTOUCH_ERR1_CUTTER_JAM,        "cutter jam" },
        { 1, PTOUCH_ERR1_WEAK_BATTERIES,    "low batteries" },
        { 1, PTOUCH_ERR1_HIGH_VOLT_ADAPTER, "high-voltage adapter" },
        { 2, PTOUCH_ERR2_WRONG_MEDIA,       "wrong media (check size)" },
        { 2, PTOUCH_ERR2_COVER_OPEN,        "cover open" },
        { 2, PTOUCH_ERR2_OVERHEATING,       "overheating" },
    };

    for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        uint8_t reg = (tbl[i].reg == 1) ? st->error_information_1
                                        : st->error_information_2;
        if (reg & tbl[i].bit) {
            const char *m = tbl[i].msg;
            if (count > 0 && used + 1 < dst_cap) {
                dst[used++] = '|';
            }
            size_t mlen = strlen(m);
            for (size_t k = 0; k < mlen && used + 1 < dst_cap; k++) {
                dst[used++] = m[k];
            }
            count++;
        }
    }
    dst[used < dst_cap ? used : dst_cap - 1] = '\0';
    return count;
}
