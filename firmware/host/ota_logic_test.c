// The update logic that needs no board.
//
//   cd firmware/host && make test
//
// The header offsets below are not guesses: they were read out of a real
// firmware/build/voice_capture.bin before this file was written, and
// `make test` in Step 6 of the plan checks them against that binary again.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/ota_logic.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        checks++;                                         \
        if (!(cond)) {                                    \
            failures++;                                   \
            printf("  FAIL %s:%d  ", __func__, __LINE__); \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

// A header that ol_check_image() must accept, so every test below can spoil
// exactly one field and see that one field's verdict.
static void good_header(unsigned char *h) {
    memset(h, 0, OL_HEADER_MIN);
    h[0x00] = 0xE9;                    // esp_image_header_t.magic
    h[0x0C] = OL_CHIP_ID_ESP32C3;      // .chip_id, little-endian uint16
    h[0x0D] = 0;
    h[0x20] = 0x32;                    // esp_app_desc_t.magic_word,
    h[0x21] = 0x54;                    // 0xABCD5432 little-endian
    h[0x22] = 0xCD;
    h[0x23] = 0xAB;
    memcpy(h + 0x30, "v0.2.1", 7);     // .version, NUL included
    memcpy(h + 0x50, OL_PROJECT_NAME, sizeof(OL_PROJECT_NAME));
}

// ------------------------------------------------------------ the image

static void test_image_accepts_a_real_header(void) {
    unsigned char h[OL_HEADER_MIN];
    good_header(h);
    CHECK(ol_check_image(h, sizeof(h)) == OL_IMAGE_OK, "got %s",
          ol_verdict_text(ol_check_image(h, sizeof(h))));
}

static void test_image_rejects_a_short_buffer(void) {
    unsigned char h[OL_HEADER_MIN];
    good_header(h);
    CHECK(ol_check_image(h, OL_HEADER_MIN - 1) == OL_IMAGE_TOO_SHORT, "short buffer accepted");
    CHECK(ol_check_image(h, 0) == OL_IMAGE_TOO_SHORT, "empty buffer accepted");
}

static void test_image_rejects_a_zip(void) {
    unsigned char h[OL_HEADER_MIN];
    good_header(h);
    h[0x00] = 'P';  // PK\3\4 - somebody uploaded the release archive
    CHECK(ol_check_image(h, sizeof(h)) == OL_IMAGE_NOT_ESP, "zip accepted");
}

static void test_image_rejects_another_chip(void) {
    unsigned char h[OL_HEADER_MIN];
    good_header(h);
    h[0x0C] = 9;  // ESP32-C6
    CHECK(ol_check_image(h, sizeof(h)) == OL_IMAGE_WRONG_CHIP, "wrong chip accepted");
}

// The high byte of chip_id is part of the field, and a build for a chip whose
// id needs it must not pass by having the right low byte.
static void test_image_rejects_a_chip_id_hiding_in_the_high_byte(void) {
    unsigned char h[OL_HEADER_MIN];
    good_header(h);
    h[0x0D] = 1;  // 0x0105, not 5
    CHECK(ol_check_image(h, sizeof(h)) == OL_IMAGE_WRONG_CHIP, "high byte ignored");
}

static void test_image_rejects_a_missing_app_desc(void) {
    unsigned char h[OL_HEADER_MIN];
    good_header(h);
    h[0x20] = 0;
    CHECK(ol_check_image(h, sizeof(h)) == OL_IMAGE_NO_APP_DESC, "no app desc accepted");
}

static void test_image_rejects_another_project(void) {
    unsigned char h[OL_HEADER_MIN];
    good_header(h);
    memset(h + 0x50, 0, 32);
    memcpy(h + 0x50, "other_app", 10);
    CHECK(ol_check_image(h, sizeof(h)) == OL_IMAGE_WRONG_PROJECT, "foreign project accepted");
}

// A name that starts with ours and carries on is not ours: "voice_capture2"
// is a different project, and a prefix comparison would take it.
static void test_image_rejects_a_project_name_with_our_name_as_a_prefix(void) {
    unsigned char h[OL_HEADER_MIN];
    good_header(h);
    memset(h + 0x50, 0, 32);
    memcpy(h + 0x50, OL_PROJECT_NAME "2", sizeof(OL_PROJECT_NAME) + 1);
    CHECK(ol_check_image(h, sizeof(h)) == OL_IMAGE_WRONG_PROJECT, "prefix match accepted");
}

static void test_image_version_is_read_out(void) {
    unsigned char h[OL_HEADER_MIN];
    good_header(h);
    char v[33] = {0};
    CHECK(ol_image_version(h, sizeof(h), v, sizeof(v)), "returned false");
    CHECK(strcmp(v, "v0.2.1") == 0, "got %s", v);
}

static void test_image_version_truncates_rather_than_overflows(void) {
    unsigned char h[OL_HEADER_MIN];
    good_header(h);
    char v[4];
    CHECK(ol_image_version(h, sizeof(h), v, sizeof(v)), "returned false");
    CHECK(strcmp(v, "v0.") == 0, "got %s", v);
}

// 32 non-zero bytes is a malformed field, not a long version: esp_app_desc_t
// declares version[32] and the string inside it has to be terminated.
static void test_image_version_rejects_an_unterminated_field(void) {
    unsigned char h[OL_HEADER_MIN];
    good_header(h);
    memset(h + 0x30, 'x', 32);
    char v[33] = {0};
    CHECK(!ol_image_version(h, sizeof(h), v, sizeof(v)), "unterminated field accepted");
}

static void test_image_version_rejects_a_short_buffer(void) {
    unsigned char h[OL_HEADER_MIN];
    good_header(h);
    char v[33] = {0};
    CHECK(!ol_image_version(h, OL_HEADER_MIN - 1, v, sizeof(v)), "short buffer accepted");
}

// ------------------------------------------------------------ the frames

static void test_frame_type_recognises_all_three(void) {
    const char *b = "{\"type\":\"ota_begin\",\"size\":12}";
    CHECK(ol_frame_type(b, strlen(b)) == OL_FRAME_BEGIN, "begin missed");
    const char *e = "{\"type\":\"ota_end\"}";
    CHECK(ol_frame_type(e, strlen(e)) == OL_FRAME_END, "end missed");
    const char *a = "{\"type\":\"ota_abort\"}";
    CHECK(ol_frame_type(a, strlen(a)) == OL_FRAME_ABORT, "abort missed");
}

static void test_frame_type_tolerates_whitespace(void) {
    const char *b = "{ \"type\" : \"ota_begin\" , \"size\" : 12 }";
    CHECK(ol_frame_type(b, strlen(b)) == OL_FRAME_BEGIN, "spaced begin missed");
}

static void test_frame_type_ignores_a_state_frame(void) {
    const char *s = "{\"type\":\"state\",\"value\":\"speaking\"}";
    CHECK(ol_frame_type(s, strlen(s)) == OL_FRAME_OTHER, "state frame taken");
    const char *d = "{\"type\":\"done\"}";
    CHECK(ol_frame_type(d, strlen(d)) == OL_FRAME_OTHER, "done frame taken");
}

// The whole reason this is not a substring match. A spoken reply that
// happens to contain the words must not reflash the device.
static void test_frame_type_does_not_match_a_substring_elsewhere(void) {
    const char *t = "{\"type\":\"text\",\"value\":\"ota_begin\"}";
    CHECK(ol_frame_type(t, strlen(t)) == OL_FRAME_OTHER, "value matched as a type");
}

static void test_frame_type_does_not_match_a_longer_type(void) {
    const char *t = "{\"type\":\"ota_beginning\"}";
    CHECK(ol_frame_type(t, strlen(t)) == OL_FRAME_OTHER, "prefix matched");
}

static void test_frame_type_of_an_empty_body(void) {
    CHECK(ol_frame_type("", 0) == OL_FRAME_OTHER, "empty body matched");
    CHECK(ol_frame_type(NULL, 0) == OL_FRAME_OTHER, "null body matched");
}

static void test_field_u32_reads_a_size(void) {
    const char *b = "{\"type\":\"ota_begin\",\"size\":1099920}";
    uint32_t v = 0;
    CHECK(ol_field_u32(b, strlen(b), "size", &v), "returned false");
    CHECK(v == 1099920u, "got %u", (unsigned)v);
}

static void test_field_u32_tolerates_whitespace(void) {
    const char *b = "{\"size\" :  42 }";
    uint32_t v = 0;
    CHECK(ol_field_u32(b, strlen(b), "size", &v), "returned false");
    CHECK(v == 42u, "got %u", (unsigned)v);
}

static void test_field_u32_absent_returns_false(void) {
    const char *b = "{\"type\":\"ota_end\"}";
    uint32_t v = 7;
    CHECK(!ol_field_u32(b, strlen(b), "size", &v), "absent field accepted");
    CHECK(v == 7u, "out was written on failure");
}

static void test_field_u32_rejects_a_non_number(void) {
    const char *b = "{\"size\":\"big\"}";
    uint32_t v = 0;
    CHECK(!ol_field_u32(b, strlen(b), "size", &v), "string accepted as a number");
}

static void test_field_u32_rejects_an_overflowing_number(void) {
    const char *b = "{\"size\":99999999999999999999}";
    uint32_t v = 0;
    CHECK(!ol_field_u32(b, strlen(b), "size", &v), "overflow accepted");
}

static void test_field_u32_accepts_the_largest_u32(void) {
    const char *b = "{\"size\":4294967295}";
    uint32_t v = 0;
    CHECK(ol_field_u32(b, strlen(b), "size", &v), "returned false");
    CHECK(v == 4294967295u, "got %u", (unsigned)v);
}

static void test_field_u32_rejects_one_past_the_largest_u32(void) {
    const char *b = "{\"size\":4294967296}";
    uint32_t v = 0;
    CHECK(!ol_field_u32(b, strlen(b), "size", &v), "4294967296 accepted");
}

static void test_field_u32_does_not_match_a_suffix(void) {
    const char *b = "{\"mysize\":7}";
    uint32_t v = 0;
    CHECK(!ol_field_u32(b, strlen(b), "size", &v), "suffix matched");
}

// A body that is not NUL-terminated is what actually arrives: e->data_ptr
// points into the websocket client's receive buffer with e->data_len bytes
// of payload and no terminator of its own.
static void test_field_u32_respects_the_length_and_not_a_terminator(void) {
    const char full[] = "{\"size\":42}{\"size\":99}";
    uint32_t v = 0;
    CHECK(ol_field_u32(full, 11, "size", &v), "returned false");
    CHECK(v == 42u, "read past the length: got %u", (unsigned)v);
}

static void test_field_u32_does_not_read_past_a_truncated_value(void) {
    const char full[] = "{\"size\":123456}";
    uint32_t v = 0;
    // Cut mid-number: everything up to and including "123" and no further.
    CHECK(ol_field_u32(full, 11, "size", &v), "returned false");
    CHECK(v == 123u, "got %u", (unsigned)v);
}

// ------------------------------------------------------ the state machine

#define CAP (1024u * 1024u)

static void test_xfer_accepts_a_clean_run(void) {
    ol_xfer_t x;
    ol_xfer_reset(&x);
    CHECK(ol_xfer_begin(&x, 3000, CAP) == OL_STEP_OK, "begin refused");
    CHECK(x.state == OL_XFER_ACTIVE, "not active");
    CHECK(ol_xfer_write(&x, 1000) == OL_STEP_OK, "write 1 refused");
    CHECK(ol_xfer_write(&x, 1000) == OL_STEP_OK, "write 2 refused");
    CHECK(ol_xfer_write(&x, 1000) == OL_STEP_OK, "write 3 refused");
    CHECK(x.received == 3000u, "received %u", (unsigned)x.received);
    CHECK(ol_xfer_end(&x) == OL_STEP_OK, "end refused");
    CHECK(x.state == OL_XFER_DONE, "not done");
}

static void test_xfer_rejects_a_second_begin(void) {
    ol_xfer_t x;
    ol_xfer_reset(&x);
    CHECK(ol_xfer_begin(&x, 3000, CAP) == OL_STEP_OK, "begin refused");
    CHECK(ol_xfer_begin(&x, 3000, CAP) == OL_STEP_BUSY, "second begin accepted");
    // BUSY is the one rejection that must NOT destroy the running transfer.
    CHECK(x.state == OL_XFER_ACTIVE, "a stray begin killed a live transfer");
    CHECK(ol_xfer_write(&x, 3000) == OL_STEP_OK, "live transfer broken");
}

static void test_xfer_rejects_a_write_with_no_begin(void) {
    ol_xfer_t x;
    ol_xfer_reset(&x);
    CHECK(ol_xfer_write(&x, 10) == OL_STEP_NOT_ACTIVE, "write without begin accepted");
}

static void test_xfer_rejects_an_end_with_no_begin(void) {
    ol_xfer_t x;
    ol_xfer_reset(&x);
    CHECK(ol_xfer_end(&x) == OL_STEP_NOT_ACTIVE, "end without begin accepted");
}

static void test_xfer_rejects_a_size_past_the_partition(void) {
    ol_xfer_t x;
    ol_xfer_reset(&x);
    CHECK(ol_xfer_begin(&x, CAP + 1, CAP) == OL_STEP_TOO_BIG, "oversized accepted");
    CHECK(x.state == OL_XFER_IDLE, "left active after refusing");
}

static void test_xfer_accepts_a_size_exactly_filling_the_partition(void) {
    ol_xfer_t x;
    ol_xfer_reset(&x);
    CHECK(ol_xfer_begin(&x, CAP, CAP) == OL_STEP_OK, "exact fit refused");
}

static void test_xfer_rejects_a_zero_size(void) {
    ol_xfer_t x;
    ol_xfer_reset(&x);
    CHECK(ol_xfer_begin(&x, 0, CAP) == OL_STEP_EMPTY, "zero size accepted");
}

static void test_xfer_rejects_more_bytes_than_promised(void) {
    ol_xfer_t x;
    ol_xfer_reset(&x);
    ol_xfer_begin(&x, 100, CAP);
    CHECK(ol_xfer_write(&x, 90) == OL_STEP_OK, "write refused");
    CHECK(ol_xfer_write(&x, 20) == OL_STEP_OVERRUN, "overrun accepted");
    // A caller that ignores the return value must not be able to carry on.
    CHECK(x.state != OL_XFER_ACTIVE, "still active after an overrun");
    CHECK(ol_xfer_write(&x, 1) == OL_STEP_NOT_ACTIVE, "writes continued after an overrun");
}

static void test_xfer_rejects_an_early_end(void) {
    ol_xfer_t x;
    ol_xfer_reset(&x);
    ol_xfer_begin(&x, 100, CAP);
    ol_xfer_write(&x, 99);
    CHECK(ol_xfer_end(&x) == OL_STEP_SHORT, "short transfer accepted");
    CHECK(x.state != OL_XFER_ACTIVE, "still active after a short end");
}

static void test_xfer_rejects_a_second_end(void) {
    ol_xfer_t x;
    ol_xfer_reset(&x);
    ol_xfer_begin(&x, 10, CAP);
    ol_xfer_write(&x, 10);
    CHECK(ol_xfer_end(&x) == OL_STEP_OK, "end refused");
    CHECK(ol_xfer_end(&x) == OL_STEP_NOT_ACTIVE, "second end accepted");
}

static void test_xfer_reset_allows_a_retry(void) {
    ol_xfer_t x;
    ol_xfer_reset(&x);
    ol_xfer_begin(&x, 100, CAP);
    ol_xfer_write(&x, 200);  // overrun
    ol_xfer_reset(&x);
    CHECK(ol_xfer_begin(&x, 100, CAP) == OL_STEP_OK, "retry refused");
    CHECK(x.received == 0u, "received not cleared: %u", (unsigned)x.received);
}

static void test_xfer_a_zero_length_write_is_harmless(void) {
    ol_xfer_t x;
    ol_xfer_reset(&x);
    ol_xfer_begin(&x, 10, CAP);
    CHECK(ol_xfer_write(&x, 0) == OL_STEP_OK, "empty write refused");
    CHECK(x.received == 0u, "empty write counted");
}

// ------------------------------------------------------------------ text

static void test_step_and_verdict_text_cover_every_value(void) {
    for (int v = 0; v <= OL_IMAGE_WRONG_PROJECT; v++) {
        const char *t = ol_verdict_text((ol_image_verdict_t)v);
        CHECK(t != NULL && t[0] != '\0' && t[0] != '?', "verdict %d has no text", v);
    }
    for (int s = 0; s <= OL_STEP_SHORT; s++) {
        const char *t = ol_step_text((ol_step_t)s);
        CHECK(t != NULL && t[0] != '\0' && t[0] != '?', "step %d has no text", s);
    }
}

int main(void) {
    test_image_accepts_a_real_header();
    test_image_rejects_a_short_buffer();
    test_image_rejects_a_zip();
    test_image_rejects_another_chip();
    test_image_rejects_a_chip_id_hiding_in_the_high_byte();
    test_image_rejects_a_missing_app_desc();
    test_image_rejects_another_project();
    test_image_rejects_a_project_name_with_our_name_as_a_prefix();
    test_image_version_is_read_out();
    test_image_version_truncates_rather_than_overflows();
    test_image_version_rejects_an_unterminated_field();
    test_image_version_rejects_a_short_buffer();

    test_frame_type_recognises_all_three();
    test_frame_type_tolerates_whitespace();
    test_frame_type_ignores_a_state_frame();
    test_frame_type_does_not_match_a_substring_elsewhere();
    test_frame_type_does_not_match_a_longer_type();
    test_frame_type_of_an_empty_body();
    test_field_u32_reads_a_size();
    test_field_u32_tolerates_whitespace();
    test_field_u32_absent_returns_false();
    test_field_u32_rejects_a_non_number();
    test_field_u32_rejects_an_overflowing_number();
    test_field_u32_accepts_the_largest_u32();
    test_field_u32_rejects_one_past_the_largest_u32();
    test_field_u32_does_not_match_a_suffix();
    test_field_u32_respects_the_length_and_not_a_terminator();
    test_field_u32_does_not_read_past_a_truncated_value();

    test_xfer_accepts_a_clean_run();
    test_xfer_rejects_a_second_begin();
    test_xfer_rejects_a_write_with_no_begin();
    test_xfer_rejects_an_end_with_no_begin();
    test_xfer_rejects_a_size_past_the_partition();
    test_xfer_accepts_a_size_exactly_filling_the_partition();
    test_xfer_rejects_a_zero_size();
    test_xfer_rejects_more_bytes_than_promised();
    test_xfer_rejects_an_early_end();
    test_xfer_rejects_a_second_end();
    test_xfer_reset_allows_a_retry();
    test_xfer_a_zero_length_write_is_harmless();

    test_step_and_verdict_text_cover_every_value();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
