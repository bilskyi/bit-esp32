// Layout invariants for the settings carousel.
//
// The same job setup_screen_test.c does for the provisioning screen: at
// 128x64 a line one character too long does not wrap, it leaves the panel,
// and nobody finds out until they are standing in front of the device.
//
//   cd firmware/host && make test

#include <stdio.h>
#include <string.h>

#include "../main/settings_screen.h"

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

// The rows the title and the dots occupy. settings_screen.c keeps its own
// layout constants private, the same way the rest of this file already
// hardcodes the rows it samples - these two are pulled to the top because
// more than one test below needs them to agree with each other.
#define TEST_TITLE_TOP 0
#define TEST_TITLE_ROWS 7
#define TEST_DOT_TOP 56
#define TEST_DOT_ROWS 5

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

static int lit_in_row(const uint8_t *fb, int y) {
    int n = 0;
    for (int x = 0; x < FACE_W; x++) if (lit_at(fb, x, y)) n++;
    return n;
}

// Counts contiguous lit runs on one row of fb.
static int count_runs(const uint8_t *fb, int y) {
    int runs = 0;
    bool inside = false;
    for (int x = 0; x < FACE_W; x++) {
        const bool on = lit_at(fb, x, y);
        if (on && !inside) runs++;
        inside = on;
    }
    return runs;
}

static settings_t opened_on(uint8_t page) {
    settings_t s;
    settings_init(&s, 3, 2, 1);
    s.open = true;
    s.page = page;
    return s;
}

static void test_every_page_draws_something(void) {
    uint8_t fb[FACE_FB_BYTES];
    for (uint8_t page = 0; page < SETTINGS_PAGES; page++) {
        settings_t s = opened_on(page);
        settings_screen_render(fb, &s);
        CHECK(lit_count(fb) > 40, "page %u drew only %d pixels", page, lit_count(fb));
    }
}

static void test_render_clears_what_was_there(void) {
    uint8_t fb[FACE_FB_BYTES];
    memset(fb, 0xff, sizeof(fb));
    settings_t s = opened_on(SETTINGS_PAGE_VOLUME);
    settings_screen_render(fb, &s);
    CHECK(lit_count(fb) < FACE_W * FACE_H / 2,
          "a full framebuffer survived the render: %d pixels", lit_count(fb));
}

static void test_the_page_dots_count_the_pages(void) {
    uint8_t fb[FACE_FB_BYTES];
    settings_t s = opened_on(SETTINGS_PAGE_VOLUME);
    settings_screen_render(fb, &s);

    // The dot row is where the dots start; nothing else reaches it.
    const int runs = count_runs(fb, TEST_DOT_TOP);
    CHECK(runs == SETTINGS_PAGES, "%d dots for %d pages", runs, SETTINGS_PAGES);
}

static void test_the_current_page_has_the_fattest_dot(void) {
    uint8_t fb[FACE_FB_BYTES];
    for (uint8_t page = 0; page < SETTINGS_PAGES; page++) {
        settings_t s = opened_on(page);
        settings_screen_render(fb, &s);
        int widest = -1, widest_run = 0, run = 0, index = -1, seen = -1;
        for (int x = 0; x <= FACE_W; x++) {
            const bool on = (x < FACE_W) && lit_at(fb, x, TEST_DOT_TOP);
            if (on) {
                if (run == 0) index++;
                run++;
            } else if (run > 0) {
                if (run > widest_run) { widest_run = run; widest = index; }
                run = 0;
            }
        }
        seen = widest;
        CHECK(seen == (int)page, "page %u marked dot %d", page, seen);
    }
}

static void test_the_gauge_has_one_cell_per_step(void) {
    uint8_t fb[FACE_FB_BYTES];
    for (uint8_t page = 0; page < SETTINGS_PAGES; page++) {
        const uint8_t steps = settings_page_steps(page);
        if (steps == 0) continue;          // the wifi page has no gauge
        if (page == SETTINGS_PAGE_EYES) continue;  // it previews the pose instead
        settings_t s = opened_on(page);
        settings_screen_render(fb, &s);
        const uint8_t filled = s.step[page];

        // Row 13 sits inside the gauge's top border, which is solid across a
        // cell's whole width whether the cell is filled or outlined - so this
        // only proves the cells are all there and evenly spaced, not that an
        // unfilled one is actually outlined rather than solid.
        CHECK(count_runs(fb, 13) == steps,
              "page %u drew %d cells for %u steps at the border row", page, count_runs(fb, 13), steps);

        // Row 20 is clear of both borders. There a filled cell is one solid
        // run and an outlined cell is two - its left and right edges - so
        // this is the row where filled and outlined actually look different,
        // and the one a fill_rect standing in for frame_rect would not
        // survive.
        const int expected = filled + 2 * (steps - filled);
        CHECK(count_runs(fb, 20) == expected,
              "page %u drew %d runs at mid-gauge, expected %d (filled=%u of %u steps)",
              page, count_runs(fb, 20), expected, filled, steps);
    }
}

static void test_nothing_is_drawn_outside_the_framebuffer(void) {
    // A canary either side of the buffer: a render that writes past an edge
    // is caught here even when ASan is not available.
    //
    // hold_pct is swept past 100 as well as up to it: the field is only
    // ever set to 0-100 by settings_tick(), but settings_screen_render()
    // does not get to assume that - it clamps hold_pct itself before
    // scaling FACE_W by it, and this is the only thing that would notice if
    // that clamp were ever lost. Without it, set_px()'s own bounds check
    // would quietly absorb the overflow and nothing would fail.
    struct { uint8_t before[16]; uint8_t fb[FACE_FB_BYTES]; uint8_t after[16]; } m;
    const uint8_t hold_pcts[] = {90, 100, 255};
    for (size_t p = 0; p < sizeof(hold_pcts) / sizeof(hold_pcts[0]); p++) {
        for (uint8_t page = 0; page < SETTINGS_PAGES; page++) {
            memset(&m, 0xa5, sizeof(m));
            settings_t s = opened_on(page);
            s.hold_pct = hold_pcts[p];
            settings_screen_render(m.fb, &s);
            for (int i = 0; i < 16; i++) {
                CHECK(m.before[i] == 0xa5, "page %u hold_pct %u wrote %d bytes before the buffer",
                      page, hold_pcts[p], i);
                CHECK(m.after[i] == 0xa5, "page %u hold_pct %u wrote %d bytes after the buffer",
                      page, hold_pcts[p], i);
            }
        }
    }
}

static void test_the_hold_bar_grows_and_disappears(void) {
    uint8_t fb[FACE_FB_BYTES];
    settings_t s = opened_on(SETTINGS_PAGE_VOLUME);

    s.hold_pct = 0;
    settings_screen_render(fb, &s);
    const int none = lit_in_row(fb, 63);

    s.hold_pct = 50;
    settings_screen_render(fb, &s);
    const int half = lit_in_row(fb, 63);

    s.hold_pct = 100;
    settings_screen_render(fb, &s);
    const int full = lit_in_row(fb, 63);

    CHECK(none == 0, "a bar was drawn at 0%%: %d pixels", none);
    CHECK(half > 0 && half < full, "50%% (%d) is not between 0 and 100%% (%d)", half, full);
    CHECK(full == FACE_W, "100%% drew %d of %d pixels", full, FACE_W);
}

static void test_the_eyes_page_draws_a_face_not_a_gauge(void) {
    uint8_t fb[FACE_FB_BYTES];
    settings_t eyes = opened_on(SETTINGS_PAGE_EYES);
    settings_t volume = opened_on(SETTINGS_PAGE_VOLUME);
    uint8_t other[FACE_FB_BYTES];

    settings_screen_render(fb, &eyes);
    settings_screen_render(other, &volume);
    CHECK(lit_count(fb) > lit_count(other),
          "the eyes page (%d px) is not fuller than a gauge page (%d px)",
          lit_count(fb), lit_count(other));

    // Each choice must look different, or the page is showing nothing useful.
    // Compared as whole framebuffers rather than as pixel counts: two poses
    // can light the same number of pixels in different places, and a count
    // would call that a pass.
    uint8_t drawn[SETTINGS_EYES_STEPS][FACE_FB_BYTES];
    for (uint8_t i = 0; i < SETTINGS_EYES_STEPS; i++) {
        eyes.step[SETTINGS_PAGE_EYES] = i;
        settings_screen_render(drawn[i], &eyes);
    }
    for (uint8_t i = 1; i < SETTINGS_EYES_STEPS; i++) {
        for (uint8_t j = 0; j < i; j++) {
            CHECK(memcmp(drawn[i], drawn[j], FACE_FB_BYTES) != 0,
                  "eyes steps %u and %u render identically", i, j);
        }
    }
}

static bool rows_equal(const uint8_t *a, const uint8_t *b, int top, int rows) {
    for (int y = top; y < top + rows; y++)
        for (int x = 0; x < FACE_W; x++)
            if (lit_at(a, x, y) != lit_at(b, x, y)) return false;
    return true;
}

static void test_the_eyes_pose_does_not_bleed_into_title_or_dots(void) {
    // Calm is step 0 - FACE_EMO_NEUTRAL, the one pose in the vocabulary that
    // is rest in every field - so it stands in for "the same page rendered
    // with a blank pose" without this test needing a way to render no pose
    // at all. Every other step's title and dot rows must match it exactly:
    // face_render_pose() fills the whole panel before the title and the dots
    // go down, so whatever it leaves under them is a rendering bug the moment
    // it differs from what an inert pose leaves there.
    uint8_t drawn[SETTINGS_EYES_STEPS][FACE_FB_BYTES];
    settings_t eyes = opened_on(SETTINGS_PAGE_EYES);
    for (uint8_t i = 0; i < SETTINGS_EYES_STEPS; i++) {
        eyes.step[SETTINGS_PAGE_EYES] = i;
        settings_screen_render(drawn[i], &eyes);
    }
    for (uint8_t i = 1; i < SETTINGS_EYES_STEPS; i++) {
        CHECK(rows_equal(drawn[i], drawn[0], TEST_TITLE_TOP, TEST_TITLE_ROWS),
              "eyes step %u's title band does not match step 0's - the pose is showing through", i);
        CHECK(rows_equal(drawn[i], drawn[0], TEST_DOT_TOP, TEST_DOT_ROWS),
              "eyes step %u's dot band does not match step 0's - the pose is showing through", i);
    }
}

static void test_the_hold_bar_never_reaches_the_dots(void) {
    // The bar and the dots are positioned from separate constants; nothing
    // but a test stops them drifting onto the same row. Comparing a
    // full-bar render against a no-bar render across the dots' whole
    // vertical span catches that collision however it happens, without this
    // test needing to know the dots' own x positions.
    uint8_t fb_no_bar[FACE_FB_BYTES];
    uint8_t fb_full_bar[FACE_FB_BYTES];
    for (uint8_t page = 0; page < SETTINGS_PAGES; page++) {
        settings_t s = opened_on(page);
        s.hold_pct = 0;
        settings_screen_render(fb_no_bar, &s);
        s.hold_pct = 100;
        settings_screen_render(fb_full_bar, &s);
        for (int y = TEST_DOT_TOP; y < TEST_DOT_TOP + TEST_DOT_ROWS; y++) {
            for (int x = 0; x < FACE_W; x++) {
                CHECK(lit_at(fb_no_bar, x, y) == lit_at(fb_full_bar, x, y),
                      "page %u (%d,%d): a full hold bar changed a dot-row pixel",
                      page, x, y);
            }
        }
    }
}

int main(void) {
    test_every_page_draws_something();
    test_render_clears_what_was_there();
    test_the_page_dots_count_the_pages();
    test_the_current_page_has_the_fattest_dot();
    test_the_gauge_has_one_cell_per_step();
    test_nothing_is_drawn_outside_the_framebuffer();
    test_the_hold_bar_grows_and_disappears();
    test_the_eyes_page_draws_a_face_not_a_gauge();
    test_the_eyes_pose_does_not_bleed_into_title_or_dots();
    test_the_hold_bar_never_reaches_the_dots();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
