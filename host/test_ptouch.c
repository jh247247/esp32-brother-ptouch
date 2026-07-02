/*
 * test_ptouch.c — Host tests for the portable P-touch raster protocol core.
 *
 * Covers:
 *   1. PackBits round-trip (encode->decode == identity) over edge + random data.
 *   2. Known PackBits vectors vs the `packbits` reference oracle.
 *   3. Byte-layout unit tests: each command's exact bytes vs the Brother spec /
 *      brother_pt reference.
 *   4. parse_status with a synthetic 32-byte reply.
 *   5. Full print-job stream diffed byte-for-byte vs the brother_pt oracle.
 *
 * Prints per-check PASS/FAIL lines and a final "PASS: n  FAIL: n" summary.
 * Exit code 0 iff all checks pass.
 */
#include "ptouch_raster.h"
#include "oracle_generated.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int g_pass = 0;
static int g_fail = 0;

static void ok(int cond, const char *name)
{
    if (cond) {
        g_pass++;
        printf("PASS  %s\n", name);
    } else {
        g_fail++;
        printf("FAIL  %s\n", name);
    }
}

/* Compare two byte arrays; on mismatch print a hex diff and return 0. */
static int bytes_eq(const uint8_t *a, size_t alen,
                    const uint8_t *b, size_t blen, const char *name)
{
    if (alen != blen) {
        printf("      %s: length %zu != %zu\n", name, alen, blen);
        return 0;
    }
    for (size_t i = 0; i < alen; i++) {
        if (a[i] != b[i]) {
            printf("      %s: byte %zu got 0x%02X want 0x%02X\n",
                   name, i, a[i], b[i]);
            return 0;
        }
    }
    return 1;
}

/* ---- 1. PackBits round-trip ------------------------------------------- */

static void test_packbits_roundtrip(void)
{
    /* Deterministic pseudo-random + structured edge buffers. */
    uint32_t rng = 0xC0FFEEu;
    for (int trial = 0; trial < 2000; trial++) {
        size_t n = (size_t)(rng % 300u);
        rng = rng * 1664525u + 1013904223u;

        uint8_t *src = (uint8_t *)malloc(n ? n : 1);
        for (size_t i = 0; i < n; i++) {
            rng = rng * 1664525u + 1013904223u;
            /* Bias toward short alphabets so we exercise runs heavily. */
            uint32_t mode = (rng >> 16) & 3u;
            if (mode == 0)      src[i] = 0x00;
            else if (mode == 1) src[i] = 0xFF;
            else if (mode == 2) src[i] = (uint8_t)(rng >> 8);
            else                src[i] = (uint8_t)((rng >> 3) & 1u ? 0xAA : 0x55);
        }

        size_t bound = ptouch_packbits_max_encoded_size(n);
        uint8_t *enc = (uint8_t *)malloc(bound ? bound : 1);
        int enclen = ptouch_packbits_encode(src, n, enc, bound ? bound : 1);
        if (enclen < 0) {
            ok(0, "packbits_roundtrip (encode overflow)");
            free(src); free(enc);
            return;
        }
        uint8_t *dec = (uint8_t *)malloc(n ? n : 1);
        int declen = ptouch_packbits_decode(enc, (size_t)enclen, dec, n ? n : 1);
        int good = (declen == (int)n) && (n == 0 || memcmp(src, dec, n) == 0);
        if (!good) {
            printf("      roundtrip trial %d n=%zu declen=%d\n", trial, n, declen);
            ok(0, "packbits_roundtrip");
            free(src); free(enc); free(dec);
            return;
        }
        free(src); free(enc); free(dec);
    }
    ok(1, "packbits_roundtrip (2000 random/edge buffers)");

    /* Explicit edge cases. */
    uint8_t out[16], dec[16];
    ok(ptouch_packbits_encode((const uint8_t *)"", 0, out, sizeof out) == 0,
       "packbits_encode empty -> 0 bytes");
    uint8_t one = 0x5A;
    int e = ptouch_packbits_encode(&one, 1, out, sizeof out);
    ok(e == 2 && out[0] == 0x00 && out[1] == 0x5A,
       "packbits_encode single byte -> 00 5A");
    int d = ptouch_packbits_decode(out, 2, dec, sizeof dec);
    ok(d == 1 && dec[0] == 0x5A, "packbits_decode single byte");
}

/* ---- 2. Known PackBits vectors vs oracle ------------------------------ */

#define PB_VEC(name)                                                         \
    do {                                                                     \
        uint8_t enc[512];                                                    \
        int n = ptouch_packbits_encode(name##_in, name##_in_len,            \
                                       enc, sizeof enc);                     \
        int good = (n >= 0) &&                                               \
                   bytes_eq(enc, (size_t)n, name##_enc, name##_enc_len,     \
                            "encode " #name);                                \
        ok(good, "packbits_oracle encode " #name);                          \
        /* decode(oracle_enc) must reproduce the input */                    \
        uint8_t dec[512];                                                    \
        int dn = ptouch_packbits_decode(name##_enc, name##_enc_len,         \
                                        dec, sizeof dec);                    \
        int dgood = (dn == (int)name##_in_len) &&                           \
                    (name##_in_len == 0 ||                                   \
                     memcmp(dec, name##_in, name##_in_len) == 0);           \
        ok(dgood, "packbits_oracle decode " #name);                         \
    } while (0)

static void test_packbits_oracle(void)
{
    PB_VEC(pb_classic);
    PB_VEC(pb_all_ff);
    PB_VEC(pb_all_00);
    PB_VEC(pb_single);
    PB_VEC(pb_two_same);
    PB_VEC(pb_alt);
    PB_VEC(pb_raster1);
    PB_VEC(pb_run129);
    PB_VEC(pb_empty);
}

/* ---- 3. Command byte-layout unit tests -------------------------------- */

static void test_command_bytes(void)
{
    ptouch_buf_t b;

    ptouch_buf_init(&b);
    ptouch_build_invalidate(&b);
    int inv_ok = (b.len == 100);
    for (size_t i = 0; i < b.len; i++) if (b.data[i] != 0) inv_ok = 0;
    ok(inv_ok, "invalidate = 100 x 0x00");
    ptouch_buf_free(&b);

    ptouch_buf_init(&b);
    ptouch_build_init(&b);
    { uint8_t w[] = {0x1B, 0x40};
      ok(bytes_eq(b.data, b.len, w, sizeof w, "init"), "init = 1B 40"); }
    ptouch_buf_free(&b);

    ptouch_buf_init(&b);
    ptouch_build_status_request(&b);
    { uint8_t w[] = {0x1B, 0x69, 0x53};
      ok(bytes_eq(b.data, b.len, w, sizeof w, "status_req"),
         "status_request = 1B 69 53"); }
    ptouch_buf_free(&b);

    ptouch_buf_init(&b);
    ptouch_build_raster_mode(&b);
    { uint8_t w[] = {0x1B, 0x69, 0x61, 0x01};
      ok(bytes_eq(b.data, b.len, w, sizeof w, "raster_mode"),
         "raster_mode = 1B 69 61 01"); }
    ptouch_buf_free(&b);

    ptouch_buf_init(&b);
    ptouch_build_enable_status_notification(&b);
    { uint8_t w[] = {0x1B, 0x69, 0x21, 0x00};
      ok(bytes_eq(b.data, b.len, w, sizeof w, "notify"),
         "enable_status_notification = 1B 69 21 00"); }
    ptouch_buf_free(&b);

    ptouch_buf_init(&b);
    ptouch_build_print_info(&b, 24, 3);
    { uint8_t w[] = {0x1B, 0x69, 0x7A, 0x84, 0x00, 0x18, 0x00,
                     0x03, 0x00, 0x00, 0x00, 0x00, 0x00};
      ok(bytes_eq(b.data, b.len, w, sizeof w, "print_info"),
         "print_info(24,3) = 1B 69 7A 84 00 18 00 03000000 00 00"); }
    ptouch_buf_free(&b);

    /* Multi-byte line count little-endian check. */
    ptouch_buf_init(&b);
    ptouch_build_print_info(&b, 12, 0x01020304);
    { uint8_t w[] = {0x1B, 0x69, 0x7A, 0x84, 0x00, 0x0C, 0x00,
                     0x04, 0x03, 0x02, 0x01, 0x00, 0x00};
      ok(bytes_eq(b.data, b.len, w, sizeof w, "print_info LE"),
         "print_info line-count little-endian"); }
    ptouch_buf_free(&b);

    ptouch_buf_init(&b);
    ptouch_build_mode(&b, PTOUCH_MODE_AUTO_CUT);
    { uint8_t w[] = {0x1B, 0x69, 0x4D, 0x40};
      ok(bytes_eq(b.data, b.len, w, sizeof w, "mode"),
         "mode(AUTO_CUT) = 1B 69 4D 40"); }
    ptouch_buf_free(&b);

    ptouch_buf_init(&b);
    ptouch_build_advanced_mode(&b, PTOUCH_ADV_NO_CHAIN);
    { uint8_t w[] = {0x1B, 0x69, 0x4B, 0x08};
      ok(bytes_eq(b.data, b.len, w, sizeof w, "adv"),
         "advanced_mode(NO_CHAIN) = 1B 69 4B 08"); }
    ptouch_buf_free(&b);

    ptouch_buf_init(&b);
    ptouch_build_margin(&b, 0);
    { uint8_t w[] = {0x1B, 0x69, 0x64, 0x00, 0x00};
      ok(bytes_eq(b.data, b.len, w, sizeof w, "margin0"),
         "margin(0) = 1B 69 64 00 00"); }
    ptouch_buf_free(&b);

    ptouch_buf_init(&b);
    ptouch_build_margin(&b, 0x1234);
    { uint8_t w[] = {0x1B, 0x69, 0x64, 0x34, 0x12};
      ok(bytes_eq(b.data, b.len, w, sizeof w, "marginLE"),
         "margin(0x1234) little-endian = 1B 69 64 34 12"); }
    ptouch_buf_free(&b);

    ptouch_buf_init(&b);
    ptouch_build_compression_mode(&b);
    { uint8_t w[] = {0x4D, 0x02};
      ok(bytes_eq(b.data, b.len, w, sizeof w, "compress"),
         "compression_mode = 4D 02"); }
    ptouch_buf_free(&b);

    /* Empty raster line -> 'Z' shortcut. */
    ptouch_buf_init(&b);
    { uint8_t zero[16] = {0};
      ptouch_build_raster_line(&b, zero);
      uint8_t w[] = {0x5A};
      ok(bytes_eq(b.data, b.len, w, sizeof w, "raster Z"),
         "raster_line all-zero = 5A (Z)"); }
    ptouch_buf_free(&b);

    /* Non-empty raster line -> 'G' + LE length + packbits (vs oracle vector). */
    ptouch_buf_init(&b);
    ptouch_build_raster_line(&b, pb_raster1_in);
    { uint8_t want[3 + 64];
      size_t wl = 0;
      want[wl++] = 0x47;
      want[wl++] = (uint8_t)(pb_raster1_enc_len & 0xFF);
      want[wl++] = (uint8_t)((pb_raster1_enc_len >> 8) & 0xFF);
      memcpy(want + wl, pb_raster1_enc, pb_raster1_enc_len);
      wl += pb_raster1_enc_len;
      ok(bytes_eq(b.data, b.len, want, wl, "raster G"),
         "raster_line non-zero = 47 <len LE> <packbits>"); }
    ptouch_buf_free(&b);

    ptouch_buf_init(&b);
    ptouch_build_print(&b, PTOUCH_PRINT_FEED);
    { uint8_t w[] = {0x1A};
      ok(bytes_eq(b.data, b.len, w, sizeof w, "print feed"),
         "print(FEED) = 1A"); }
    ptouch_buf_free(&b);

    ptouch_buf_init(&b);
    ptouch_build_print(&b, PTOUCH_PRINT_CHAIN);
    { uint8_t w[] = {0x0C};
      ok(bytes_eq(b.data, b.len, w, sizeof w, "print chain"),
         "print(CHAIN) = 0C"); }
    ptouch_buf_free(&b);
}

/* ---- 4. parse_status -------------------------------------------------- */

static void test_parse_status(void)
{
    /* Synthetic 32-byte reply: PT-P710BT, 24mm laminated, cover-open error. */
    uint8_t s[32] = {0};
    s[0]  = 0x80;   /* print head mark */
    s[1]  = 0x20;   /* size = 32 */
    s[2]  = 0x42;   /* 'B' Brother code */
    s[3]  = 0x30;   /* series code */
    s[4]  = 0x59;   /* model code (arbitrary sentinel) */
    s[8]  = PTOUCH_ERR1_NO_MEDIA;                    /* error 1 */
    s[9]  = PTOUCH_ERR2_COVER_OPEN;                  /* error 2 */
    s[10] = 24;     /* media width mm */
    s[11] = PTOUCH_MEDIA_LAMINATED;
    s[15] = PTOUCH_MODE_AUTO_CUT;
    s[17] = 0;      /* media length */
    s[18] = PTOUCH_ST_ERROR_OCCURRED;
    s[19] = 0x01;   /* phase type: printing */
    s[20] = 0x00; s[21] = 0x14;   /* phase number big-endian = 0x0014 */
    s[22] = 0x01;   /* notification: cover open */
    s[24] = 0x01;   /* tape color: white */
    s[25] = 0x08;   /* text color: black */
    s[26] = 0x00;

    ptouch_status_t st;
    ok(ptouch_parse_status(s, &st), "parse_status returns true");
    ok(st.valid, "parse_status header valid");
    ok(st.model == 0x59, "parse_status model = 0x59");
    ok(st.media_width_mm == 24, "parse_status media_width = 24");
    ok(st.media_type == PTOUCH_MEDIA_LAMINATED, "parse_status media_type laminated");
    ok(st.mode == PTOUCH_MODE_AUTO_CUT, "parse_status mode auto-cut");
    ok(st.status_type == PTOUCH_ST_ERROR_OCCURRED, "parse_status status_type error");
    ok(st.phase_type == 0x01, "parse_status phase_type printing");
    ok(st.phase_number == 0x0014, "parse_status phase_number big-endian 0x0014");
    ok(st.notification_number == 0x01, "parse_status notification cover-open");
    ok(st.tape_color == 0x01, "parse_status tape_color white");
    ok(st.text_color == 0x08, "parse_status text_color black");
    ok(st.error_information_1 == PTOUCH_ERR1_NO_MEDIA, "parse_status err1 no-media");
    ok(st.error_information_2 == PTOUCH_ERR2_COVER_OPEN, "parse_status err2 cover-open");

    char msg[128];
    int nerr = ptouch_status_error_string(&st, msg, sizeof msg);
    ok(nerr == 2 && strcmp(msg, "no media|cover open") == 0,
       "status_error_string = \"no media|cover open\"");

    /* Clean status -> no errors, empty string. */
    uint8_t clean[32] = {0};
    clean[0] = 0x80; clean[1] = 0x20; clean[10] = 12;
    clean[18] = PTOUCH_ST_REPLY_TO_REQUEST;
    ptouch_status_t cst;
    ptouch_parse_status(clean, &cst);
    int cn = ptouch_status_error_string(&cst, msg, sizeof msg);
    ok(cn == 0 && msg[0] == '\0', "status_error_string clean = \"\"");
    ok(cst.valid && cst.media_width_mm == 12, "parse_status clean 12mm valid");

    /* Bad header -> valid=false but fields still populated. */
    uint8_t bad[32] = {0};
    bad[10] = 9;
    ptouch_status_t bst;
    ptouch_parse_status(bad, &bst);
    ok(!bst.valid && bst.media_width_mm == 9,
       "parse_status bad header -> valid=false, fields populated");

    ok(!ptouch_parse_status(NULL, &st), "parse_status NULL buf -> false");
}

/* ---- 5. Full-stream oracle diff --------------------------------------- */

static void test_full_stream_oracle(void)
{
    ptouch_buf_t b;
    ptouch_buf_init(&b);

    /* Reproduce brother_pt's print-job output order exactly. */
    ptouch_build_invalidate(&b);
    ptouch_build_init(&b);
    ptouch_build_status_request(&b);
    ptouch_build_raster_mode(&b);
    ptouch_build_enable_status_notification(&b);
    ptouch_build_print_info(&b, oracle_stream_media_width, oracle_stream_num_lines);
    ptouch_build_mode(&b, PTOUCH_MODE_AUTO_CUT);
    ptouch_build_advanced_mode(&b, PTOUCH_ADV_NO_CHAIN);
    ptouch_build_margin(&b, 0);
    ptouch_build_compression_mode(&b);
    for (unsigned i = 0; i < oracle_raster_len; i += PTOUCH_LINE_LENGTH_BYTES) {
        ptouch_build_raster_line(&b, &oracle_raster[i]);
    }
    ptouch_build_print(&b, PTOUCH_PRINT_FEED);

    ok(!b.oom, "full stream built without OOM");
    ok(bytes_eq(b.data, b.len, oracle_stream, oracle_stream_len, "full stream"),
       "full print-job stream == brother_pt oracle (172 bytes)");

    ptouch_buf_free(&b);
}

int main(void)
{
    printf("== ptouch raster protocol host tests ==\n\n");
    test_packbits_roundtrip();
    test_packbits_oracle();
    test_command_bytes();
    test_parse_status();
    test_full_stream_oracle();

    printf("\n== SUMMARY ==\nPASS: %d  FAIL: %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
