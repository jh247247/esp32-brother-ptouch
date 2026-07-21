/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Jack Hosemans
 *
 * Immutable Brother P-touch USB model profiles and media geometry.
 */
#ifndef PTOUCH_MODEL_H
#define PTOUCH_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PTOUCH_BROTHER_VID 0x04F9
#define PTOUCH_MAX_HEAD_DOTS 560
#define PTOUCH_MAX_LINE_BYTES (PTOUCH_MAX_HEAD_DOTS / 8)

#define PTOUCH_TAPE_WIDTH_4   (1U << 0) /* 3.5 mm, protocol value 4 */
#define PTOUCH_TAPE_WIDTH_6   (1U << 1)
#define PTOUCH_TAPE_WIDTH_9   (1U << 2)
#define PTOUCH_TAPE_WIDTH_12  (1U << 3)
#define PTOUCH_TAPE_WIDTH_18  (1U << 4)
#define PTOUCH_TAPE_WIDTH_21  (1U << 5)
#define PTOUCH_TAPE_WIDTH_24  (1U << 6)
#define PTOUCH_TAPE_WIDTH_36  (1U << 7)
#define PTOUCH_TAPE_WIDTHS_12 \
    (PTOUCH_TAPE_WIDTH_4 | PTOUCH_TAPE_WIDTH_6 | \
     PTOUCH_TAPE_WIDTH_9 | PTOUCH_TAPE_WIDTH_12)
#define PTOUCH_TAPE_WIDTHS_18 \
    (PTOUCH_TAPE_WIDTHS_12 | PTOUCH_TAPE_WIDTH_18)
#define PTOUCH_TAPE_WIDTHS_24 \
    (PTOUCH_TAPE_WIDTHS_18 | PTOUCH_TAPE_WIDTH_24)
#define PTOUCH_TAPE_WIDTHS_36 \
    (PTOUCH_TAPE_WIDTHS_24 | PTOUCH_TAPE_WIDTH_36)
#define PTOUCH_TAPE_WIDTHS_KNOWN \
    (PTOUCH_TAPE_WIDTHS_36 | PTOUCH_TAPE_WIDTH_21)

typedef enum {
    /* End-to-end output and mechanical completion verified on hardware. */
    PTOUCH_SUPPORT_HARDWARE_VALIDATED = 0,
    /* Printed output was inspected on hardware; completion remains provisional. */
    PTOUCH_SUPPORT_OUTPUT_INSPECTED,
    /* A protocol recipe exists, but output requires explicit transport opt-in. */
    PTOUCH_SUPPORT_EXPERIMENTAL_RECIPE,
    PTOUCH_SUPPORT_P_LITE,
    PTOUCH_SUPPORT_UNSUPPORTED_RASTER,
    PTOUCH_SUPPORT_KNOWN_UNSUPPORTED,
    PTOUCH_SUPPORT_UNKNOWN,
} ptouch_support_t;

typedef enum {
    PTOUCH_RASTER_LEGACY_R = 0, /* ESC i R 01 */
    PTOUCH_RASTER_DYNAMIC_A,    /* ESC i a 01 */
    PTOUCH_RASTER_IMPLICIT,     /* no raster-select command */
} ptouch_raster_select_t;

typedef enum {
    PTOUCH_ENCODING_RAW = 0,
    PTOUCH_ENCODING_PACKBITS,
} ptouch_encoding_t;

typedef enum {
    PTOUCH_INFO_NONE = 0,
    PTOUCH_INFO_P710_RECOVER,
    PTOUCH_INFO_BASIC,
    PTOUCH_INFO_D460,
} ptouch_info_variant_t;

typedef enum {
    PTOUCH_FINALIZE_STANDARD = 0,
    PTOUCH_FINALIZE_D460,
} ptouch_finalize_variant_t;

typedef enum {
    PTOUCH_TRANSFER_COALESCED = 0,
    PTOUCH_TRANSFER_FRAMED,
} ptouch_transfer_policy_t;

typedef enum {
    PTOUCH_COMPLETION_STRICT_NOTIFICATIONS = 0,
    PTOUCH_COMPLETION_JOB_STATUS,
} ptouch_completion_strategy_t;

typedef enum {
    /* The final feed command requests the model's ordinary cut behavior. */
    PTOUCH_AUTOCUT_FINALIZE = 0,
    /* ESC i M explicitly enables or disables automatic cutting. */
    PTOUCH_AUTOCUT_ESC_I_M,
} ptouch_autocut_strategy_t;

typedef struct ptouch_model_profile {
    uint16_t vid;
    uint16_t pid;
    const char *name;
    ptouch_support_t support;

    uint16_t head_dots;
    uint16_t head_dpi;
    uint16_t feed_dpi;
    /* Offset in this component's byte-0/MSB wire coordinate system. */
    int16_t wire_offset_dots;

    ptouch_raster_select_t raster_select;
    ptouch_encoding_t encoding;
    ptouch_info_variant_t info_variant;
    ptouch_finalize_variant_t finalize_variant;
    ptouch_transfer_policy_t transfer_policy;
    ptouch_completion_strategy_t completion_strategy;

    ptouch_autocut_strategy_t autocut_strategy;
    /* Low five bits of ESC i M on legacy models; zero for modern modes. */
    uint8_t mode_feed_code;
    /* Emit ESC i K with the ordinary no-chain bit for finalized jobs. */
    bool emit_no_chain_mode;
    /* Emit ESC i M/K before compression, as required by older drivers. */
    bool mode_before_compression;
    bool completion_validated;
    bool chain_validated;
    uint16_t minimum_cut_dots_180;
    uint8_t trailing_pad_dots_180;
    uint8_t expected_status_model; /* 0 when no authoritative value is known */
    uint16_t tape_width_mask;       /* admitted standard TZe/TZ widths */
    bool blank_line_shortcut;       /* emit one-byte Z for an all-zero row */
    const char *limitation;
} ptouch_model_profile_t;

typedef struct {
    int tape_width_mm;          /* 4 is the protocol value for 3.5 mm tape */
    uint16_t printable_dots;    /* physical cross-feed dots, head-clamped */
    uint16_t logical_height_180;/* renderer height in the 180-dpi design space */
    uint16_t head_dots;
    uint16_t row_bytes;
    uint16_t head_dpi;
    uint16_t feed_dpi;
    int16_t wire_top_dot;       /* placement of a full-width media bitmap */
} ptouch_media_geometry_t;

const ptouch_model_profile_t *ptouch_model_lookup(uint16_t vid, uint16_t pid);
size_t ptouch_model_count(void);
const ptouch_model_profile_t *ptouch_model_at(size_t index);
const ptouch_model_profile_t *ptouch_model_p710bt(void);

/* True when a complete raster recipe is available, including experimental
 * profiles. Building a byte stream is side-effect free and does not imply that
 * output is enabled. */
bool ptouch_model_has_print_recipe(const ptouch_model_profile_t *profile);

/* Conservative default: hardware-validated and inspected-output profiles only. */
bool ptouch_model_can_print(const ptouch_model_profile_t *profile);

/* Transport policy gate. Recipe-derived profiles are admitted only when the
 * caller explicitly opts in. Unsupported/P-Lite identities always fail. */
bool ptouch_model_output_allowed(const ptouch_model_profile_t *profile,
                                 bool allow_experimental);
const char *ptouch_support_name(ptouch_support_t support);

/* Standard tape widths admitted by the hardware profile. Geometry remains
 * separately available for offline preview, but a print preflight must pass
 * both width and media-type validation. Heat-shrink and incompatible media are
 * deliberately excluded until a profile-specific recipe is validated. */
uint16_t ptouch_model_tape_width_mask(
    const ptouch_model_profile_t *profile);
bool ptouch_model_accepts_width(const ptouch_model_profile_t *profile,
                                int tape_width_mm);
bool ptouch_model_accepts_media(const ptouch_model_profile_t *profile,
                                int tape_width_mm, uint8_t media_type);

/* Unknown tape widths and unsupported profiles fail instead of falling back. */
bool ptouch_media_geometry(const ptouch_model_profile_t *profile,
                           int tape_width_mm,
                           ptouch_media_geometry_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PTOUCH_MODEL_H */
