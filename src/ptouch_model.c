/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Jack Hosemans
 *
 * Profile data is an independently expressed interoperability catalog. The
 * raster recipes use semantic fields so no third-party flag layout or source
 * structure becomes part of this Apache-2.0 component.
 */
#include "ptouch_model.h"

#include <string.h>

#define PROFILE(pid_, name_, dots_, dpi_, raster_, encoding_, info_, final_, \
                transfer_, completion_, autocut_mode_, chain_, min_, pad_, status_, note_) \
    { .vid = PTOUCH_BROTHER_VID, .pid = (pid_), .name = (name_),              \
      .support = PTOUCH_SUPPORT_EXPERIMENTAL_RECIPE,                          \
      .head_dots = (dots_), .head_dpi = (dpi_), .feed_dpi = (dpi_),           \
      .wire_offset_dots = 0, .raster_select = (raster_),                      \
      .encoding = (encoding_), .info_variant = (info_),                       \
      .finalize_variant = (final_), .transfer_policy = (transfer_),           \
      .completion_strategy = (completion_),                                   \
      .autocut_strategy = (autocut_mode_) ? PTOUCH_AUTOCUT_ESC_I_M            \
                                           : PTOUCH_AUTOCUT_FINALIZE,          \
      .completion_validated = false, .chain_validated = (chain_),             \
      .minimum_cut_dots_180 = (min_), .trailing_pad_dots_180 = (pad_),        \
      .expected_status_model = (status_),                                     \
      .tape_width_mask = PTOUCH_TAPE_WIDTHS_24,                               \
      .blank_line_shortcut = false, .limitation = (note_) }

#define PROFILE_WIDTHS(pid_, name_, dots_, dpi_, raster_, encoding_, info_,  \
                       final_, transfer_, completion_, autocut_mode_, chain_, min_, \
                       pad_, status_, widths_, note_)                         \
    { .vid = PTOUCH_BROTHER_VID, .pid = (pid_), .name = (name_),              \
      .support = PTOUCH_SUPPORT_EXPERIMENTAL_RECIPE,                          \
      .head_dots = (dots_), .head_dpi = (dpi_), .feed_dpi = (dpi_),           \
      .wire_offset_dots = 0, .raster_select = (raster_),                      \
      .encoding = (encoding_), .info_variant = (info_),                       \
      .finalize_variant = (final_), .transfer_policy = (transfer_),           \
      .completion_strategy = (completion_),                                   \
      .autocut_strategy = (autocut_mode_) ? PTOUCH_AUTOCUT_ESC_I_M            \
                                           : PTOUCH_AUTOCUT_FINALIZE,          \
      .completion_validated = false, .chain_validated = (chain_),             \
      .minimum_cut_dots_180 = (min_), .trailing_pad_dots_180 = (pad_),        \
      .expected_status_model = (status_), .tape_width_mask = (widths_),       \
      .blank_line_shortcut = false, .limitation = (note_) }

#define LEGACY_PACKBITS(pid_, name_, dots_, dpi_, autocut_mode_, note_)       \
    PROFILE((pid_), (name_), (dots_), (dpi_), PTOUCH_RASTER_LEGACY_R,          \
            PTOUCH_ENCODING_PACKBITS, PTOUCH_INFO_NONE,                       \
            PTOUCH_FINALIZE_STANDARD, PTOUCH_TRANSFER_FRAMED,                 \
            PTOUCH_COMPLETION_JOB_STATUS, (autocut_mode_), false, 0, 0, 0, (note_))

#define LEGACY_RAW(pid_, name_, autocut_mode_, note_)                          \
    PROFILE((pid_), (name_), 128, 180, PTOUCH_RASTER_LEGACY_R,                 \
            PTOUCH_ENCODING_RAW, PTOUCH_INFO_NONE, PTOUCH_FINALIZE_STANDARD,   \
            PTOUCH_TRANSFER_FRAMED, PTOUCH_COMPLETION_JOB_STATUS,              \
            (autocut_mode_), false, 0, 0, 0, (note_))

static const ptouch_model_profile_t s_profiles[] = {
    PROFILE_WIDTHS(0x2001, "PT-9200DX", 384, 360, PTOUCH_RASTER_LEGACY_R,
                   PTOUCH_ENCODING_PACKBITS, PTOUCH_INFO_NONE,
                   PTOUCH_FINALIZE_STANDARD, PTOUCH_TRANSFER_FRAMED,
                   PTOUCH_COMPLETION_JOB_STATUS, true, false, 0, 0, 0,
                   PTOUCH_TAPE_WIDTHS_36 & ~PTOUCH_TAPE_WIDTH_4,
                   "3.5 mm reports as 6 mm; non-36 mm media remain unverified"),
    PROFILE_WIDTHS(0x2002, "PT-9200DX", 384, 360, PTOUCH_RASTER_LEGACY_R,
                   PTOUCH_ENCODING_PACKBITS, PTOUCH_INFO_NONE,
                   PTOUCH_FINALIZE_STANDARD, PTOUCH_TRANSFER_FRAMED,
                   PTOUCH_COMPLETION_JOB_STATUS, true, false, 0, 0, 0,
                   PTOUCH_TAPE_WIDTHS_36 & ~PTOUCH_TAPE_WIDTH_4,
                   "alternate USB identity; 3.5 mm reports as 6 mm"),
    PROFILE_WIDTHS(0x2004, "PT-2300", 112, 180, PTOUCH_RASTER_LEGACY_R,
                   PTOUCH_ENCODING_PACKBITS, PTOUCH_INFO_NONE,
                   PTOUCH_FINALIZE_STANDARD, PTOUCH_TRANSFER_FRAMED,
                   PTOUCH_COMPLETION_JOB_STATUS, true, false, 0, 0, 0,
                   PTOUCH_TAPE_WIDTHS_24 & ~PTOUCH_TAPE_WIDTH_4,
                   "Brother's supported consumables begin at 6 mm"),
    PROFILE_WIDTHS(0x2007, "PT-2420PC", 128, 180, PTOUCH_RASTER_LEGACY_R,
                   PTOUCH_ENCODING_PACKBITS, PTOUCH_INFO_NONE,
                   PTOUCH_FINALIZE_STANDARD, PTOUCH_TRANSFER_FRAMED,
                   PTOUCH_COMPLETION_JOB_STATUS, false, false, 0, 0, 0,
                   PTOUCH_TAPE_WIDTHS_24 & ~PTOUCH_TAPE_WIDTH_4,
                   "3.5 mm media reports as 6 mm"),
    PROFILE_WIDTHS(0x2011, "PT-2450PC / PT-2450DX", 128, 180,
                   PTOUCH_RASTER_LEGACY_R, PTOUCH_ENCODING_PACKBITS,
                   PTOUCH_INFO_NONE, PTOUCH_FINALIZE_STANDARD,
                   PTOUCH_TRANSFER_FRAMED, PTOUCH_COMPLETION_JOB_STATUS,
                   false, false, 0, 0, 0,
                   PTOUCH_TAPE_WIDTHS_24 & ~PTOUCH_TAPE_WIDTH_4,
                   "3.5 mm media reports as 6 mm"),
    { .vid = PTOUCH_BROTHER_VID, .pid = 0x2019, .name = "PT-1950",
      .support = PTOUCH_SUPPORT_OUTPUT_INSPECTED,
      .head_dots = 112, .head_dpi = 180, .feed_dpi = 180,
      .raster_select = PTOUCH_RASTER_IMPLICIT,
      .encoding = PTOUCH_ENCODING_PACKBITS,
      .info_variant = PTOUCH_INFO_NONE,
      .finalize_variant = PTOUCH_FINALIZE_STANDARD,
      .transfer_policy = PTOUCH_TRANSFER_FRAMED,
      .completion_strategy = PTOUCH_COMPLETION_JOB_STATUS,
      .autocut_strategy = PTOUCH_AUTOCUT_ESC_I_M,
      /* Brother's default PT-1950 driver setup requests its 2 mm feed bucket
       * and explicitly disables chain: ESC i M 42, then ESC i K 08. */
      .mode_feed_code = 0x02, .emit_no_chain_mode = true,
      .mode_before_compression = true,
      .completion_validated = false, .chain_validated = false,
      /* The PT-1950 supplies its own fixed head-to-cutter leader.  Adding a
       * synthetic one-inch raster floor duplicates that feed and lengthens
       * every short label; keep the raster at the rendered artwork length. */
      .minimum_cut_dots_180 = 0,
      .expected_status_model = 0x35,
      .tape_width_mask = PTOUCH_TAPE_WIDTHS_18 & ~PTOUCH_TAPE_WIDTH_4,
      .limitation =
          "18 mm maximum; a manual cut guide appears about 24 mm from the leading edge; automatic completion requires inspection; batch chaining is unavailable" },
    LEGACY_RAW(0x201F, "PT-2700", true, NULL),
    PROFILE_WIDTHS(0x202C, "PT-1230PC", 128, 180,
                   PTOUCH_RASTER_LEGACY_R, PTOUCH_ENCODING_RAW,
                   PTOUCH_INFO_NONE, PTOUCH_FINALIZE_STANDARD,
                   PTOUCH_TRANSFER_FRAMED, PTOUCH_COMPLETION_JOB_STATUS,
                   false, false, 0, 0, 0, PTOUCH_TAPE_WIDTHS_12,
                   "some units require additional leading blank head dots"),
    LEGACY_RAW(0x202D, "PT-2430PC", false, NULL),
    LEGACY_RAW(0x2041, "PT-2730", false,
               "reported to require additional leading whitespace"),
    LEGACY_PACKBITS(0x205E, "PT-H500", 128, 180, true,
                    "some units require additional trailing padding"),
    LEGACY_PACKBITS(0x205F, "PT-E500", 128, 180, false,
                    "some units require additional trailing padding"),
    PROFILE(0x2061, "PT-P700", 128, 180, PTOUCH_RASTER_DYNAMIC_A,
            PTOUCH_ENCODING_PACKBITS, PTOUCH_INFO_NONE,
            PTOUCH_FINALIZE_STANDARD, PTOUCH_TRANSFER_FRAMED,
            PTOUCH_COMPLETION_JOB_STATUS, true, false, 0, 0, 0, NULL),
    PROFILE(0x2062, "PT-P750W", 128, 180, PTOUCH_RASTER_DYNAMIC_A,
            PTOUCH_ENCODING_PACKBITS, PTOUCH_INFO_NONE,
            PTOUCH_FINALIZE_STANDARD, PTOUCH_TRANSFER_FRAMED,
            PTOUCH_COMPLETION_JOB_STATUS, false, false, 0, 0, 0, NULL),
    PROFILE(0x2073, "PT-D450", 128, 180, PTOUCH_RASTER_LEGACY_R,
            PTOUCH_ENCODING_RAW, PTOUCH_INFO_BASIC, PTOUCH_FINALIZE_STANDARD,
            PTOUCH_TRANSFER_FRAMED, PTOUCH_COMPLETION_JOB_STATUS,
            false, false, 0, 0, 0, "printable head width is hardware-unverified"),
    LEGACY_PACKBITS(0x2074, "PT-D600", 128, 180, false,
                    "reported 73 mm length limit and early-cut quirks"),
    { .vid = PTOUCH_BROTHER_VID, .pid = 0x2085, .name = "PT-P900Wc",
      .support = PTOUCH_SUPPORT_EXPERIMENTAL_RECIPE,
      .head_dots = 560, .head_dpi = 360, .feed_dpi = 360,
      .wire_offset_dots = 8, .raster_select = PTOUCH_RASTER_DYNAMIC_A,
      .encoding = PTOUCH_ENCODING_PACKBITS,
      .info_variant = PTOUCH_INFO_BASIC,
      .finalize_variant = PTOUCH_FINALIZE_STANDARD,
      .transfer_policy = PTOUCH_TRANSFER_FRAMED,
      .completion_strategy = PTOUCH_COMPLETION_JOB_STATUS,
      .autocut_strategy = PTOUCH_AUTOCUT_ESC_I_M,
      .completion_validated = false, .chain_validated = false,
      .tape_width_mask = PTOUCH_TAPE_WIDTHS_36,
      .limitation =
          "36 mm is hardware-reported; narrower media remain unverified" },
    { .vid = PTOUCH_BROTHER_VID, .pid = 0x20AF, .name = "PT-P710BT",
      .support = PTOUCH_SUPPORT_HARDWARE_VALIDATED,
      .head_dots = 128, .head_dpi = 180, .feed_dpi = 180,
      .raster_select = PTOUCH_RASTER_DYNAMIC_A,
      .encoding = PTOUCH_ENCODING_PACKBITS,
      .info_variant = PTOUCH_INFO_P710_RECOVER,
      .finalize_variant = PTOUCH_FINALIZE_STANDARD,
      .transfer_policy = PTOUCH_TRANSFER_COALESCED,
      .completion_strategy = PTOUCH_COMPLETION_STRICT_NOTIFICATIONS,
      .autocut_strategy = PTOUCH_AUTOCUT_ESC_I_M,
      .completion_validated = true, .chain_validated = true,
      .minimum_cut_dots_180 = 174, .trailing_pad_dots_180 = 12,
      .tape_width_mask = PTOUCH_TAPE_WIDTHS_24,
      .blank_line_shortcut = true },
    PROFILE_WIDTHS(0x20DF, "PT-D410", 128, 180, PTOUCH_RASTER_LEGACY_R,
            PTOUCH_ENCODING_RAW, PTOUCH_INFO_D460, PTOUCH_FINALIZE_D460,
            PTOUCH_TRANSFER_FRAMED, PTOUCH_COMPLETION_JOB_STATUS,
            true, false, 0, 0, 0, PTOUCH_TAPE_WIDTHS_18,
            "batch chaining is not hardware-validated"),
    PROFILE_WIDTHS(0x20E0, "PT-D460BT", 128, 180, PTOUCH_RASTER_DYNAMIC_A,
            PTOUCH_ENCODING_RAW, PTOUCH_INFO_D460, PTOUCH_FINALIZE_D460,
            PTOUCH_TRANSFER_FRAMED, PTOUCH_COMPLETION_JOB_STATUS,
            true, false, 0, 0, 0, PTOUCH_TAPE_WIDTHS_18,
            "batch chaining is not hardware-validated"),
    PROFILE(0x20E1, "PT-D610BT", 128, 180, PTOUCH_RASTER_DYNAMIC_A,
            PTOUCH_ENCODING_RAW, PTOUCH_INFO_D460, PTOUCH_FINALIZE_D460,
            PTOUCH_TRANSFER_FRAMED, PTOUCH_COMPLETION_JOB_STATUS,
            true, false, 0, 0, 0, "batch chaining is not hardware-validated"),
    PROFILE_WIDTHS(0x2201, "PT-E310BT", 128, 180, PTOUCH_RASTER_DYNAMIC_A,
            PTOUCH_ENCODING_RAW, PTOUCH_INFO_D460, PTOUCH_FINALIZE_D460,
            PTOUCH_TRANSFER_FRAMED, PTOUCH_COMPLETION_JOB_STATUS,
            false, false, 0, 0, 0, PTOUCH_TAPE_WIDTHS_18,
            "heat-shrink media are not validated"),
    PROFILE(0x2203, "PT-E560BT", 128, 180, PTOUCH_RASTER_DYNAMIC_A,
            PTOUCH_ENCODING_RAW, PTOUCH_INFO_D460, PTOUCH_FINALIZE_D460,
            PTOUCH_TRANSFER_FRAMED, PTOUCH_COMPLETION_JOB_STATUS,
            false, false, 0, 0, 0, "heat-shrink media are not validated"),

    { .vid = PTOUCH_BROTHER_VID, .pid = 0x2060, .name = "PT-E550W",
      .support = PTOUCH_SUPPORT_UNSUPPORTED_RASTER,
      },
    { .vid = PTOUCH_BROTHER_VID, .pid = 0x2030,
      .name = "PT-1230PC (P-Lite mode)", .support = PTOUCH_SUPPORT_P_LITE,
      },
    { .vid = PTOUCH_BROTHER_VID, .pid = 0x2031,
      .name = "PT-2430PC (P-Lite mode)", .support = PTOUCH_SUPPORT_P_LITE,
      },
    { .vid = PTOUCH_BROTHER_VID, .pid = 0x2064,
      .name = "PT-P700 (Editor Lite mode)", .support = PTOUCH_SUPPORT_P_LITE,
      },
    { .vid = PTOUCH_BROTHER_VID, .pid = 0x2065,
      .name = "PT-P750W (Editor Lite mode)", .support = PTOUCH_SUPPORT_P_LITE,
      },
    { .vid = PTOUCH_BROTHER_VID, .pid = 0x200D, .name = "PT-3600",
      .support = PTOUCH_SUPPORT_KNOWN_UNSUPPORTED,
      },
};

typedef struct {
    int mm;
    uint16_t dots_180;
    uint16_t dots_360;
} tape_geometry_t;

static const tape_geometry_t s_tapes[] = {
    { 4, 24, 48 }, { 6, 32, 64 }, { 9, 52, 106 }, { 12, 76, 150 },
    { 18, 120, 234 }, { 21, 124, 248 }, { 24, 128, 320 },
    { 36, 192, 454 },
};

const ptouch_model_profile_t *ptouch_model_lookup(uint16_t vid, uint16_t pid)
{
    for (size_t i = 0; i < ptouch_model_count(); ++i) {
        if (s_profiles[i].vid == vid && s_profiles[i].pid == pid) {
            return &s_profiles[i];
        }
    }
    return NULL;
}

size_t ptouch_model_count(void)
{
    return sizeof s_profiles / sizeof s_profiles[0];
}

const ptouch_model_profile_t *ptouch_model_at(size_t index)
{
    return index < ptouch_model_count() ? &s_profiles[index] : NULL;
}

const ptouch_model_profile_t *ptouch_model_p710bt(void)
{
    return ptouch_model_lookup(PTOUCH_BROTHER_VID, 0x20AF);
}

bool ptouch_model_has_print_recipe(const ptouch_model_profile_t *profile)
{
    return profile && profile->vid == PTOUCH_BROTHER_VID &&
           (profile->support == PTOUCH_SUPPORT_HARDWARE_VALIDATED ||
            profile->support == PTOUCH_SUPPORT_OUTPUT_INSPECTED ||
            profile->support == PTOUCH_SUPPORT_EXPERIMENTAL_RECIPE) &&
           profile->head_dots >= 8 &&
           profile->head_dots <= PTOUCH_MAX_HEAD_DOTS &&
           profile->head_dots % 8 == 0 &&
           (profile->head_dpi == 180 || profile->head_dpi == 360) &&
           (profile->feed_dpi == 180 || profile->feed_dpi == 360) &&
           profile->wire_offset_dots > -(int)profile->head_dots &&
           profile->wire_offset_dots < (int)profile->head_dots &&
           (int)profile->raster_select >= 0 &&
           profile->raster_select <= PTOUCH_RASTER_IMPLICIT &&
           (int)profile->encoding >= 0 &&
           profile->encoding <= PTOUCH_ENCODING_PACKBITS &&
           (int)profile->info_variant >= 0 &&
           profile->info_variant <= PTOUCH_INFO_D460 &&
           (int)profile->finalize_variant >= 0 &&
           profile->finalize_variant <= PTOUCH_FINALIZE_D460 &&
           (int)profile->transfer_policy >= 0 &&
           profile->transfer_policy <= PTOUCH_TRANSFER_FRAMED &&
           (int)profile->completion_strategy >= 0 &&
           profile->completion_strategy <= PTOUCH_COMPLETION_JOB_STATUS &&
           (int)profile->autocut_strategy >= 0 &&
           profile->autocut_strategy <= PTOUCH_AUTOCUT_ESC_I_M &&
           profile->mode_feed_code <= 0x1F &&
           (profile->autocut_strategy == PTOUCH_AUTOCUT_ESC_I_M ||
            profile->mode_feed_code == 0) &&
           (!profile->emit_no_chain_mode ||
            profile->autocut_strategy == PTOUCH_AUTOCUT_ESC_I_M) &&
           (!profile->mode_before_compression ||
            profile->autocut_strategy == PTOUCH_AUTOCUT_ESC_I_M) &&
           (!profile->chain_validated || profile->completion_validated) &&
           (!profile->completion_validated ||
            profile->support == PTOUCH_SUPPORT_HARDWARE_VALIDATED) &&
           profile->tape_width_mask != 0 &&
           (profile->tape_width_mask & ~PTOUCH_TAPE_WIDTHS_KNOWN) == 0 &&
           (profile->tape_width_mask & PTOUCH_TAPE_WIDTH_21) == 0;
}

bool ptouch_model_can_print(const ptouch_model_profile_t *profile)
{
    return ptouch_model_has_print_recipe(profile) &&
           profile->support != PTOUCH_SUPPORT_EXPERIMENTAL_RECIPE;
}

bool ptouch_model_output_allowed(const ptouch_model_profile_t *profile,
                                 bool allow_experimental)
{
    return ptouch_model_has_print_recipe(profile) &&
           (profile->support != PTOUCH_SUPPORT_EXPERIMENTAL_RECIPE ||
            allow_experimental);
}

uint16_t ptouch_model_tape_width_mask(
    const ptouch_model_profile_t *profile)
{
    return ptouch_model_has_print_recipe(profile) ? profile->tape_width_mask : 0;
}

static uint16_t tape_width_bit(int tape_width_mm)
{
    switch (tape_width_mm) {
    case 4: return PTOUCH_TAPE_WIDTH_4;
    case 6: return PTOUCH_TAPE_WIDTH_6;
    case 9: return PTOUCH_TAPE_WIDTH_9;
    case 12: return PTOUCH_TAPE_WIDTH_12;
    case 18: return PTOUCH_TAPE_WIDTH_18;
    case 21: return PTOUCH_TAPE_WIDTH_21;
    case 24: return PTOUCH_TAPE_WIDTH_24;
    case 36: return PTOUCH_TAPE_WIDTH_36;
    default: return 0;
    }
}

bool ptouch_model_accepts_width(const ptouch_model_profile_t *profile,
                                int tape_width_mm)
{
    uint16_t bit = tape_width_bit(tape_width_mm);
    return bit && (ptouch_model_tape_width_mask(profile) & bit) != 0;
}

bool ptouch_model_accepts_media(const ptouch_model_profile_t *profile,
                                int tape_width_mm, uint8_t media_type)
{
    /* Brother status values: 0x01 laminated, 0x03 non-laminated. Media 0x11
     * is heat-shrink and needs its own geometry/thermal validation. */
    return ptouch_model_accepts_width(profile, tape_width_mm) &&
           (media_type == 0x01 || media_type == 0x03);
}

const char *ptouch_support_name(ptouch_support_t support)
{
    switch (support) {
    case PTOUCH_SUPPORT_HARDWARE_VALIDATED: return "hardware_validated";
    case PTOUCH_SUPPORT_OUTPUT_INSPECTED: return "output_inspected";
    case PTOUCH_SUPPORT_EXPERIMENTAL_RECIPE: return "experimental_recipe";
    case PTOUCH_SUPPORT_P_LITE: return "p_lite";
    case PTOUCH_SUPPORT_UNSUPPORTED_RASTER: return "unsupported_raster";
    case PTOUCH_SUPPORT_KNOWN_UNSUPPORTED: return "known_unsupported";
    case PTOUCH_SUPPORT_UNKNOWN: return "unknown";
    default: return "unknown";
    }
}

bool ptouch_media_geometry(const ptouch_model_profile_t *profile,
                           int tape_width_mm,
                           ptouch_media_geometry_t *out)
{
    if (!out || !ptouch_model_has_print_recipe(profile)) return false;
    const tape_geometry_t *tape = NULL;
    for (size_t i = 0; i < sizeof s_tapes / sizeof s_tapes[0]; ++i) {
        if (s_tapes[i].mm == tape_width_mm) {
            tape = &s_tapes[i];
            break;
        }
    }
    if (!tape) return false;

    uint16_t dots = profile->head_dpi == 360 ? tape->dots_360 : tape->dots_180;
    if (dots > profile->head_dots) dots = profile->head_dots;
    int top = ((int)profile->head_dots - (int)dots) / 2 +
              profile->wire_offset_dots;
    if (top < 0 || top + dots > profile->head_dots) return false;

    memset(out, 0, sizeof *out);
    out->tape_width_mm = tape_width_mm;
    out->printable_dots = dots;
    out->logical_height_180 = (uint16_t)
        (((uint32_t)dots * 180U + profile->head_dpi - 1U) /
         profile->head_dpi);
    out->head_dots = profile->head_dots;
    out->row_bytes = (uint16_t)(profile->head_dots / 8U);
    out->head_dpi = profile->head_dpi;
    out->feed_dpi = profile->feed_dpi;
    out->wire_top_dot = (int16_t)top;
    return true;
}
