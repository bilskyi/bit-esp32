// Layout invariants for the provisioning screen.
//
// The panel is 128x64 and unforgiving: a line one character too long does not
// wrap, it disappears off the edge, and the difference is invisible until
// someone is standing in front of the device trying to read a code.
//
//   cd firmware/host && make test

#include <stdio.h>
#include <string.h>

#include "../main/setup_screen.h"

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

static bool lit_at(const uint8_t *fb, int x, int y) {
    if (x < 0 || y < 0 || x >= FACE_W || y >= FACE_H) return false;
    return (fb[(y / 8) * FACE_W + x] >> (y % 8)) & 1;
}

static int lit_count(const uint8_t *fb) {
    int n = 0;
    for (int i = 0; i < FACE_FB_BYTES; i++) {
        uint8_t b = fb[i];
        while (b) { n += b & 1; b >>= 1; }
    }
    return n;
}

static int lit_in_column(const uint8_t *fb, int x) {
    int n = 0;
    for (int y = 0; y < FACE_H; y++) if (lit_at(fb, x, y)) n++;
    return n;
}

static setup_screen_t sample(void) {
    setup_screen_t s;
    memset(&s, 0, sizeof(s));
    snprintf(s.ap_name, sizeof(s.ap_name), "Voice-3f2a");
    snprintf(s.address, sizeof(s.address), "192.168.4.1");
    snprintf(s.code, sizeof(s.code), "4271");
    s.status = SS_STATUS_WAITING;
    return s;
}

static void test_space_draws_nothing(void) {
    uint8_t fb[FACE_FB_BYTES];
    memset(fb, 0, sizeof(fb));
    ss_draw_text(fb, 0, 0, "   ");
    CHECK(lit_count(fb) == 0, "space drew %d pixels", lit_count(fb));
}

static void test_every_printable_character_draws_something(void) {
    // A mis-transcribed font table shows up here as a blank glyph long before
    // anyone notices a missing character in a code on the panel.
    for (int c = '!'; c <= '~'; c++) {
        uint8_t fb[FACE_FB_BYTES];
        memset(fb, 0, sizeof(fb));
        const char text[2] = {(char)c, '\0'};
        ss_draw_text(fb, 0, 0, text);
        CHECK(lit_count(fb) > 0, "'%c' (0x%02x) drew nothing", c, c);
    }
}

static void test_a_glyph_stays_inside_its_five_columns(void) {
    uint8_t fb[FACE_FB_BYTES];
    memset(fb, 0, sizeof(fb));
    ss_draw_text(fb, 0, 0, "A");
    CHECK(lit_in_column(fb, SS_GLYPH_W) == 0, "column %d has ink", SS_GLYPH_W);
    for (int x = SS_GLYPH_W; x < FACE_W; x++) {
        CHECK(lit_in_column(fb, x) == 0, "ink at x=%d from one glyph", x);
    }
}

static void test_a_glyph_stays_inside_its_seven_rows(void) {
    uint8_t fb[FACE_FB_BYTES];
    memset(fb, 0, sizeof(fb));
    ss_draw_text(fb, 0, 0, "A");
    for (int y = SS_GLYPH_H; y < FACE_H; y++) {
        for (int x = 0; x < FACE_W; x++) {
            CHECK(!lit_at(fb, x, y), "ink at y=%d from one glyph", y);
        }
    }
}

static void test_text_is_truncated_not_wrapped(void) {
    // 30 characters into a 21-column panel. The tail must be dropped, and
    // nothing may appear on the row below.
    uint8_t fb[FACE_FB_BYTES];
    memset(fb, 0, sizeof(fb));
    ss_draw_text(fb, 0, 0, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123");
    for (int y = SS_GLYPH_H; y < FACE_H; y++) {
        for (int x = 0; x < FACE_W; x++) {
            CHECK(!lit_at(fb, x, y), "wrapped: ink at (%d,%d)", x, y);
        }
    }
}

static void test_nothing_is_drawn_outside_the_framebuffer(void) {
    // Negative and past-the-edge origins must clip rather than corrupt. The
    // guard bytes catch a write outside fb entirely.
    uint8_t buf[FACE_FB_BYTES + 16];
    memset(buf, 0xAA, sizeof(buf));
    uint8_t *fb = buf + 8;
    memset(fb, 0, FACE_FB_BYTES);

    ss_draw_text(fb, -20, 0, "EDGE");
    ss_draw_text(fb, FACE_W - 2, 0, "EDGE");
    ss_draw_text(fb, 0, FACE_H - 2, "EDGE");

    for (int i = 0; i < 8; i++) {
        CHECK(buf[i] == 0xAA, "wrote %d bytes before fb", 8 - i);
        CHECK(buf[FACE_FB_BYTES + 8 + i] == 0xAA, "wrote past the end of fb");
    }
}

static void test_the_screen_shows_all_four_lines(void) {
    uint8_t fb[FACE_FB_BYTES];
    setup_screen_t s = sample();
    setup_screen_render(fb, &s);

    // Four rows of 7 pixels with gaps must all carry ink, and none may sit
    // below the panel. Checked as four disjoint horizontal bands.
    int bands_with_ink = 0;
    for (int band = 0; band < 4; band++) {
        int n = 0;
        for (int y = band * 16; y < band * 16 + SS_GLYPH_H && y < FACE_H; y++) {
            for (int x = 0; x < FACE_W; x++) if (lit_at(fb, x, y)) n++;
        }
        if (n > 0) bands_with_ink++;
    }
    CHECK(bands_with_ink == 4, "only %d of 4 lines drew", bands_with_ink);
}

static void test_render_clears_what_was_there(void) {
    uint8_t fb[FACE_FB_BYTES];
    memset(fb, 0xFF, sizeof(fb));
    setup_screen_t s = sample();
    setup_screen_render(fb, &s);
    CHECK(lit_count(fb) < FACE_FB_BYTES * 8 / 2, "did not clear: %d lit", lit_count(fb));
}

static void test_a_maximum_length_ap_name_does_not_overrun(void) {
    uint8_t buf[FACE_FB_BYTES + 16];
    memset(buf, 0xAA, sizeof(buf));
    uint8_t *fb = buf + 8;

    setup_screen_t s = sample();
    memset(s.ap_name, 'W', SS_LINE_MAX - 1);
    s.ap_name[SS_LINE_MAX - 1] = '\0';
    setup_screen_render(fb, &s);

    for (int i = 0; i < 8; i++) {
        CHECK(buf[i] == 0xAA, "overran before fb");
        CHECK(buf[FACE_FB_BYTES + 8 + i] == 0xAA, "overran after fb");
    }
}

static void test_every_status_has_distinct_text(void) {
    // A status the panel renders as an empty string is a device that has
    // stopped explaining itself at the moment it most needs to.
    for (int a = 0; a < SS_STATUS_COUNT; a++) {
        const char *ta = ss_status_text((ss_status_t)a);
        CHECK(ta != NULL && ta[0] != '\0', "status %d has no text", a);
        CHECK(strlen(ta) <= SS_COLS, "status %d is %zu cols, panel fits %d",
              a, strlen(ta), SS_COLS);
        for (int b = a + 1; b < SS_STATUS_COUNT; b++) {
            CHECK(strcmp(ta, ss_status_text((ss_status_t)b)) != 0,
                  "status %d and %d read the same", a, b);
        }
    }
}

int main(void) {
    test_space_draws_nothing();
    test_every_printable_character_draws_something();
    test_a_glyph_stays_inside_its_five_columns();
    test_a_glyph_stays_inside_its_seven_rows();
    test_text_is_truncated_not_wrapped();
    test_nothing_is_drawn_outside_the_framebuffer();
    test_the_screen_shows_all_four_lines();
    test_render_clears_what_was_there();
    test_a_maximum_length_ap_name_does_not_overrun();
    test_every_status_has_distinct_text();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
