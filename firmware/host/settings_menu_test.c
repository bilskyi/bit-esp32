// Every gesture the settings carousel answers to, without a board.
//
// The panel is not involved here: this file drives settings_tick() with a
// clock it controls and asserts what came out. The screen has its own test.
//
//   cd firmware/host && make test

#include <stdio.h>
#include <string.h>

#include "../main/settings_menu.h"

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

// Drives the clock forward in 40 ms steps - the tick face_task actually runs
// at - holding the buttons as asked.
static uint32_t run(settings_t *s, bool a, bool b, uint32_t ms, uint32_t from) {
    uint32_t t = from;
    for (uint32_t elapsed = 0; elapsed < ms; elapsed += 40) {
        t += 40;
        settings_tick(s, a, b, t);
    }
    return t;
}

static settings_t fresh(void) {
    settings_t s;
    settings_init(&s, 5, 3, 0);
    return s;
}

static void test_a_short_b_hold_does_not_open(void) {
    settings_t s = fresh();
    uint32_t t = run(&s, false, true, 1500, 1000);
    run(&s, false, false, 200, t);
    CHECK(!s.open, "1.5 s opened the menu");
}

static void test_a_two_second_b_hold_opens_on_the_first_page(void) {
    settings_t s = fresh();
    run(&s, false, true, 2200, 1000);
    CHECK(s.open, "2.2 s did not open the menu");
    CHECK(s.page == SETTINGS_PAGE_VOLUME, "opened on page %u", s.page);
}

static void test_the_opening_release_is_not_a_tap(void) {
    settings_t s = fresh();
    const uint8_t before = 5;
    uint32_t t = run(&s, false, true, 2200, 1000);
    run(&s, false, false, 200, t);
    CHECK(s.step[SETTINGS_PAGE_VOLUME] == before,
          "the release that ended the opening hold changed volume to %u",
          s.step[SETTINGS_PAGE_VOLUME]);
}

static void test_a_tap_walks_the_pages_and_wraps(void) {
    settings_t s = fresh();
    uint32_t t = run(&s, false, true, 2200, 1000);
    t = run(&s, false, false, 200, t);
    for (uint8_t expect = 1; expect < SETTINGS_PAGES; expect++) {
        t = run(&s, true, false, 120, t);
        t = run(&s, false, false, 120, t);
        CHECK(s.page == expect, "after %u taps page is %u", expect, s.page);
    }
    t = run(&s, true, false, 120, t);
    run(&s, false, false, 120, t);
    CHECK(s.page == SETTINGS_PAGE_VOLUME, "page did not wrap, it is %u", s.page);
}

static void test_b_tap_walks_the_values_and_wraps(void) {
    settings_t s = fresh();
    uint32_t t = run(&s, false, true, 2200, 1000);
    t = run(&s, false, false, 200, t);
    t = run(&s, false, true, 120, t);
    t = run(&s, false, false, 120, t);
    CHECK(s.step[SETTINGS_PAGE_VOLUME] == 0, "5 + 1 did not wrap to 0, it is %u",
          s.step[SETTINGS_PAGE_VOLUME]);
    t = run(&s, false, true, 120, t);
    run(&s, false, false, 120, t);
    CHECK(s.step[SETTINGS_PAGE_VOLUME] == 1, "second tap gave %u",
          s.step[SETTINGS_PAGE_VOLUME]);
}

static void test_holding_a_leaves_and_asks_for_a_save(void) {
    settings_t s = fresh();
    uint32_t t = run(&s, false, true, 2200, 1000);
    t = run(&s, false, false, 200, t);
    t = run(&s, true, false, 1200, t);
    CHECK(!s.open, "a 1.2 s A hold did not close the menu");
    CHECK(s.save_requested, "closing did not ask for a save");
    run(&s, false, false, 200, t);
    CHECK(s.page == SETTINGS_PAGE_VOLUME, "the closing hold also walked to page %u", s.page);
}

static void test_holding_b_on_the_wifi_page_requests_setup(void) {
    settings_t s = fresh();
    uint32_t t = run(&s, false, true, 2200, 1000);
    t = run(&s, false, false, 200, t);
    for (int i = 0; i < SETTINGS_PAGE_WIFI; i++) {
        t = run(&s, true, false, 120, t);
        t = run(&s, false, false, 120, t);
    }
    CHECK(s.page == SETTINGS_PAGE_WIFI, "did not reach the wifi page, on %u", s.page);
    run(&s, false, true, 1200, t);
    CHECK(s.wifi_requested, "a 1.2 s B hold on the wifi page requested nothing");
}

static void test_a_b_tap_on_the_wifi_page_changes_nothing(void) {
    settings_t s = fresh();
    uint32_t t = run(&s, false, true, 2200, 1000);
    t = run(&s, false, false, 200, t);
    for (int i = 0; i < SETTINGS_PAGE_WIFI; i++) {
        t = run(&s, true, false, 120, t);
        t = run(&s, false, false, 120, t);
    }
    t = run(&s, false, true, 120, t);
    run(&s, false, false, 120, t);
    CHECK(!s.wifi_requested, "a tap started setup; only a hold may");
    CHECK(s.open, "a tap on the wifi page closed the menu");
}

static void test_a_long_b_hold_on_a_value_page_is_not_a_tap(void) {
    settings_t s = fresh();
    uint32_t t = run(&s, false, true, 2200, 1000);
    t = run(&s, false, false, 200, t);
    const uint8_t before = s.step[SETTINGS_PAGE_VOLUME];
    t = run(&s, false, true, 1500, t);
    run(&s, false, false, 200, t);
    CHECK(s.step[SETTINGS_PAGE_VOLUME] == before,
          "a 1.5 s hold acted as a tap, volume went %u -> %u", before,
          s.step[SETTINGS_PAGE_VOLUME]);
}

static void test_hold_progress_appears_late_and_only_where_it_means_something(void) {
    settings_t s = fresh();
    uint32_t t = run(&s, false, true, 600, 1000);  // 30% of 2000 ms
    CHECK(s.hold_pct == 0, "progress showed at 30%%: %u", s.hold_pct);
    t = run(&s, false, true, 800, t);              // now past 40%
    CHECK(s.hold_pct > 0, "progress still hidden at 70%%");
    t = run(&s, false, true, 900, t);
    t = run(&s, false, false, 200, t);
    CHECK(s.open, "setup for the next part failed: menu not open");
    CHECK(s.hold_pct == 0, "progress outlived the gesture");

    t = run(&s, false, true, 900, t);  // B hold on the volume page: does nothing
    CHECK(s.hold_pct == 0, "a hold that will do nothing drew a progress bar");
}

static void test_twenty_seconds_of_silence_closes_and_saves(void) {
    settings_t s = fresh();
    uint32_t t = run(&s, false, true, 2200, 1000);
    t = run(&s, false, false, 200, t);
    s.save_requested = false;
    t = run(&s, false, false, SETTINGS_IDLE_MS - 2000, t);
    CHECK(s.open, "closed early, before the timeout");
    run(&s, false, false, 3000, t);
    CHECK(!s.open, "the idle timeout did not close the menu");
    CHECK(s.save_requested, "the idle timeout did not ask for a save");
}

static void test_a_press_postpones_the_timeout(void) {
    settings_t s = fresh();
    uint32_t t = run(&s, false, true, 2200, 1000);
    t = run(&s, false, false, 200, t);
    t = run(&s, false, false, SETTINGS_IDLE_MS - 2000, t);
    t = run(&s, true, false, 120, t);
    t = run(&s, false, false, 120, t);
    run(&s, false, false, SETTINGS_IDLE_MS - 2000, t);
    CHECK(s.open, "a press did not postpone the timeout");
}

static void test_a_closed_menu_ignores_button_a(void) {
    settings_t s = fresh();
    uint32_t t = run(&s, true, false, 5000, 1000);
    run(&s, false, false, 200, t);
    CHECK(!s.open, "holding A opened the menu; that is push-to-talk");
    CHECK(!s.save_requested, "holding A asked for a save");
}

static void test_the_gain_table_is_monotonic_and_ends_exactly(void) {
    CHECK(settings_volume_gain(0) == 0, "step 0 is not silent: %d",
          (int)settings_volume_gain(0));
    CHECK(settings_volume_gain(SETTINGS_VOLUME_STEPS - 1) == 32767,
          "the top step is not unity: %d",
          (int)settings_volume_gain(SETTINGS_VOLUME_STEPS - 1));
    for (uint8_t i = 1; i < SETTINGS_VOLUME_STEPS; i++) {
        CHECK(settings_volume_gain(i) > settings_volume_gain(i - 1),
              "gain fell from step %u to %u", i - 1, i);
    }
    CHECK(settings_volume_gain(200) == 32767, "an out-of-range step is not clamped loud");
}

static void test_the_contrast_table_is_monotonic_and_tops_out_where_init_does(void) {
    CHECK(settings_screen_contrast(SETTINGS_SCREEN_STEPS - 1) == 0xcf,
          "the top step is not the init value: 0x%02x",
          settings_screen_contrast(SETTINGS_SCREEN_STEPS - 1));
    for (uint8_t i = 1; i < SETTINGS_SCREEN_STEPS; i++) {
        CHECK(settings_screen_contrast(i) > settings_screen_contrast(i - 1),
              "contrast fell from step %u to %u", i - 1, i);
    }
}

static void test_every_label_is_short_ascii_and_distinct(void) {
    settings_t s = fresh();
    for (uint8_t page = 0; page < SETTINGS_PAGES; page++) {
        const char *title = settings_page_title(page);
        CHECK(strlen(title) <= 12, "title \"%s\" is too long for the line", title);
        const uint8_t n = settings_page_steps(page);
        for (uint8_t i = 0; i < n; i++) {
            s.step[page] = i;
            const char *v = settings_value_text(&s, page);
            CHECK(strlen(v) > 0 && strlen(v) <= 12, "value \"%s\" will not fit", v);
            for (const char *p = v; *p; p++) {
                CHECK(*p >= 0x20 && *p <= 0x7e, "value \"%s\" leaves the font", v);
            }
            for (uint8_t j = 0; j < i; j++) {
                s.step[page] = j;
                CHECK(strcmp(v, settings_value_text(&s, page)) != 0,
                      "steps %u and %u of page %u read the same", i, j, page);
                s.step[page] = i;
            }
        }
        // WIFI has no step slot at all (n == 0, so the loop above never ran
        // for it) - resetting here would index step[SETTINGS_PAGES - 1],
        // one past the array UBSan rightly flags as out of bounds.
        if (n > 0) s.step[page] = 0;
    }
}

static void test_a_muted_step_asks_for_no_beep(void) {
    settings_t s = fresh();
    uint32_t t = run(&s, false, true, 2200, 1000);
    t = run(&s, false, false, 200, t);
    s.beep_requested = false;
    t = run(&s, false, true, 120, t);  // 5 -> 0, muted
    t = run(&s, false, false, 120, t);
    CHECK(s.step[SETTINGS_PAGE_VOLUME] == 0, "setup failed, volume is %u",
          s.step[SETTINGS_PAGE_VOLUME]);
    CHECK(!s.beep_requested, "muted asked for a tone to demonstrate silence");
    t = run(&s, false, true, 120, t);  // 0 -> 1
    run(&s, false, false, 120, t);
    CHECK(s.beep_requested, "an audible step asked for no tone");
}

static void test_init_clamps_nonsense_from_nvs(void) {
    settings_t s;
    settings_init(&s, 99, 99, 99);
    CHECK(s.step[SETTINGS_PAGE_VOLUME] == 0, "volume %u", s.step[SETTINGS_PAGE_VOLUME]);
    CHECK(s.step[SETTINGS_PAGE_SCREEN] == 0, "screen %u", s.step[SETTINGS_PAGE_SCREEN]);
    CHECK(s.step[SETTINGS_PAGE_EYES] == 0, "eyes %u", s.step[SETTINGS_PAGE_EYES]);
    CHECK(!s.open, "init opened the menu");
}

int main(void) {
    test_a_short_b_hold_does_not_open();
    test_a_two_second_b_hold_opens_on_the_first_page();
    test_the_opening_release_is_not_a_tap();
    test_a_tap_walks_the_pages_and_wraps();
    test_b_tap_walks_the_values_and_wraps();
    test_holding_a_leaves_and_asks_for_a_save();
    test_holding_b_on_the_wifi_page_requests_setup();
    test_a_b_tap_on_the_wifi_page_changes_nothing();
    test_a_long_b_hold_on_a_value_page_is_not_a_tap();
    test_hold_progress_appears_late_and_only_where_it_means_something();
    test_twenty_seconds_of_silence_closes_and_saves();
    test_a_press_postpones_the_timeout();
    test_a_closed_menu_ignores_button_a();
    test_the_gain_table_is_monotonic_and_ends_exactly();
    test_the_contrast_table_is_monotonic_and_tops_out_where_init_does();
    test_every_label_is_short_ascii_and_distinct();
    test_a_muted_step_asks_for_no_beep();
    test_init_clamps_nonsense_from_nvs();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
