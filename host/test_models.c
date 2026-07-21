#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ptouch_model.h"
#include "ptouch_print.h"
#include "ptouch_usb_descriptor.h"

static bool has_bytes(const uint8_t *data, size_t len,
                      const uint8_t *needle, size_t needle_len)
{
    if (!data || !needle || needle_len > len) return false;
    for (size_t i = 0; i + needle_len <= len; ++i) {
        if (memcmp(data + i, needle, needle_len) == 0) return true;
    }
    return false;
}

static size_t count_exact_frames(const ptouch_print_job_t *job,
                                 const uint8_t *bytes, size_t len)
{
    size_t count = 0;
    size_t start = 0;
    for (size_t i = 0; i < job->frame_count; ++i) {
        size_t end = job->frame_ends[i];
        if (end - start == len && memcmp(job->stream.data + start,
                                        bytes, len) == 0) {
            ++count;
        }
        start = end;
    }
    return count;
}

static uint64_t fnv1a64(const uint8_t *data, size_t len)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void assert_only_bytes(const uint8_t *row, size_t row_len,
                              size_t byte_a, uint8_t value_a,
                              size_t byte_b, uint8_t value_b)
{
    for (size_t i = 0; i < row_len; ++i) {
        uint8_t expected = 0;
        if (i == byte_a) expected |= value_a;
        if (i == byte_b) expected |= value_b;
        assert(row[i] == expected);
    }
}

static void decode_raster_frame(const ptouch_print_job_t *job,
                                size_t frame_index,
                                uint8_t *row, size_t row_len)
{
    assert(job && frame_index < job->frame_count);
    size_t start = frame_index ? job->frame_ends[frame_index - 1] : 0;
    size_t end = job->frame_ends[frame_index];
    assert(end > start + 3 && job->stream.data[start] == 0x47);
    size_t packed_len = (size_t)job->stream.data[start + 1] |
                        ((size_t)job->stream.data[start + 2] << 8);
    assert(start + 3 + packed_len == end);
    assert(ptouch_packbits_decode(job->stream.data + start + 3,
                                  packed_len, row, row_len) == (int)row_len);
}

static void test_catalog(void)
{
    static const uint16_t recipes[] = {
        0x2001, 0x2002, 0x2004, 0x2007, 0x2011, 0x2019, 0x201F,
        0x202C, 0x202D, 0x2041, 0x205E, 0x205F, 0x2061, 0x2062,
        0x2073, 0x2074, 0x2085, 0x20AF, 0x20DF, 0x20E0, 0x20E1,
        0x2201, 0x2203,
    };
    assert(ptouch_model_count() == 29);
    size_t recipe_count = 0;
    size_t default_enabled = 0;
    for (size_t i = 0; i < ptouch_model_count(); ++i) {
        const ptouch_model_profile_t *a = ptouch_model_at(i);
        assert(a && a->vid == PTOUCH_BROTHER_VID && a->name && a->name[0]);
        for (size_t j = i + 1; j < ptouch_model_count(); ++j) {
            const ptouch_model_profile_t *b = ptouch_model_at(j);
            assert(a->pid != b->pid);
        }
        if (ptouch_model_has_print_recipe(a)) {
            ++recipe_count;
            assert(a->head_dots % 8 == 0);
            assert(a->head_dots <= PTOUCH_MAX_HEAD_DOTS);
        }
        if (ptouch_model_can_print(a)) ++default_enabled;
    }
    assert(recipe_count == sizeof recipes / sizeof recipes[0]);
    assert(default_enabled == 2); /* PT-1950 inspected + PT-P710BT validated */
    for (size_t i = 0; i < sizeof recipes / sizeof recipes[0]; ++i) {
        const ptouch_model_profile_t *profile =
            ptouch_model_lookup(PTOUCH_BROTHER_VID, recipes[i]);
        assert(ptouch_model_has_print_recipe(profile));
        assert(ptouch_model_output_allowed(profile, true));
        assert(ptouch_model_output_allowed(profile, false) ==
               (profile->support != PTOUCH_SUPPORT_EXPERIMENTAL_RECIPE));
    }
    assert(ptouch_model_lookup(PTOUCH_BROTHER_VID, 0x2060)->support ==
           PTOUCH_SUPPORT_UNSUPPORTED_RASTER);
    assert(ptouch_model_lookup(PTOUCH_BROTHER_VID, 0x2030)->support ==
           PTOUCH_SUPPORT_P_LITE);
    assert(ptouch_model_lookup(PTOUCH_BROTHER_VID, 0x200D)->support ==
           PTOUCH_SUPPORT_KNOWN_UNSUPPORTED);
    assert(!ptouch_model_lookup(PTOUCH_BROTHER_VID, 0xFFFF));
    assert(!ptouch_model_lookup(0x1234, 0x2019));
}

static void test_geometry(void)
{
    const ptouch_model_profile_t *pt1950 =
        ptouch_model_lookup(PTOUCH_BROTHER_VID, 0x2019);
    ptouch_media_geometry_t g;
    assert(ptouch_media_geometry(pt1950, 12, &g));
    assert(g.printable_dots == 76 && g.logical_height_180 == 76);
    assert(g.head_dots == 112 && g.row_bytes == 14 && g.wire_top_dot == 18);
    assert(ptouch_media_geometry(pt1950, 18, &g));
    assert(g.printable_dots == 112 && g.wire_top_dot == 0);
    assert(!ptouch_media_geometry(pt1950, 29, &g));

    const ptouch_model_profile_t *p900 =
        ptouch_model_lookup(PTOUCH_BROTHER_VID, 0x2085);
    assert(ptouch_media_geometry(p900, 36, &g));
    assert(g.printable_dots == 454 && g.logical_height_180 == 227);
    assert(g.row_bytes == 70 && g.wire_top_dot == 61);
    assert(ptouch_media_geometry(p900, 24, &g));
    assert(g.printable_dots == 320 && g.logical_height_180 == 160);

    const ptouch_model_profile_t *p710 = ptouch_model_p710bt();
    static const int widths[] = {4, 6, 9, 12, 18, 21, 24, 36};
    static const int dots[] = {24, 32, 52, 76, 120, 124, 128, 128};
    for (size_t i = 0; i < sizeof widths / sizeof widths[0]; ++i) {
        assert(ptouch_media_geometry(p710, widths[i], &g));
        assert(g.printable_dots == dots[i]);
    }
}

static void test_pt1950_job(void)
{
    const ptouch_model_profile_t *profile =
        ptouch_model_lookup(PTOUCH_BROTHER_VID, 0x2019);
    uint8_t px[2 * 76] = {0};
    px[0 * 2 + 0] = 1;
    px[17 * 2 + 0] = 1;
    px[7 * 2 + 1] = 1;
    px[33 * 2 + 1] = 1;
    ptouch_print_opts_t opts = PTOUCH_PRINT_OPTS_DEFAULT();
    opts.tape_mm = 12;
    opts.trailing_pad_dots = 0;
    opts.min_length_dots = 0;

    ptouch_print_job_t job;
    ptouch_print_job_init(&job);
    assert(ptouch_print_build_job_for_model(profile, px, 2, 76, &opts, &job));
    assert(profile->autocut_strategy == PTOUCH_AUTOCUT_ESC_I_M);
    assert(profile->mode_feed_code == 0x02);
    assert(profile->emit_no_chain_mode);
    assert(profile->mode_before_compression);
    assert(profile->raster_select == PTOUCH_RASTER_IMPLICIT);
    assert(job.frame_count == 7);
    static const size_t expected_ends[] = {
        102, 106, 110, 112, 123, 136, 137,
    };
    assert(memcmp(job.frame_ends, expected_ends, sizeof expected_ends) == 0);
    assert(job.frame_ends[0] == 102);
    assert(job.stream.data[100] == 0x1B && job.stream.data[101] == 0x40);
    assert(job.stream.data[102] == 0x1B && job.stream.data[103] == 0x69 &&
           job.stream.data[104] == 0x4D && job.stream.data[105] == 0x42);
    assert(job.stream.data[106] == 0x1B && job.stream.data[107] == 0x69 &&
           job.stream.data[108] == 0x4B &&
           job.stream.data[109] == PTOUCH_ADV_NO_CHAIN);
    assert(job.stream.data[110] == 0x4D && job.stream.data[111] == 0x02);
    static const uint8_t mode_auto[] = {
        0x1B, 0x69, 0x4D, 0x42,
    };
    static const uint8_t mode_off[] = {0x1B, 0x69, 0x4D, 0x02};
    static const uint8_t no_chain[] = {
        0x1B, 0x69, 0x4B, PTOUCH_ADV_NO_CHAIN,
    };
    assert(count_exact_frames(&job, mode_auto, sizeof mode_auto) == 1);
    assert(count_exact_frames(&job, mode_off, sizeof mode_off) == 0);
    assert(count_exact_frames(&job, no_chain, sizeof no_chain) == 1);
    uint8_t row[14] = {0};
    decode_raster_frame(&job, 4, row, sizeof row);
    assert_only_bytes(row, sizeof row, 2, 0x20, 4, 0x10);
    memset(row, 0, sizeof row);
    decode_raster_frame(&job, 5, row, sizeof row);
    assert_only_bytes(row, sizeof row, 3, 0x40, 6, 0x10);
    assert(job.stream.data[job.stream.len - 1] == PTOUCH_PRINT_FEED);
    ptouch_print_job_free(&job);

    /* The real local-print path must not duplicate the PT-1950's fixed
     * head-to-cutter leader with a synthetic raster-length floor. */
    assert(profile->minimum_cut_dots_180 == 0);
    static uint8_t physical_retry[170 * 76];
    ptouch_print_job_init(&job);
    assert(ptouch_print_build_job_for_model(profile, physical_retry, 168, 76,
                                            NULL, &job));
    assert(job.frame_count == 173); /* four setup + 168 raster + final feed */
    ptouch_print_job_free(&job);

    physical_retry[0 * 170 + 0] = 1;
    physical_retry[17 * 170 + 0] = 1;
    physical_retry[7 * 170 + 1] = 1;
    physical_retry[33 * 170 + 1] = 1;
    ptouch_print_job_init(&job);
    assert(ptouch_print_build_job_for_model(profile, physical_retry, 170, 76,
                                            NULL, &job));
    assert(job.frame_count == 175);
    uint8_t unpadded[14] = {0};
    decode_raster_frame(&job, 4, unpadded, sizeof unpadded);
    assert_only_bytes(unpadded, sizeof unpadded, 2, 0x20, 4, 0x10);
    memset(unpadded, 0, sizeof unpadded);
    decode_raster_frame(&job, 5, unpadded, sizeof unpadded);
    assert_only_bytes(unpadded, sizeof unpadded, 3, 0x40, 6, 0x10);
    assert(job.stream.data[job.stream.len - 1] == PTOUCH_PRINT_FEED);
    ptouch_print_job_free(&job);

    uint8_t blank = 0;
    ptouch_print_job_init(&job);
    assert(ptouch_print_build_job_for_model(profile, &blank, 1, 1,
                                            &opts, &job));
    assert(job.stream.data[job.frame_ends[3]] == 0x47);
    ptouch_print_job_free(&job);

    opts.autocut = false;
    ptouch_print_job_init(&job);
    assert(ptouch_print_build_job_for_model(profile, &blank, 1, 1,
                                            &opts, &job));
    assert(job.stream.data[102] == 0x1B && job.stream.data[103] == 0x69 &&
           job.stream.data[104] == 0x4D && job.stream.data[105] == 0x02);
    assert(count_exact_frames(&job, mode_auto, sizeof mode_auto) == 0);
    assert(count_exact_frames(&job, mode_off, sizeof mode_off) == 1);
    assert(count_exact_frames(&job, no_chain, sizeof no_chain) == 1);
    ptouch_print_job_free(&job);

    opts = PTOUCH_PRINT_OPTS_DEFAULT();
    opts.tape_mm = 12;
    opts.chain = true;
    ptouch_print_job_init(&job);
    assert(!ptouch_print_build_job_for_model(profile, &blank, 1, 1,
                                             &opts, &job));
    assert(job.stream.len == 0 && job.frame_count == 0);
    ptouch_print_job_free(&job);
}

static void test_raw_and_d460(void)
{
    uint8_t blank = 0;
    ptouch_print_opts_t opts = PTOUCH_PRINT_OPTS_DEFAULT();
    opts.tape_mm = 12;
    opts.trailing_pad_dots = 0;
    opts.min_length_dots = 0;

    ptouch_print_job_t raw;
    ptouch_print_job_init(&raw);
    const ptouch_model_profile_t *pt1230 =
        ptouch_model_lookup(PTOUCH_BROTHER_VID, 0x202C);
    assert(ptouch_print_build_job_for_model(pt1230, &blank, 1, 1,
                                            &opts, &raw));
    size_t raster = raw.frame_ends[1];
    assert(raw.stream.data[raster] == 0x47);
    assert(raw.stream.data[raster + 1] == 16 &&
           raw.stream.data[raster + 2] == 0);
    assert(raw.frame_ends[2] - raw.frame_ends[1] == 19);
    ptouch_print_job_free(&raw);

    ptouch_print_job_t d460;
    ptouch_print_job_init(&d460);
    const ptouch_model_profile_t *profile =
        ptouch_model_lookup(PTOUCH_BROTHER_VID, 0x20E0);
    assert(ptouch_print_build_job_for_model(profile, &blank, 1, 1,
                                            &opts, &d460));
    const uint8_t info_prefix[] = {0x1B, 0x69, 0x7A, 0x00, 0x00, 0x0C};
    const uint8_t margin[] = {0x1B, 0x69, 0x64, 0x01, 0x00, 0x4D, 0x00};
    assert(has_bytes(d460.stream.data, d460.stream.len,
                     info_prefix, sizeof info_prefix));
    assert(has_bytes(d460.stream.data, d460.stream.len,
                     margin, sizeof margin));
    static const size_t d460_ends[] = {
        102, 106, 119, 126, 130, 149, 150,
    };
    assert(d460.frame_count == 7);
    assert(memcmp(d460.frame_ends, d460_ends, sizeof d460_ends) == 0);
    assert(d460.stream.data[126] == 0x1B &&
           d460.stream.data[127] == 0x69 &&
           d460.stream.data[128] == 0x4D &&
           d460.stream.data[129] == PTOUCH_MODE_AUTO_CUT);
    assert(d460.stream.data[d460.stream.len - 1] == PTOUCH_PRINT_FEED);
    ptouch_print_job_free(&d460);
}

static void test_p710_regression_and_p900_offset(void)
{
    uint8_t px[7 * 13] = {0};
    px[0 * 7 + 0] = 1;
    px[2 * 7 + 6] = 1;
    px[5 * 7 + 1] = 1;
    px[9 * 7 + 4] = 1;
    px[12 * 7 + 2] = 1;
    ptouch_print_opts_t opts = PTOUCH_PRINT_OPTS_DEFAULT();

    /* Independent byte-stream goldens frozen from scanner commit 2a3e141.
     * These are deliberately not computed through another new-code wrapper. */
    ptouch_buf_t legacy;
    ptouch_buf_init(&legacy);
    assert(ptouch_print_build_stream(px, 7, 13, NULL, &legacy));
    assert(legacy.len == 198);
    assert(fnv1a64(legacy.data, legacy.len) == UINT64_C(0x79a1386f193dd58b));
    const uint8_t p710_plan[] = {
        0x1B, 0x40, 0x1B, 0x69, 0x61, 0x01,
        0x1B, 0x69, 0x21, 0x00, 0x1B, 0x69, 0x7A, 0x84,
    };
    assert(has_bytes(legacy.data + 100, legacy.len - 100,
                     p710_plan, sizeof p710_plan));
    ptouch_buf_free(&legacy);

    ptouch_print_job_t p710_job;
    ptouch_print_job_init(&p710_job);
    assert(ptouch_model_p710bt()->autocut_strategy ==
           PTOUCH_AUTOCUT_ESC_I_M);
    assert(ptouch_print_build_job_for_model(ptouch_model_p710bt(),
                                            px, 7, 13, NULL, &p710_job));
    assert(p710_job.stream.len == 353);
    assert(fnv1a64(p710_job.stream.data, p710_job.stream.len) ==
           UINT64_C(0x58ab235c437ab2e6));
    static const size_t p710_command_ends[] = {
        102, 106, 110, 123, 127, 131, 136, 138,
    };
    assert(p710_job.frame_count == 183);
    assert(memcmp(p710_job.frame_ends, p710_command_ends,
                  sizeof p710_command_ends) == 0);
    ptouch_print_job_free(&p710_job);

    ptouch_print_job_init(&p710_job);
    opts = PTOUCH_PRINT_OPTS_DEFAULT();
    opts.autocut = false;
    assert(ptouch_print_build_job_for_model(ptouch_model_p710bt(),
                                            px, 7, 13, &opts, &p710_job));
    static const uint8_t p710_mode_auto[] = {
        0x1B, 0x69, 0x4D, PTOUCH_MODE_AUTO_CUT,
    };
    static const uint8_t p710_mode_off[] = {0x1B, 0x69, 0x4D, 0x00};
    assert(count_exact_frames(&p710_job, p710_mode_auto,
                              sizeof p710_mode_auto) == 0);
    assert(count_exact_frames(&p710_job, p710_mode_off,
                              sizeof p710_mode_off) == 1);
    ptouch_print_job_free(&p710_job);

    opts = PTOUCH_PRINT_OPTS_DEFAULT();
    opts.min_length_dots = PTOUCH_MIN_CUT_DOTS;
    ptouch_buf_init(&legacy);
    assert(ptouch_print_build_stream(px, 7, 13, &opts, &legacy));
    assert(legacy.len == 353);
    assert(fnv1a64(legacy.data, legacy.len) == UINT64_C(0x58ab235c437ab2e6));
    ptouch_buf_free(&legacy);

    opts = PTOUCH_PRINT_OPTS_DEFAULT();
    opts.chain = true;
    ptouch_buf_init(&legacy);
    assert(ptouch_print_build_stream(px, 7, 13, &opts, &legacy));
    assert(legacy.len == 186);
    assert(fnv1a64(legacy.data, legacy.len) == UINT64_C(0xf2ea1e07dac06eb9));
    ptouch_buf_free(&legacy);

    opts = PTOUCH_PRINT_OPTS_DEFAULT();
    opts.mirror = true;
    ptouch_buf_init(&legacy);
    assert(ptouch_print_build_stream(px, 7, 13, &opts, &legacy));
    assert(legacy.len == 198);
    assert(fnv1a64(legacy.data, legacy.len) == UINT64_C(0xd4f0c0be41dc8f6b));
    ptouch_buf_free(&legacy);

    const ptouch_model_profile_t *p900 =
        ptouch_model_lookup(PTOUCH_BROTHER_VID, 0x2085);
    uint8_t p900_px[454] = {0};
    p900_px[0] = 1;
    p900_px[9] = 1;
    p900_px[453] = 1;
    opts = PTOUCH_PRINT_OPTS_DEFAULT();
    opts.tape_mm = 36;
    opts.trailing_pad_dots = 0;
    opts.min_length_dots = 0;
    ptouch_print_job_t job;
    ptouch_print_job_init(&job);
    assert(ptouch_print_build_job_for_model(p900, p900_px, 1, 454,
                                            &opts, &job));
    static const size_t p900_ends[] = {102, 104, 108, 121, 125, 139, 140};
    assert(job.frame_count == 7);
    assert(memcmp(job.frame_ends, p900_ends, sizeof p900_ends) == 0);
    /* Full-height 36 mm media begins at wire dot 61, including +8 shift. */
    assert(job.stream.data[121] == 0x1B && job.stream.data[122] == 0x69 &&
           job.stream.data[123] == 0x4D &&
           job.stream.data[124] == PTOUCH_MODE_AUTO_CUT);
    size_t first_raster = job.frame_ends[4];
    assert(job.stream.data[first_raster] == 0x47);
    size_t n = (size_t)job.stream.data[first_raster + 1] |
               ((size_t)job.stream.data[first_raster + 2] << 8);
    uint8_t row[70] = {0};
    assert(ptouch_packbits_decode(job.stream.data + first_raster + 3,
                                  n, row, sizeof row) == 70);
    for (size_t i = 0; i < sizeof row; ++i) {
        uint8_t expected = i == 7 ? 0x04 :
                           i == 8 ? 0x02 :
                           i == 64 ? 0x20 : 0;
        assert(row[i] == expected);
    }
    ptouch_print_job_free(&job);
}

static void test_media_and_profile_validation(void)
{
    const ptouch_model_profile_t *pt1950 =
        ptouch_model_lookup(PTOUCH_BROTHER_VID, 0x2019);
    const ptouch_model_profile_t *p710 = ptouch_model_p710bt();
    const ptouch_model_profile_t *p900 =
        ptouch_model_lookup(PTOUCH_BROTHER_VID, 0x2085);
    const ptouch_model_profile_t *e310 =
        ptouch_model_lookup(PTOUCH_BROTHER_VID, 0x2201);

    assert(!ptouch_model_accepts_width(pt1950, 4));
    assert(ptouch_model_accepts_width(pt1950, 6));
    assert(ptouch_model_accepts_width(pt1950, 18));
    assert(!ptouch_model_accepts_width(pt1950, 21));
    assert(!ptouch_model_accepts_width(pt1950, 24));
    assert(!ptouch_model_accepts_width(pt1950, 36));
    assert(ptouch_model_accepts_width(p710, 24));
    assert(!ptouch_model_accepts_width(p710, 21));
    assert(!ptouch_model_accepts_width(p710, 36));
    assert(ptouch_model_accepts_width(p900, 36));
    assert(ptouch_model_accepts_width(e310, 18));
    assert(!ptouch_model_accepts_width(e310, 24));

    static const uint16_t autocut_mode_pids[] = {
        0x2001, 0x2002, 0x2004, 0x2019, 0x201F, 0x205E,
        0x2061, 0x2085, 0x20AF, 0x20DF, 0x20E0, 0x20E1,
    };
    size_t autocut_index = 0;
    for (size_t i = 0; i < ptouch_model_count(); ++i) {
        const ptouch_model_profile_t *candidate = ptouch_model_at(i);
        if (candidate->autocut_strategy != PTOUCH_AUTOCUT_ESC_I_M) continue;
        assert(ptouch_model_has_print_recipe(candidate));
        assert(autocut_index <
               sizeof autocut_mode_pids / sizeof autocut_mode_pids[0]);
        assert(candidate->pid == autocut_mode_pids[autocut_index++]);
    }
    assert(autocut_index ==
           sizeof autocut_mode_pids / sizeof autocut_mode_pids[0]);
    assert(pt1950->autocut_strategy == PTOUCH_AUTOCUT_ESC_I_M);
    assert(pt1950->support == PTOUCH_SUPPORT_OUTPUT_INSPECTED);
    assert(!pt1950->completion_validated && !pt1950->chain_validated);
    assert(ptouch_model_can_print(pt1950));
    assert(p710->support == PTOUCH_SUPPORT_HARDWARE_VALIDATED);
    size_t completion_validated = 0;
    size_t chain_validated = 0;
    for (size_t i = 0; i < ptouch_model_count(); ++i) {
        const ptouch_model_profile_t *candidate = ptouch_model_at(i);
        if (candidate->completion_validated) {
            ++completion_validated;
            assert(candidate->pid == 0x20AF);
        }
        if (candidate->chain_validated) {
            ++chain_validated;
            assert(candidate->pid == 0x20AF);
        }
    }
    assert(completion_validated == 1 && chain_validated == 1);

    uint8_t blank = 0;
    ptouch_print_opts_t no_cut = PTOUCH_PRINT_OPTS_DEFAULT();
    no_cut.tape_mm = 12;
    no_cut.autocut = false;
    no_cut.trailing_pad_dots = 0;
    ptouch_print_job_t rejected;
    ptouch_print_job_init(&rejected);
    const ptouch_model_profile_t *pt1230 =
        ptouch_model_lookup(PTOUCH_BROTHER_VID, 0x202C);
    assert(!ptouch_print_build_job_for_model(pt1230, &blank, 1, 1,
                                             &no_cut, &rejected));
    assert(rejected.stream.len == 0 && rejected.frame_count == 0);
    ptouch_print_job_free(&rejected);

    assert(ptouch_model_accepts_media(pt1950, 12, 0x01));
    assert(ptouch_model_accepts_media(pt1950, 12, 0x03));
    assert(!ptouch_model_accepts_media(pt1950, 12, 0x11));
    assert(!ptouch_model_accepts_media(pt1950, 12, 0xFF));
    assert(!ptouch_model_accepts_media(pt1950, 24, 0x01));

    ptouch_model_profile_t invalid = *p710;
    invalid.vid = 0x1234;
    assert(!ptouch_model_can_print(&invalid));
    invalid = *p710;
    invalid.head_dots = PTOUCH_MAX_HEAD_DOTS + 8;
    assert(!ptouch_model_can_print(&invalid));
    invalid = *p710;
    invalid.encoding = (ptouch_encoding_t)-1;
    assert(!ptouch_model_can_print(&invalid));
    invalid = *p710;
    invalid.autocut_strategy = (ptouch_autocut_strategy_t)-1;
    assert(!ptouch_model_can_print(&invalid));
    invalid = *p710;
    invalid.mode_feed_code = 0x20;
    assert(!ptouch_model_can_print(&invalid));
    invalid = *p710;
    invalid.completion_validated = false;
    assert(!ptouch_model_can_print(&invalid));
    invalid = *p710;
    invalid.support = PTOUCH_SUPPORT_EXPERIMENTAL_RECIPE;
    assert(!ptouch_model_can_print(&invalid));
    invalid = *p710;
    invalid.tape_width_mask = (uint16_t)(1U << 15);
    assert(!ptouch_model_can_print(&invalid));
    invalid = *p710;
    invalid.tape_width_mask |= PTOUCH_TAPE_WIDTH_21;
    assert(!ptouch_model_can_print(&invalid));
}

static void test_every_recipe_builds_with_profile_defaults(void)
{
    static const int widths[] = {4, 6, 9, 12, 18, 24, 36};
    uint8_t pixel = 1;
    size_t built = 0;

    for (size_t i = 0; i < ptouch_model_count(); ++i) {
        const ptouch_model_profile_t *profile = ptouch_model_at(i);
        if (!ptouch_model_has_print_recipe(profile)) continue;

        int tape_mm = 0;
        for (size_t j = 0; j < sizeof widths / sizeof widths[0]; ++j) {
            if (ptouch_model_accepts_width(profile, widths[j])) {
                tape_mm = widths[j];
                break;
            }
        }
        assert(tape_mm != 0);

        ptouch_print_opts_t opts;
        assert(ptouch_print_opts_init(profile, tape_mm, &opts));
        assert(opts.tape_mm == tape_mm && opts.autocut && !opts.chain);
        assert(opts.trailing_pad_dots == profile->trailing_pad_dots_180);
        assert(opts.min_length_dots == profile->minimum_cut_dots_180);

        ptouch_print_job_t job;
        ptouch_print_job_init(&job);
        assert(ptouch_print_build_job_for_model(profile, &pixel, 1, 1,
                                                &opts, &job));
        assert(ptouch_print_job_frames_valid(&job));
        ptouch_print_job_free(&job);
        ++built;
    }
    assert(built == 23);

    ptouch_print_opts_t p710;
    assert(ptouch_print_opts_init(ptouch_model_p710bt(), 12, &p710));
    assert(p710.trailing_pad_dots == 12);
    assert(p710.min_length_dots == PTOUCH_MIN_CUT_DOTS);

    const ptouch_model_profile_t *pt1950 =
        ptouch_model_lookup(PTOUCH_BROTHER_VID, 0x2019);
    ptouch_print_opts_t legacy;
    assert(ptouch_print_opts_init(pt1950, 12, &legacy));
    assert(legacy.trailing_pad_dots == 0 && legacy.min_length_dots == 0);
    assert(!ptouch_print_opts_init(pt1950, 24, &legacy));
}

static void test_frame_validation(void)
{
    uint8_t px = 1;
    ptouch_print_opts_t opts = PTOUCH_PRINT_OPTS_DEFAULT();
    opts.trailing_pad_dots = 0;
    opts.min_length_dots = 0;
    ptouch_print_job_t job;
    ptouch_print_job_init(&job);
    assert(ptouch_print_build_job_for_model(ptouch_model_p710bt(),
                                            &px, 1, 1, &opts, &job));
    assert(ptouch_print_job_frames_valid(&job));

    size_t saved = job.frame_ends[2];
    job.frame_ends[2] = job.frame_ends[1];
    assert(!ptouch_print_job_frames_valid(&job));
    job.frame_ends[2] = saved;
    saved = job.frame_ends[job.frame_count - 1];
    job.frame_ends[job.frame_count - 1] = job.stream.len - 1;
    assert(!ptouch_print_job_frames_valid(&job));
    job.frame_ends[job.frame_count - 1] = saved;
    size_t saved_capacity = job.frame_capacity;
    job.frame_capacity = job.frame_count - 1;
    assert(!ptouch_print_job_frames_valid(&job));
    job.frame_capacity = saved_capacity;
    job.profile = NULL;
    assert(!ptouch_print_job_frames_valid(&job));
    ptouch_print_job_free(&job);
}

static void test_usb_descriptor_selection(void)
{
    static const uint8_t valid[] = {
        9, 0x02, 32, 0, 1, 1, 0, 0x80, 50,
        9, 0x04, 0, 0, 2, 7, 1, 2, 0,
        7, 0x05, 0x02, 0x02, 64, 0, 0,
        7, 0x05, 0x81, 0x02, 64, 0, 0,
    };
    ptouch_usb_bulk_pair_t pair;
    assert(ptouch_usb_find_printer_bulk_pair(valid, sizeof valid, &pair));
    assert(pair.interface_number == 0 && pair.alternate_setting == 0);
    assert(pair.bulk_out_ep == 0x02 && pair.bulk_in_ep == 0x81);
    assert(pair.bulk_out_mps == 64 && pair.bulk_in_mps == 64);

    uint8_t reversed[sizeof valid];
    memcpy(reversed, valid, sizeof reversed);
    memcpy(reversed + 18, valid + 25, 7);
    memcpy(reversed + 25, valid + 18, 7);
    assert(ptouch_usb_find_printer_bulk_pair(reversed, sizeof reversed, &pair));

    static const uint8_t split_interfaces[] = {
        9, 0x02, 41, 0, 2, 1, 0, 0x80, 50,
        9, 0x04, 0, 0, 1, 7, 1, 2, 0,
        7, 0x05, 0x02, 0x02, 64, 0, 0,
        9, 0x04, 1, 0, 1, 7, 1, 2, 0,
        7, 0x05, 0x81, 0x02, 64, 0, 0,
    };
    assert(!ptouch_usb_find_printer_bulk_pair(
        split_interfaces, sizeof split_interfaces, &pair));

    uint8_t invalid[sizeof valid];
    memcpy(invalid, valid, sizeof invalid);
    invalid[18 + 4] = 0;
    assert(!ptouch_usb_find_printer_bulk_pair(invalid, sizeof invalid, &pair));
    memcpy(invalid, valid, sizeof invalid);
    invalid[9 + 3] = 1;
    assert(!ptouch_usb_find_printer_bulk_pair(invalid, sizeof invalid, &pair));
    memcpy(invalid, valid, sizeof invalid);
    invalid[18 + 2] = 0x03;
    assert(!ptouch_usb_find_printer_bulk_pair(invalid, sizeof invalid, &pair));
    memcpy(invalid, valid, sizeof invalid);
    invalid[18] = 20;
    assert(!ptouch_usb_find_printer_bulk_pair(invalid, sizeof invalid, &pair));
    assert(!ptouch_usb_find_printer_bulk_pair(valid, sizeof valid - 1, &pair));

    /* Device-controlled descriptors must remain memory-safe under arbitrary
     * truncation and byte patterns. A positive parse still has to describe one
     * direction-correct bulk pair with non-zero packet sizes. */
    uint32_t state = UINT32_C(0x7A11C0DE);
    uint8_t fuzz[256];
    for (size_t iteration = 0; iteration < 10000; ++iteration) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        size_t length = (size_t)(state & 0xFFU);
        for (size_t i = 0; i < length; ++i) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            fuzz[i] = (uint8_t)state;
        }
        if (ptouch_usb_find_printer_bulk_pair(fuzz, length, &pair)) {
            assert((pair.bulk_out_ep & 0x80U) == 0);
            assert((pair.bulk_in_ep & 0x80U) != 0);
            assert(pair.bulk_out_mps > 0 && pair.bulk_in_mps > 0);
        }
    }
}

int main(void)
{
    test_catalog();
    test_geometry();
    test_pt1950_job();
    test_raw_and_d460();
    test_p710_regression_and_p900_offset();
    test_media_and_profile_validation();
    test_every_recipe_builds_with_profile_defaults();
    test_frame_validation();
    test_usb_descriptor_selection();
    puts("ptouch model/profile tests passed");
    return 0;
}
