/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Jack Hosemans
 *
 * ptouch_raster.h — Portable Brother P-touch raster protocol core.
 *
 * Device-independent byte-level protocol: PackBits (TIFF) compression, the
 * Brother raster command stream builders, and 32-byte status-reply parsing.
 *
 * This file has NO ESP-IDF / hardware dependencies. It only builds byte
 * sequences into a caller-provided growable buffer. The USB *transport*
 * (esp-idf usb_host bulk in/out) is a separate thin layer — see ptouch_driver.h.
 *
 * Protocol reference: Brother "Raster Command Reference PT-E550W/P750W/P710BT",
 * cross-checked against the treideme/brother_pt Python reference implementation
 * (Apache-2.0) which is verified against real PT-P710BT hardware.
 */
#ifndef PTOUCH_RASTER_H
#define PTOUCH_RASTER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Backward-compatible PT-P710BT constants. Model-aware code gets row length
 * from ptouch_media_geometry(). */
#define PTOUCH_PRINT_HEAD_PINS   128
#define PTOUCH_LINE_LENGTH_BYTES 16
#define PTOUCH_MAX_RASTER_BYTES  70 /* 560-dot PT-P900Wc head */
#define PTOUCH_STATUS_LENGTH     32

/* ---------------------------------------------------------------------------
 * Growable byte buffer (caller-owned). Init, append, then free.
 * Growth uses realloc(); all appends are no-ops once `oom` latches true, so a
 * caller may append a whole command sequence and check `oom` once at the end.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
    bool     oom;   /* latched true if any allocation failed */
} ptouch_buf_t;

void ptouch_buf_init(ptouch_buf_t *b);
void ptouch_buf_free(ptouch_buf_t *b);
/* Ensure room for `extra` more bytes. Returns false (and latches oom) on fail. */
bool ptouch_buf_reserve(ptouch_buf_t *b, size_t extra);
bool ptouch_buf_append(ptouch_buf_t *b, const uint8_t *src, size_t n);
bool ptouch_buf_append_byte(ptouch_buf_t *b, uint8_t v);

/* ---------------------------------------------------------------------------
 * PackBits (TIFF) compression — the format used by the Brother 'G' raster
 * transfer command. encode() is byte-exact with the `packbits` Python package
 * used by brother_pt (including its 1-byte and run-boundary quirks) so that a
 * generated command stream matches the reference oracle.
 * ------------------------------------------------------------------------- */

/* Upper bound on encoded size for `src_len` input bytes. */
size_t ptouch_packbits_max_encoded_size(size_t src_len);

/* Encode src[0..src_len) into dst. Returns bytes written, or -1 if dst_cap is
 * too small. dst_cap should be >= ptouch_packbits_max_encoded_size(src_len). */
int ptouch_packbits_encode(const uint8_t *src, size_t src_len,
                           uint8_t *dst, size_t dst_cap);

/* Decode PackBits src[0..src_len) into dst. Returns bytes written, or -1 on
 * truncated input / insufficient dst_cap. */
int ptouch_packbits_decode(const uint8_t *src, size_t src_len,
                           uint8_t *dst, size_t dst_cap);

/* ---------------------------------------------------------------------------
 * Command builders. Each appends its exact byte sequence to `b` and returns
 * false (latching b->oom) on allocation failure.
 * ------------------------------------------------------------------------- */

/* Invalidate: 100 x 0x00. Flushes any partial command in the printer. */
bool ptouch_build_invalidate(ptouch_buf_t *b);
/* Initialise: ESC @  = 1B 40 */
bool ptouch_build_init(ptouch_buf_t *b);
/* Status information request: ESC i S = 1B 69 53 */
bool ptouch_build_status_request(ptouch_buf_t *b);
/* Switch to raster mode (dynamic command mode): ESC i a 01 = 1B 69 61 01 */
bool ptouch_build_raster_mode(ptouch_buf_t *b);
/* Legacy graphics transfer selection: ESC i R 01 = 1B 69 52 01. */
bool ptouch_build_legacy_raster_mode(ptouch_buf_t *b);
/* Automatic status notification "notify": ESC i ! 00 = 1B 69 21 00 */
bool ptouch_build_enable_status_notification(ptouch_buf_t *b);

/* Print information: ESC i z = 1B 69 7A, then
 *   n1=0x84 (valid: media width + recover), n2=0x00 (media type),
 *   n3=media_width_mm, n4=0x00 (media length),
 *   n5..n8 = num_lines (raster line count, 4 bytes little-endian),
 *   n9=0x00 (starting page), n10=0x00.
 * => 1B 69 7A 84 00 <width> 00 <num_lines LE32> 00 00  (13 bytes) */
bool ptouch_build_print_info(ptouch_buf_t *b, uint8_t media_width_mm,
                             uint32_t num_lines);
bool ptouch_build_print_info_ex(ptouch_buf_t *b, uint8_t valid_flags,
                                uint8_t media_width_mm, uint32_t num_lines,
                                uint8_t starting_page);

/* Various mode flags (ESC i M = 1B 69 4D <mode>). */
#define PTOUCH_MODE_AUTO_CUT 0x40
#define PTOUCH_MODE_MIRROR   0x80
bool ptouch_build_mode(ptouch_buf_t *b, uint8_t mode);

/* Advanced mode (ESC i K = 1B 69 4B <adv>). 0x08 = no-chain (print-chaining
 * off). Bit definitions per spec; brother_pt sends 0x08. */
#define PTOUCH_ADV_NO_CHAIN 0x08
bool ptouch_build_advanced_mode(ptouch_buf_t *b, uint8_t adv);

/* Margin (feed) amount in dots: ESC i d = 1B 69 64 <dots LE16>. */
bool ptouch_build_margin(ptouch_buf_t *b, uint16_t dots);
bool ptouch_build_extended_margin(ptouch_buf_t *b, uint16_t dots,
                                  uint8_t marker, uint8_t reserved);

/* Select compression mode = TIFF/PackBits: 4D 02 */
bool ptouch_build_compression_mode(ptouch_buf_t *b);

/* One raster line (exactly 16 bytes). Emits 'Z' (0x5A, empty-line shortcut) if
 * all 16 bytes are zero, otherwise 'G' (0x47) + 2-byte LE length + PackBits
 * data. */
bool ptouch_build_raster_line(ptouch_buf_t *b,
                              const uint8_t line[PTOUCH_LINE_LENGTH_BYTES]);
/* Model-aware line builder. `blank_shortcut` permits the one-byte Z record;
 * older framed printers instead require a normal G record even for zero rows. */
bool ptouch_build_raster_line_ex(ptouch_buf_t *b, const uint8_t *line,
                                 size_t line_len, bool packbits,
                                 bool blank_shortcut);

/* Print command. */
#define PTOUCH_PRINT_FEED  0x1A  /* print + feed (eject / auto-cut) */
#define PTOUCH_PRINT_CHAIN 0x0C  /* print, no feed (chain to next label) */
bool ptouch_build_print(ptouch_buf_t *b, uint8_t feed_or_chain);

/* ---------------------------------------------------------------------------
 * Status parsing — decode the 32-byte reply to ESC i S / notifications.
 * ------------------------------------------------------------------------- */

/* Media type (status byte 11). */
typedef enum {
    PTOUCH_MEDIA_NONE          = 0x00,
    PTOUCH_MEDIA_LAMINATED     = 0x01,
    PTOUCH_MEDIA_NON_LAMINATED = 0x03,
    PTOUCH_MEDIA_HEAT_SHRINK   = 0x11,
    PTOUCH_MEDIA_INCOMPATIBLE  = 0xFF,
} ptouch_media_type_t;

/* Status type (status byte 18). */
typedef enum {
    PTOUCH_ST_REPLY_TO_REQUEST  = 0x00,
    PTOUCH_ST_PRINTING_COMPLETED = 0x01,
    PTOUCH_ST_ERROR_OCCURRED    = 0x02,
    PTOUCH_ST_TURNED_OFF        = 0x04,
    PTOUCH_ST_NOTIFICATION      = 0x05,
    PTOUCH_ST_PHASE_CHANGE      = 0x06,
} ptouch_status_type_t;

/* Error information 1 (status byte 8) flags. */
#define PTOUCH_ERR1_NO_MEDIA          0x01
#define PTOUCH_ERR1_CUTTER_JAM        0x04
#define PTOUCH_ERR1_WEAK_BATTERIES    0x08
#define PTOUCH_ERR1_HIGH_VOLT_ADAPTER 0x40
/* Error information 2 (status byte 9) flags. */
#define PTOUCH_ERR2_WRONG_MEDIA 0x01
#define PTOUCH_ERR2_COVER_OPEN  0x10
#define PTOUCH_ERR2_OVERHEATING 0x20

typedef struct {
    bool     valid;                 /* header (0x80 0x20) looked like a status reply */
    uint8_t  model;                 /* byte 4 — model code */
    uint8_t  error_information_1;   /* byte 8 */
    uint8_t  error_information_2;   /* byte 9 */
    uint8_t  media_width_mm;        /* byte 10 */
    uint8_t  media_type;            /* byte 11 */
    uint8_t  mode;                  /* byte 15 */
    uint8_t  media_length;          /* byte 17 */
    uint8_t  status_type;           /* byte 18 */
    uint8_t  phase_type;            /* byte 19 */
    uint16_t phase_number;          /* bytes 20-21, big-endian */
    uint8_t  notification_number;   /* byte 22 */
    uint8_t  tape_color;            /* byte 24 */
    uint8_t  text_color;            /* byte 25 */
    uint8_t  hardware_settings;     /* byte 26 */
} ptouch_status_t;

/* Parse the 32-byte status reply. Always fills every field from the raw bytes;
 * returns false only if buf is NULL. `valid` reflects the header sanity check
 * (byte0==0x80, byte1==0x20) which the PT-P710BT emits on genuine replies. */
bool ptouch_parse_status(const uint8_t buf[PTOUCH_STATUS_LENGTH],
                         ptouch_status_t *out);

/* Human-readable terminal-error string built from error bits and status type
 * into `dst` (NUL-terminated, truncated to dst_cap). Returns the number of
 * distinct conditions reported (0 => no error, dst is ""). */
int ptouch_status_error_string(const ptouch_status_t *st, char *dst, size_t dst_cap);

#ifdef __cplusplus
}
#endif

#endif /* PTOUCH_RASTER_H */
