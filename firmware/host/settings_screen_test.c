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

    // The dot row is the last thing on the panel and nothing else reaches it.
    int runs = 0;
    bool inside = false;
    for (int x = 0; x < FACE_W; x++) {
        const bool on = lit_at(fb, x, 58);
        if (on && !inside) runs++;
        inside = on;
    }
    CHECK(runs == SETTINGS_PAGES, "%d dots for %d pages", runs, SETTINGS_PAGES);
}

static void test_the_current_page_has_the_fattest_dot(void) {
    uint8_t fb[FACE_FB_BYTES];
    for (uint8_t page = 0; page < SETTINGS_PAGES; page++) {
        settings_t s = opened_on(page);
        settings_screen_render(fb, &s);
        int widest = -1, widest_run = 0, run = 0, index = -1, seen = -1;
        for (int x = 0; x <= FACE_W; x++) {
            const bool on = (x < FACE_W) && lit_at(fb, x, 58);
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

        int runs = 0;
        bool inside = false;
        for (int x = 0; x < FACE_W; x++) {
            const bool on = lit_at(fb, x, 13);  // one row inside the gauge
            if (on && !inside) runs++;
            inside = on;
        }
        CHECK(runs == steps, "page %u drew %d cells for %u steps", page, runs, steps);
    }
}

static void test_nothing_is_drawn_outside_the_framebuffer(void) {
    // A canary either side of the buffer: a render that writes past an edge
    // is caught here even when ASan is not available.
    struct { uint8_t before[16]; uint8_t fb[FACE_FB_BYTES]; uint8_t after[16]; } m;
    memset(&m, 0xa5, sizeof(m));
    for (uint8_t page = 0; page < SETTINGS_PAGES; page++) {
        settings_t s = opened_on(page);
        s.hold_pct = 90;
        settings_screen_render(m.fb, &s);
        for (int i = 0; i < 16; i++) {
            CHECK(m.before[i] == 0xa5, "page %u wrote %d bytes before the buffer", page, i);
            CHECK(m.after[i] == 0xa5, "page %u wrote %d bytes after the buffer", page, i);
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

int main(void) {
    test_every_page_draws_something();
    test_render_clears_what_was_there();
    test_the_page_dots_count_the_pages();
    test_the_current_page_has_the_fattest_dot();
    test_the_gauge_has_one_cell_per_step();
    test_nothing_is_drawn_outside_the_framebuffer();
    test_the_hold_bar_grows_and_disappears();
    test_the_eyes_page_draws_a_face_not_a_gauge();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
