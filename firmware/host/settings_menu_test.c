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

// A hold on a button bound to nothing where the menu currently is - B on a
// value page - produces no edge for as long as it lasts, so it was possible
// for the idle timeout to close the menu out from under a hold that never
// released, and then reopen it one tick later because the same stale press
// duration immediately re-satisfied the open threshold. This drives such a
// hold straight through the idle window and well past it, watching for
// exactly one close and no unexplained reopen.
static void test_a_dead_hold_does_not_reopen_the_menu_after_the_idle_timeout(void) {
    settings_t s = fresh();
    uint32_t t = run(&s, false, true, 2200, 1000);  // open
    t = run(&s, false, false, 200, t);              // release, page = VOLUME
    const uint8_t before = s.step[SETTINGS_PAGE_VOLUME];

    int closes = 0;
    int opens = 0;
    bool was_open = s.open;
    for (uint32_t elapsed = 0; elapsed < SETTINGS_IDLE_MS * 2; elapsed += 40) {
        t += 40;
        settings_tick(&s, false, true, t);  // B held throughout, never released
        if (was_open && !s.open) closes++;
        if (!was_open && s.open) opens++;
        was_open = s.open;
    }
    CHECK(closes == 1, "the menu closed %d times during one continuous hold", closes);
    CHECK(opens == 0, "the menu reopened on its own during the same held press");
    CHECK(!s.open, "the menu ended up open under a button that never released");

    run(&s, false, false, 200, t);  // finally release B
    CHECK(s.step[SETTINGS_PAGE_VOLUME] == before,
          "a dead hold through the timeout changed volume to %u",
          s.step[SETTINGS_PAGE_VOLUME]);
}

// hold_target() answers differently depending on s->open, and A's ordinary
// exit-hold and B's stale idle rest can both be "still held" in the very
// same tick. If A's hold flips the menu closed first, B's already-long
// duration must not be free to immediately reopen it under the new
// (closed) meaning of a B hold - in the very tick that closed it.
static void test_closing_while_the_other_button_rests_does_not_reopen(void) {
    settings_t s = fresh();
    uint32_t t = run(&s, false, true, 2200, 1000);  // open, page = VOLUME
    t = run(&s, false, false, 200, t);
    t = run(&s, true, false, 120, t);  // tap A once, off the volume page
    t = run(&s, false, false, 120, t);
    CHECK(s.page == SETTINGS_PAGE_SCREEN, "setup failed: on page %u", s.page);
    const uint8_t before = s.step[SETTINGS_PAGE_SCREEN];

    t = run(&s, false, true, 3000, t);  // B settles into an idle rest here
    CHECK(s.open, "setup failed: menu closed on its own while B merely rested");

    t = run(&s, true, true, 1200, t);  // an ordinary A exit-hold, B down throughout
    CHECK(!s.open, "A's exit-hold did not close the menu with B resting");
    CHECK(s.save_requested, "closing did not ask for a save");

    t = run(&s, false, true, 500, t);  // A let go; B is still down and must stay inert
    CHECK(!s.open, "the menu reopened under B's stale rest the instant A closed it");
    CHECK(s.page == SETTINGS_PAGE_SCREEN,
          "the page jumped to %u, as it would if the close reopened the menu", s.page);

    run(&s, false, false, 200, t);  // finally release B
    CHECK(s.step[SETTINGS_PAGE_SCREEN] == before,
          "B's stale rest changed the screen setting to %u", s.step[SETTINGS_PAGE_SCREEN]);
}

// The mirror case: A already resting - as if mid push-to-talk - when B's
// hold opens the menu. A's own iteration that tick still saw open false and
// fired nothing, but its held duration is already stale by the time open
// changes; without spending it there too, the very next tick finds A's
// target now SETTINGS_EXIT_MS and its ancient held time satisfies it at
// once, closing the menu as fast as it opened.
static void test_opening_while_push_to_talk_is_held_stays_open(void) {
    settings_t s = fresh();
    uint32_t t = run(&s, true, false, 3000, 1000);  // A held, as if asking a long question
    CHECK(!s.open, "setup failed: holding A alone opened the menu");

    t = run(&s, true, true, 2200, t);  // B's full open-hold, A still down throughout
    CHECK(s.open, "B's hold did not open the menu with A already resting");

    t = run(&s, true, true, 1000, t);  // a further second - A's stale hold must not fire
    CHECK(s.open, "the menu closed on its own under A's stale push-to-talk hold");

    run(&s, false, true, 200, t);  // finally release A
    CHECK(s.open, "releasing A closed the menu; it should take a fresh exit-hold");
}

// The two cases above are only safe to fix by spending a button outright
// because in both of them the held duration genuinely already exceeds the
// newly applicable target. A button that has barely started - or starts on
// the very tick of the flip - must not pay the same price: it gets a clean
// restart of its own gesture instead. This drives B's open-gesture to the
// exact tick it fires, with A's first-ever press landing on that identical
// tick, and checks that A still gets to run an ordinary exit-hold from
// there.
static void test_a_fresh_press_at_the_flip_is_honoured(void) {
    settings_t s = fresh();
    // One tick short of B's own threshold, so the very next tick both fires
    // the open and is A's first press - the same settings_tick() call sees
    // both events.
    uint32_t t = run(&s, false, true, SETTINGS_OPEN_MS, 1000);
    CHECK(!s.open, "setup failed: B already opened the menu");

    t = run(&s, true, true, 40, t);  // this tick: B opens, A presses for the first time
    CHECK(s.open, "setup failed: B's hold did not open the menu");

    // A's press has zero held time as of the flip - a full, ordinary
    // exit-hold from here should complete normally.
    run(&s, true, true, SETTINGS_EXIT_MS + 200, t);
    CHECK(!s.open, "A's fresh press at the flip was spent instead of honoured");
    CHECK(s.save_requested, "A's exit-hold closed the menu without asking for a save");
}

// The other half of the same rule: a press that had *some* time on the
// clock before the flip, but not enough to already meet the new target,
// must not carry that time over. It needs a full SETTINGS_EXIT_MS measured
// from the flip, not "whatever is left" after subtracting what it had
// already banked. Finds the flip tick itself, since hand-computing it to
// the millisecond is exactly the kind of arithmetic this test exists to not
// have to trust.
static void test_a_partway_press_does_not_borrow_pre_flip_time(void) {
    settings_t s = fresh();
    uint32_t t = run(&s, false, true, SETTINGS_OPEN_MS - 700, 1000);  // B most of the way there
    CHECK(!s.open, "setup failed: the menu opened before A joined");

    t = run(&s, true, true, 40, t);  // A presses down here, roughly 700 ms before the flip
    CHECK(!s.open, "setup failed: the menu opened as soon as A joined");

    uint32_t flip_at = 0;
    for (int i = 0; i < 60 && !s.open; i++) {
        t += 40;
        settings_tick(&s, true, true, t);
        if (s.open) flip_at = t;
    }
    CHECK(flip_at != 0, "setup failed: B's hold never opened the menu");

    while (t < flip_at + 300) {  // a naive "target minus pre-flip credit" would be enough by now
        t += 40;
        settings_tick(&s, true, true, t);
    }
    CHECK(s.open, "A's exit-hold fired early, borrowing time from before the flip");

    while (t < flip_at + SETTINGS_EXIT_MS + 200) {  // a full exit-hold measured from the flip
        t += 40;
        settings_tick(&s, true, true, t);
    }
    CHECK(!s.open, "A never closed the menu after a full exit-hold measured from the flip");
}

// set_open()'s restart lets a press that predates the flip still finish an
// honest hold - but the release path's tap check reads that same restarted
// clock, with no memory of how long the button was down before the flip.
// A press held for about half a second while the menu was closed and
// released shortly after B's hold opens it must not read as a deliberate
// quick tap.
static void test_a_press_that_predates_an_opening_flip_does_not_tap(void) {
    settings_t s = fresh();
    uint32_t t = run(&s, false, true, SETTINGS_OPEN_MS - 500, 1000);  // B most of the way there
    CHECK(!s.open, "setup failed: the menu opened before A joined");

    t = run(&s, true, true, 40, t);  // A presses down here, roughly 500 ms before the flip
    CHECK(!s.open, "setup failed: the menu opened as soon as A joined");

    uint32_t flip_at = 0;
    for (int i = 0; i < 60 && !s.open; i++) {
        t += 40;
        settings_tick(&s, true, true, t);
        if (s.open) flip_at = t;
    }
    CHECK(flip_at != 0, "setup failed: B's hold never opened the menu");

    const uint8_t page_before = s.page;
    const uint8_t vol_before = s.step[SETTINGS_PAGE_VOLUME];

    t = run(&s, false, false, 120, t);  // release A well inside the tap window
    CHECK(s.page == page_before,
          "a press that predates the flip advanced the page to %u", s.page);
    CHECK(s.step[SETTINGS_PAGE_VOLUME] == vol_before,
          "a press that predates the flip changed a value");

    // The bar is on this press, not on A itself: a clean press afterward
    // must tap normally.
    t = run(&s, true, false, 120, t);
    run(&s, false, false, 120, t);
    CHECK(s.page == (uint8_t)((page_before + 1) % SETTINGS_PAGES),
          "A stopped tapping after one press was barred from tapping");
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

// settings_value_text() is the one accessor handed a caller-owned struct, so
// it is the one that can be given a step no gesture could produce. Nothing in
// settings_menu.c writes such a step - this is what a future caller reaching
// into step[] the way this test does would get, and the point of the check is
// that removing the guard turns it from a clamp into a read past the table,
// which the ASan build would then catch here rather than on a board.
static void test_an_out_of_range_step_clamps_instead_of_reading_past_a_table(void) {
    settings_t s = fresh();
    const uint8_t pages[3] = {SETTINGS_PAGE_VOLUME, SETTINGS_PAGE_SCREEN, SETTINGS_PAGE_EYES};
    const uint8_t nonsense[3] = {SETTINGS_VOLUME_STEPS, 100, 255};

    for (size_t p = 0; p < 3; p++) {
        const uint8_t page = pages[p];
        const uint8_t n = settings_page_steps(page);
        for (size_t k = 0; k < 3; k++) {
            s.step[page] = nonsense[k];
            const char *v = settings_value_text(&s, page);
            CHECK(strlen(v) > 0 && strlen(v) <= 12,
                  "page %u step %u gave \"%s\"", page, nonsense[k], v);

            // It must be one of that page's own labels, which is what says it
            // clamped rather than landing on whatever followed the table.
            bool known = false;
            for (uint8_t i = 0; i < n; i++) {
                s.step[page] = i;
                if (strcmp(v, settings_value_text(&s, page)) == 0) known = true;
            }
            CHECK(known, "page %u step %u gave \"%s\", which is not one of its labels",
                  page, nonsense[k], v);
        }
        s.step[page] = 0;
    }

    // And it clamps where the value does, so what the panel prints is the
    // setting actually in force rather than some other label off the table.
    s.step[SETTINGS_PAGE_VOLUME] = 200;
    const char *loud = settings_value_text(&s, SETTINGS_PAGE_VOLUME);
    s.step[SETTINGS_PAGE_VOLUME] = SETTINGS_VOLUME_STEPS - 1;
    CHECK(strcmp(loud, settings_value_text(&s, SETTINGS_PAGE_VOLUME)) == 0,
          "an out-of-range volume read as \"%s\", not as the top step whose gain it gets", loud);

    s.step[SETTINGS_PAGE_SCREEN] = 200;
    const char *bright = settings_value_text(&s, SETTINGS_PAGE_SCREEN);
    s.step[SETTINGS_PAGE_SCREEN] = SETTINGS_SCREEN_STEPS - 1;
    CHECK(strcmp(bright, settings_value_text(&s, SETTINGS_PAGE_SCREEN)) == 0,
          "an out-of-range brightness read as \"%s\", not as the top step whose contrast it gets",
          bright);

    s.step[SETTINGS_PAGE_EYES] = 200;
    const char *pose = settings_value_text(&s, SETTINGS_PAGE_EYES);
    s.step[SETTINGS_PAGE_EYES] = 0;
    CHECK(strcmp(pose, settings_value_text(&s, SETTINGS_PAGE_EYES)) == 0,
          "an out-of-range eyes step read as \"%s\", not as the first, whose pose it gets", pose);
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
    test_a_dead_hold_does_not_reopen_the_menu_after_the_idle_timeout();
    test_closing_while_the_other_button_rests_does_not_reopen();
    test_opening_while_push_to_talk_is_held_stays_open();
    test_a_fresh_press_at_the_flip_is_honoured();
    test_a_partway_press_does_not_borrow_pre_flip_time();
    test_a_press_that_predates_an_opening_flip_does_not_tap();
    test_the_gain_table_is_monotonic_and_ends_exactly();
    test_the_contrast_table_is_monotonic_and_tops_out_where_init_does();
    test_every_label_is_short_ascii_and_distinct();
    test_a_muted_step_asks_for_no_beep();
    test_init_clamps_nonsense_from_nvs();
    test_an_out_of_range_step_clamps_instead_of_reading_past_a_table();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
