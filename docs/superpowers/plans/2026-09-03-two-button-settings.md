# Two-Button Settings Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the device a settings carousel on its own panel — volume, screen brightness, the face's resting expression, and the way into WiFi setup — reached by holding the new button on GPIO 20.

**Architecture:** Two new plain-C modules that build and test on the laptop, the way `provision_logic.c` and `setup_screen.c` already do: `settings_menu.c` holds the gestures, the values and the lookup tables; `settings_screen.c` renders a `settings_t` into a framebuffer it is handed. `voice_main.c` gains a second debounced pin, a third branch in `face_task`, and a volume multiply in `audio_out_task`. The five-tap gesture and `boot_gesture_task` are deleted.

**Tech Stack:** C11, ESP-IDF v5 on an ESP32-C3, FreeRTOS tasks, NVS for storage, SSD1306 over I²C. Host tests are plain `cc` plus ASan/UBSan through `firmware/host/Makefile`.

**Spec:** `docs/superpowers/specs/2026-09-03-two-button-settings-design.md`

## Global Constraints

- `settings_menu.c` and `settings_screen.c` take **no ESP-IDF header, no float, no allocation** — the same rule `face.c` and `setup_screen.c` follow, and the reason `firmware/host` can build them.
- Host builds run `-std=c11 -O2 -Wall -Wextra -Werror`, and again under `-fsanitize=address,undefined`. Both must be clean.
- Panel text is **ASCII `0x20`-`0x7e` only**, 21 characters per line, 128×64 pixels, 5×7 glyphs at a 6-pixel advance.
- Gesture thresholds: open **2000 ms**, exit **1000 ms**, action **1000 ms**, idle timeout **20000 ms**, hold progress visible past **40%** of the threshold.
- Volume gains, Q15: `{0, 2068, 4125, 8231, 16422, 32767}`.
- Screen contrast register values: `{0x10, 0x40, 0x80, 0xCF}`.
- Eyes map to `{FACE_EMO_NEUTRAL, FACE_EMO_HAPPY, FACE_EMO_CURIOUS, FACE_EMO_EXCITED}`.
- NVS keys store the **step index**, not the value: `vol` 0-5 default 5, `bright` 0-3 default 3, `eyes` 0-3 default 0.
- Every task ends on a green `cd firmware/host && make test` where host code changed, and on a clean `idf.py -DSKETCH=voice build` where firmware code changed.

---

### Task 1: `settings_menu` — gestures, values and tables

**Files:**
- Create: `firmware/main/settings_menu.h`
- Create: `firmware/main/settings_menu.c`
- Create: `firmware/host/settings_menu_test.c`
- Modify: `firmware/host/Makefile`

**Interfaces:**
- Consumes: `face_emotion_t` from `firmware/main/face.h` (a pure header — it pulls in no ESP-IDF).
- Produces: `settings_t`, `settings_init()`, `settings_tick()`, `settings_volume_gain()`, `settings_screen_contrast()`, `settings_eyes_emotion()`, `settings_page_title()`, `settings_value_text()`, and the `SETTINGS_*` constants. Tasks 2, 4, 5, 6 and 7 all rely on these exact names.

- [ ] **Step 1: Write the header**

Create `firmware/main/settings_menu.h`:

```c
// The settings carousel: which page is showing, what each value is, and what
// the two buttons have just done about it.
//
// Same rules as face.c and setup_screen.c - no ESP-IDF header, no float, no
// allocation - so firmware/host can build it and every gesture can be tested
// without a board. It is told the debounced buttons and a millisecond clock;
// it knows nothing about GPIO, NVS or I2C.
//
// The design and the reasoning behind every constant here:
// docs/superpowers/specs/2026-09-03-two-button-settings-design.md

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "face.h"  // face_emotion_t only

#define SETTINGS_PAGES 4

enum {
    SETTINGS_PAGE_VOLUME = 0,
    SETTINGS_PAGE_SCREEN,
    SETTINGS_PAGE_EYES,
    SETTINGS_PAGE_WIFI,
};

#define SETTINGS_VOLUME_STEPS 6
#define SETTINGS_SCREEN_STEPS 4
#define SETTINGS_EYES_STEPS 4

// The gestures, in milliseconds.
//
// Entry is longer than exit on purpose: entry has to survive a pocket and a
// curious hand, exit is asked for by someone already looking at the screen.
#define SETTINGS_OPEN_MS 2000
#define SETTINGS_EXIT_MS 1000
#define SETTINGS_ACTION_MS 1000
#define SETTINGS_IDLE_MS 20000

// Below this share of a hold, no progress is reported. A fraction rather than
// a fixed number of milliseconds, so the 2 s entry and the 1 s exit warn for
// the same part of the gesture.
#define SETTINGS_WARN_PCT 40

typedef struct {
    bool open;
    uint8_t page;
    uint8_t step[3];  // volume, screen, eyes - indexed by page

    // -- what the owner must act on, each latched until the owner clears it
    bool wifi_requested;
    bool beep_requested;
    bool save_requested;

    // -- how far through a hold that will do something, 0-100
    uint8_t hold_pct;

    // -- private
    bool a_was, b_was;
    bool a_used, b_used;  // this press already fired as a hold
    uint32_t a_at, b_at;
    uint32_t last_input;
} settings_t;

// Seeded with what came out of NVS. Out-of-range steps clamp to 0.
void settings_init(settings_t *s, uint8_t volume, uint8_t screen, uint8_t eyes);

// Fed the two debounced buttons every tick. Returns true when anything the
// owner can see has changed - the page, a value, a flag or hold_pct - so a
// caller can skip redrawing an unchanged screen.
bool settings_tick(settings_t *s, bool a_down, bool b_down, uint32_t now_ms);

// Q15 playback gain. Step 0 is exactly 0, the top step exactly 0x7fff.
int32_t settings_volume_gain(uint8_t step);

// The SSD1306 contrast register value for a brightness step.
uint8_t settings_screen_contrast(uint8_t step);

// Which pose the face rests in for an eyes step.
face_emotion_t settings_eyes_emotion(uint8_t step);

// Panel strings. Both are static, ASCII, and short enough for one line.
const char *settings_page_title(uint8_t page);
const char *settings_value_text(const settings_t *s, uint8_t page);

// How many steps a page has, or 0 for a page that holds no value.
uint8_t settings_page_steps(uint8_t page);
```

- [ ] **Step 2: Write the failing gesture test**

Create `firmware/host/settings_menu_test.c`:

```c
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

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
```

- [ ] **Step 3: Wire it into the host Makefile**

In `firmware/host/Makefile`, add a plain-build rule after the `setup_screen_test` rule:

```make
$(BUILD)/settings_menu_test: ../main/settings_menu.c ../main/settings_menu.h ../main/face.c settings_menu_test.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ ../main/settings_menu.c ../main/face.c settings_menu_test.c
```

and the sanitized twin after the `setup_screen_test` ASan rule:

```make
$(ASAN_BUILD)/settings_menu_test: ../main/settings_menu.c ../main/settings_menu.h ../main/face.c settings_menu_test.c | $(ASAN_BUILD)
	$(CC) $(ASAN_CFLAGS) -o $@ ../main/settings_menu.c ../main/face.c settings_menu_test.c
```

Then extend both runner targets. `test:` becomes:

```make
test: $(BUILD)/face_test $(BUILD)/provision_logic_test $(BUILD)/setup_screen_test $(BUILD)/settings_menu_test
	@./$(BUILD)/face_test
	@./$(BUILD)/provision_logic_test
	@./$(BUILD)/setup_screen_test
	@./$(BUILD)/settings_menu_test
```

and `asan:` becomes:

```make
asan: $(ASAN_BUILD)/face_test $(ASAN_BUILD)/provision_logic_test $(ASAN_BUILD)/setup_screen_test $(ASAN_BUILD)/settings_menu_test
	@./$(ASAN_BUILD)/face_test
	@./$(ASAN_BUILD)/provision_logic_test
	@./$(ASAN_BUILD)/setup_screen_test
	@./$(ASAN_BUILD)/settings_menu_test
```

`face.c` is linked in because `settings_menu.h` includes `face.h`; the test never calls into it, but `settings_eyes_emotion()` returns its enum and later tasks will use the poses.

- [ ] **Step 4: Run the tests and watch them fail**

```bash
cd firmware/host && make test
```

Expected: the compile fails with `fatal error: ../main/settings_menu.c: No such file or directory` — the header exists, the implementation does not.

- [ ] **Step 5: Write the implementation**

Create `firmware/main/settings_menu.c`:

```c
#include "settings_menu.h"

#include <string.h>

// The value tables. Steps are stored, values are looked up, so a table can be
// retuned later without migrating what is already in NVS.

// Logarithmic. A linear scale spends its first three steps below the level
// where anything sounds different.
static const int32_t VOLUME_GAIN[SETTINGS_VOLUME_STEPS] = {
    0, 2068, 4125, 8231, 16422, 32767,  // muted, -24, -18, -12, -6, 0 dB
};

static const uint8_t SCREEN_CONTRAST[SETTINGS_SCREEN_STEPS] = {
    0x10, 0x40, 0x80, 0xcf,  // the top is what ssd1306.c writes at init
};

static const uint8_t EYES_EMOTION[SETTINGS_EYES_STEPS] = {
    FACE_EMO_NEUTRAL, FACE_EMO_HAPPY, FACE_EMO_CURIOUS, FACE_EMO_EXCITED,
};

static const char *const VOLUME_TEXT[SETTINGS_VOLUME_STEPS] = {
    "muted", "-24 dB", "-18 dB", "-12 dB", "-6 dB", "0 dB",
};

static const char *const SCREEN_TEXT[SETTINGS_SCREEN_STEPS] = {
    "Low", "Mid", "High", "Max",
};

static const char *const EYES_TEXT[SETTINGS_EYES_STEPS] = {
    "Calm", "Happy", "Curious", "Excited",
};

static const char *const PAGE_TITLE[SETTINGS_PAGES] = {
    "VOLUME", "SCREEN", "EYES", "WIFI",
};

static const uint8_t PAGE_STEPS[SETTINGS_PAGES] = {
    SETTINGS_VOLUME_STEPS, SETTINGS_SCREEN_STEPS, SETTINGS_EYES_STEPS, 0,
};

uint8_t settings_page_steps(uint8_t page) {
    return (page < SETTINGS_PAGES) ? PAGE_STEPS[page] : 0;
}

int32_t settings_volume_gain(uint8_t step) {
    return (step < SETTINGS_VOLUME_STEPS) ? VOLUME_GAIN[step]
                                          : VOLUME_GAIN[SETTINGS_VOLUME_STEPS - 1];
}

uint8_t settings_screen_contrast(uint8_t step) {
    return (step < SETTINGS_SCREEN_STEPS) ? SCREEN_CONTRAST[step]
                                          : SCREEN_CONTRAST[SETTINGS_SCREEN_STEPS - 1];
}

face_emotion_t settings_eyes_emotion(uint8_t step) {
    return (face_emotion_t)((step < SETTINGS_EYES_STEPS) ? EYES_EMOTION[step]
                                                         : EYES_EMOTION[0]);
}

const char *settings_page_title(uint8_t page) {
    return (page < SETTINGS_PAGES) ? PAGE_TITLE[page] : "";
}

const char *settings_value_text(const settings_t *s, uint8_t page) {
    switch (page) {
        case SETTINGS_PAGE_VOLUME: return VOLUME_TEXT[s->step[SETTINGS_PAGE_VOLUME]];
        case SETTINGS_PAGE_SCREEN: return SCREEN_TEXT[s->step[SETTINGS_PAGE_SCREEN]];
        case SETTINGS_PAGE_EYES:   return EYES_TEXT[s->step[SETTINGS_PAGE_EYES]];
        default:                   return "";
    }
}

static uint8_t clamp(uint8_t v, uint8_t n) { return (v < n) ? v : 0; }

void settings_init(settings_t *s, uint8_t volume, uint8_t screen, uint8_t eyes) {
    memset(s, 0, sizeof(*s));
    s->step[SETTINGS_PAGE_VOLUME] = clamp(volume, SETTINGS_VOLUME_STEPS);
    s->step[SETTINGS_PAGE_SCREEN] = clamp(screen, SETTINGS_SCREEN_STEPS);
    s->step[SETTINGS_PAGE_EYES] = clamp(eyes, SETTINGS_EYES_STEPS);
}

// How long a hold on this button would have to last to do something, or 0 if
// nothing is bound to it here. This is what decides both when a hold fires and
// whether a progress bar is honest to draw.
static uint32_t hold_target(const settings_t *s, bool is_a) {
    if (!s->open) return is_a ? 0 : SETTINGS_OPEN_MS;   // only B opens
    if (is_a) return SETTINGS_EXIT_MS;
    return (s->page == SETTINGS_PAGE_WIFI) ? SETTINGS_ACTION_MS : 0;
}

// A press that lasted less than what a hold needs, on a button that has a
// hold. A button with no hold bound - B on a value page - taps on any
// release short of the exit threshold, which is the longest any tap can be.
static bool was_a_tap(uint32_t held, uint32_t target) {
    return held < (target ? target : SETTINGS_EXIT_MS);
}

static void bump_value(settings_t *s) {
    const uint8_t n = settings_page_steps(s->page);
    if (n == 0) return;
    s->step[s->page] = (uint8_t)((s->step[s->page] + 1) % n);
    if (s->page == SETTINGS_PAGE_VOLUME && s->step[s->page] != 0) {
        s->beep_requested = true;  // nothing to demonstrate at muted
    }
}

static void close_and_save(settings_t *s) {
    s->open = false;
    s->save_requested = true;
    s->hold_pct = 0;
}

bool settings_tick(settings_t *s, bool a_down, bool b_down, uint32_t now_ms) {
    const settings_t before = *s;

    const bool edges[2] = {a_down, b_down};
    bool *was[2] = {&s->a_was, &s->b_was};
    bool *used[2] = {&s->a_used, &s->b_used};
    uint32_t *at[2] = {&s->a_at, &s->b_at};

    for (int i = 0; i < 2; i++) {
        const bool is_a = (i == 0);
        const uint32_t target = hold_target(s, is_a);

        if (edges[i] && !*was[i]) {          // press
            *at[i] = now_ms;
            *used[i] = false;
            s->last_input = now_ms;
        } else if (edges[i] && *was[i]) {    // still held
            const uint32_t held = now_ms - *at[i];
            if (target && !*used[i] && held >= target) {
                *used[i] = true;
                s->hold_pct = 0;
                s->last_input = now_ms;
                if (!s->open) {
                    s->open = true;
                    s->page = SETTINGS_PAGE_VOLUME;
                } else if (is_a) {
                    close_and_save(s);
                } else {
                    s->wifi_requested = true;
                }
            }
        } else if (!edges[i] && *was[i]) {   // release
            const uint32_t held = now_ms - *at[i];
            if (!*used[i] && s->open && was_a_tap(held, target)) {
                if (is_a) {
                    s->page = (uint8_t)((s->page + 1) % SETTINGS_PAGES);
                } else {
                    bump_value(s);
                }
            }
            *used[i] = false;
            s->last_input = now_ms;
        }
        *was[i] = edges[i];
    }

    // The bar, for whichever hold is running and will actually do something.
    uint8_t pct = 0;
    for (int i = 0; i < 2; i++) {
        if (!edges[i] || *used[i]) continue;
        const uint32_t target = hold_target(s, i == 0);
        if (target == 0) continue;
        const uint32_t held = now_ms - *at[i];
        const uint32_t share = (held * 100u) / target;
        if (share >= SETTINGS_WARN_PCT) pct = (uint8_t)(share > 100 ? 100 : share);
    }
    s->hold_pct = pct;

    if (s->open && (uint32_t)(now_ms - s->last_input) > SETTINGS_IDLE_MS) {
        close_and_save(s);
    }

    return before.open != s->open || before.page != s->page ||
           before.hold_pct != s->hold_pct ||
           before.step[0] != s->step[0] || before.step[1] != s->step[1] ||
           before.step[2] != s->step[2] || before.wifi_requested != s->wifi_requested ||
           before.beep_requested != s->beep_requested ||
           before.save_requested != s->save_requested;
}
```

- [ ] **Step 6: Run the tests and watch them pass**

```bash
cd firmware/host && make test
```

Expected: four binaries run, the last printing `26 checks, 0 failures` or thereabouts, and the ASan build repeating it.

- [ ] **Step 7: Add the table tests**

Append to `firmware/host/settings_menu_test.c`, before `main()`:

```c
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
        s.step[page] = 0;
    }
}

static void test_a_muted_step_asks_for_no_beep(void) {
    settings_t s = fresh();
    uint32_t t = run(&s, false, true, 2200, 1000);
    t = run(&s, false, false, 200, t);
    s.beep_requested = false;
    t = run(&s, false, true, 120, t);          // 5 -> 0, muted
    t = run(&s, false, false, 120, t);
    CHECK(s.step[SETTINGS_PAGE_VOLUME] == 0, "setup failed, volume is %u",
          s.step[SETTINGS_PAGE_VOLUME]);
    CHECK(!s.beep_requested, "muted asked for a tone to demonstrate silence");
    t = run(&s, false, true, 120, t);          // 0 -> 1
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
```

and add them to `main()`, after the existing calls:

```c
    test_the_gain_table_is_monotonic_and_ends_exactly();
    test_the_contrast_table_is_monotonic_and_tops_out_where_init_does();
    test_every_label_is_short_ascii_and_distinct();
    test_a_muted_step_asks_for_no_beep();
    test_init_clamps_nonsense_from_nvs();
```

- [ ] **Step 8: Run the tests**

```bash
cd firmware/host && make test
```

Expected: `0 failures` from both builds. The tables were written in Step 5, so these pass immediately — they are a guard against a later retune, not a red-green cycle.

- [ ] **Step 9: Commit**

```bash
git add firmware/main/settings_menu.h firmware/main/settings_menu.c \
        firmware/host/settings_menu_test.c firmware/host/Makefile
git commit -m "Add the settings carousel's gestures and value tables"
```

---

### Task 2: `settings_screen` — the panel

**Files:**
- Create: `firmware/main/settings_screen.h`
- Create: `firmware/main/settings_screen.c`
- Create: `firmware/host/settings_screen_test.c`
- Modify: `firmware/host/Makefile`

**Interfaces:**
- Consumes: `settings_t`, `settings_page_title()`, `settings_value_text()`, `settings_page_steps()`, `settings_eyes_emotion()` from Task 1. `ss_draw_text()` and `SS_GLYPH_H` from `setup_screen.h`. `FACE_W`, `FACE_H`, `FACE_FB_BYTES`, `face_render_pose()`, `face_emotion_pose()` from `face.h`.
- Produces: `settings_screen_render(uint8_t *fb, const settings_t *s)`, used by Task 5.

- [ ] **Step 1: Write the header**

Create `firmware/main/settings_screen.h`:

```c
// The settings carousel, drawn.
//
// Renders into a framebuffer it is handed and never touches I2C - the same
// contract setup_screen.c has, and the reason face_task can be the only
// writer to the panel without a lock. No ESP-IDF, no float, no allocation.
//
// The text is drawn with setup_screen.c's font rather than a second copy:
// one 5x7 table, one advance, one set of layout rules.

#pragma once

#include <stdint.h>

#include "settings_menu.h"

// Clears fb and draws the current page. Never draws outside the panel.
void settings_screen_render(uint8_t *fb, const settings_t *s);
```

- [ ] **Step 2: Write the failing layout test**

Create `firmware/host/settings_screen_test.c`:

```c
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
```

- [ ] **Step 3: Wire it into the Makefile**

Add to `firmware/host/Makefile`, next to the Task 1 rules:

```make
$(BUILD)/settings_screen_test: ../main/settings_screen.c ../main/settings_screen.h ../main/settings_menu.c ../main/setup_screen.c ../main/face.c settings_screen_test.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ ../main/settings_screen.c ../main/settings_menu.c ../main/setup_screen.c ../main/face.c settings_screen_test.c

$(ASAN_BUILD)/settings_screen_test: ../main/settings_screen.c ../main/settings_screen.h ../main/settings_menu.c ../main/setup_screen.c ../main/face.c settings_screen_test.c | $(ASAN_BUILD)
	$(CC) $(ASAN_CFLAGS) -o $@ ../main/settings_screen.c ../main/settings_menu.c ../main/setup_screen.c ../main/face.c settings_screen_test.c
```

Add `$(BUILD)/settings_screen_test` to the `test:` prerequisites and `@./$(BUILD)/settings_screen_test` to its recipe, and the same pair for `asan:`.

- [ ] **Step 4: Run and watch it fail**

```bash
cd firmware/host && make test
```

Expected: `fatal error: ../main/settings_screen.c: No such file or directory`.

- [ ] **Step 5: Write the renderer**

Create `firmware/main/settings_screen.c`:

```c
#include "settings_screen.h"

#include <string.h>

#include "face.h"
#include "setup_screen.h"

// The rows, as the spec fixes them. Everything here is a constant so the test
// can assert against the same numbers the panel gets.
#define ROW_TITLE 0
#define ROW_GAUGE 12
#define GAUGE_H 18
#define ROW_VALUE 33
#define ROW_HINT 45
#define ROW_DOTS 58
#define ROW_BAR 62

#define GAUGE_X 14
#define GAUGE_W 100
#define CELL_GAP 2

#define DOT_PITCH 12
#define DOT_SIZE 5

static void set_px(uint8_t *fb, int x, int y) {
    if (x < 0 || y < 0 || x >= FACE_W || y >= FACE_H) return;
    fb[(y / 8) * FACE_W + x] |= (uint8_t)(1u << (y % 8));
}

static void fill_rect(uint8_t *fb, int x, int y, int w, int h) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) set_px(fb, x + i, y + j);
}

static void frame_rect(uint8_t *fb, int x, int y, int w, int h) {
    fill_rect(fb, x, y, w, 1);
    fill_rect(fb, x, y + h - 1, w, 1);
    fill_rect(fb, x, y, 1, h);
    fill_rect(fb, x + w - 1, y, 1, h);
}

static int text_width(const char *s) {
    const int n = (int)strlen(s);
    return n ? n * SS_ADVANCE - 1 : 0;
}

static void draw_centred(uint8_t *fb, int y, const char *s) {
    ss_draw_text(fb, (FACE_W - text_width(s)) / 2, y, s);
}

// One cell per step, filled up to `filled`. Outlined rather than absent for
// the rest, so the scale's length reads at any setting.
static void draw_gauge(uint8_t *fb, int y, int h, uint8_t steps, uint8_t filled) {
    if (steps == 0) return;
    const int cell = (GAUGE_W - CELL_GAP * (steps - 1)) / steps;
    const int used = cell * steps + CELL_GAP * (steps - 1);
    const int x0 = GAUGE_X + (GAUGE_W - used) / 2;
    for (uint8_t i = 0; i < steps; i++) {
        const int x = x0 + i * (cell + CELL_GAP);
        if (i < filled) fill_rect(fb, x, y, cell, h);
        else frame_rect(fb, x, y, cell, h);
    }
}

static void draw_dots(uint8_t *fb, uint8_t page) {
    const int used = SETTINGS_PAGES * DOT_SIZE + (SETTINGS_PAGES - 1) * (DOT_PITCH - DOT_SIZE);
    const int x0 = (FACE_W - used) / 2;
    for (uint8_t i = 0; i < SETTINGS_PAGES; i++) {
        const int x = x0 + i * DOT_PITCH;
        if (i == page) fill_rect(fb, x, ROW_DOTS, DOT_SIZE, DOT_SIZE);
        else fill_rect(fb, x + 2, ROW_DOTS + 2, 2, 2);
    }
}

void settings_screen_render(uint8_t *fb, const settings_t *s) {
    const uint8_t page = (s->page < SETTINGS_PAGES) ? s->page : 0;

    if (page == SETTINGS_PAGE_EYES) {
        // The setting is a look, so the look is the gauge. face_render_pose()
        // clears the buffer itself and fills the middle of the panel; the
        // title and the dots go in the margins it leaves.
        face_render_pose(fb, face_emotion_pose(settings_eyes_emotion(s->step[SETTINGS_PAGE_EYES])));
    } else {
        memset(fb, 0, FACE_FB_BYTES);
    }

    char title[SS_LINE_MAX];
    const char *name = settings_page_title(page);
    size_t n = 0;
    title[n++] = '<';
    title[n++] = ' ';
    title[n++] = ' ';
    for (size_t i = 0; name[i] != '\0' && n + 4 < sizeof(title); i++) title[n++] = name[i];
    title[n++] = ' ';
    title[n++] = ' ';
    title[n++] = '>';
    title[n] = '\0';
    draw_centred(fb, ROW_TITLE, title);

    if (page == SETTINGS_PAGE_WIFI) {
        draw_centred(fb, 16, "set up network");
        draw_centred(fb, 30, "hold B to start");
        draw_centred(fb, ROW_HINT, "hold A to exit");
    } else if (page == SETTINGS_PAGE_EYES) {
        // No hint line: the pose fills the rows it would use, and there is
        // nothing here a hint could say that the WiFi page has not already
        // said on the way past.
    } else {
        draw_gauge(fb, ROW_GAUGE, GAUGE_H, settings_page_steps(page), s->step[page]);
        draw_centred(fb, ROW_VALUE, settings_value_text(s, page));
        draw_centred(fb, ROW_HINT, "hold A to exit");
    }

    draw_dots(fb, page);

    if (s->hold_pct > 0) {
        const uint8_t pct = (s->hold_pct > 100) ? 100 : s->hold_pct;
        fill_rect(fb, 0, ROW_BAR, (FACE_W * pct) / 100, 2);
    }
}
```

- [ ] **Step 6: Run the tests**

```bash
cd firmware/host && make test
```

Expected: `0 failures` from five binaries in the plain build and five under ASan.

If `test_the_gauge_has_one_cell_per_step` fails on the screen page, the cell arithmetic has produced a zero-width gap for four cells — print the computed `cell` and `x0` before adjusting `GAUGE_W`, and do not "fix" it by loosening the test.

- [ ] **Step 7: Commit**

```bash
git add firmware/main/settings_screen.h firmware/main/settings_screen.c \
        firmware/host/settings_screen_test.c firmware/host/Makefile
git commit -m "Draw the settings carousel, one page at a time"
```

---

### Task 3: a resting expression the face can be told

**Files:**
- Modify: `firmware/main/face.h:168` area (new declaration next to `face_set_reset_progress`)
- Modify: `firmware/main/face.c:710`, `:723`, `:916`
- Modify: `firmware/host/face_test.c`

**Interfaces:**
- Produces: `void face_set_resting(face_t *f, face_emotion_t e)`, called by Task 5.

- [ ] **Step 1: Write the failing test**

Append to `firmware/host/face_test.c`, before `main()`:

```c
// The face lets go of the last thing it felt after 45 seconds. What it lets
// go *into* is a setting now, not a constant.
static void test_the_resting_expression_is_what_idle_decays_to(void) {
    face_t f;
    face_init(&f, 0);
    face_set_resting(&f, FACE_EMO_HAPPY);
    face_set_state(&f, FACE_ST_IDLE, 0);
    face_set_emotion(&f, FACE_EMO_ANNOYED, 1000);

    face_tick(&f, 50000);   // past the 45 s decay, short of the 90 s sleepy
    uint8_t happy[FACE_FB_BYTES];
    memcpy(happy, face_framebuffer(&f), FACE_FB_BYTES);

    face_t g;
    face_init(&g, 0);
    face_set_resting(&g, FACE_EMO_NEUTRAL);
    face_set_state(&g, FACE_ST_IDLE, 0);
    face_set_emotion(&g, FACE_EMO_ANNOYED, 1000);
    face_tick(&g, 50000);

    CHECK(memcmp(happy, face_framebuffer(&g), FACE_FB_BYTES) != 0,
          "resting on happy and resting on neutral render the same");
}

static void test_a_fresh_face_rests_neutral(void) {
    face_t f;
    face_init(&f, 0);
    face_set_state(&f, FACE_ST_IDLE, 0);
    face_tick(&f, 50000);
    uint8_t untold[FACE_FB_BYTES];
    memcpy(untold, face_framebuffer(&f), FACE_FB_BYTES);

    face_t g;
    face_init(&g, 0);
    face_set_resting(&g, FACE_EMO_NEUTRAL);
    face_set_state(&g, FACE_ST_IDLE, 0);
    face_tick(&g, 50000);

    CHECK(memcmp(untold, face_framebuffer(&g), FACE_FB_BYTES) == 0,
          "a face nobody configured does not rest neutral");
}

static void test_an_out_of_range_resting_emotion_is_ignored(void) {
    face_t f;
    face_init(&f, 0);
    face_set_resting(&f, (face_emotion_t)99);
    face_set_state(&f, FACE_ST_IDLE, 0);
    face_tick(&f, 50000);
    uint8_t got[FACE_FB_BYTES];
    memcpy(got, face_framebuffer(&f), FACE_FB_BYTES);

    face_t g;
    face_init(&g, 0);
    face_set_state(&g, FACE_ST_IDLE, 0);
    face_tick(&g, 50000);

    CHECK(memcmp(got, face_framebuffer(&g), FACE_FB_BYTES) == 0,
          "nonsense from NVS changed the resting face");
}
```

and to `main()`:

```c
    test_the_resting_expression_is_what_idle_decays_to();
    test_a_fresh_face_rests_neutral();
    test_an_out_of_range_resting_emotion_is_ignored();
```

- [ ] **Step 2: Run and watch it fail**

```bash
cd firmware/host && make test
```

Expected: `error: implicit declaration of function 'face_set_resting'`, which `-Werror` turns into a build failure.

- [ ] **Step 3: Declare it**

In `firmware/main/face.h`, immediately after the `face_set_reset_progress` declaration:

```c
// Which pose the face relaxes into when a conversation is over, and what it
// wakes up as. Set from the device's settings; out of range is ignored, so a
// corrupt byte in NVS cannot leave the face rendering nothing.
void face_set_resting(face_t *f, face_emotion_t e);
```

and add the field to `face_t`, next to `emotion`:

```c
    face_emotion_t resting;   // what idle decays to, FACE_EMO_NEUTRAL unless told
```

- [ ] **Step 4: Implement it**

In `firmware/main/face.c`, add next to `face_set_emotion`:

```c
void face_set_resting(face_t *f, face_emotion_t e) {
    if (e >= FACE_EMO_COUNT) return;
    f->resting = e;
}
```

In `face_init`, beside the line that sets `f->emotion = FACE_EMO_NEUTRAL;` (around `face.c:710`):

```c
    f->resting = FACE_EMO_NEUTRAL;
```

Leave `f->cur = EMO_POSE[FACE_EMO_NEUTRAL];` at `:723` alone — it is the pose the face is slewing *from* at boot, and the boot animation owns it.

In `face_tick`, in the idle decay ladder (around `face.c:916`), replace the constant:

```c
        } else if (idle_ms > 45000u) {
            emo = f->resting;
        }
```

- [ ] **Step 5: Run the tests**

```bash
cd firmware/host && make test
```

Expected: `0 failures`, both builds. The face preview still builds, because nothing about its interface changed.

- [ ] **Step 6: Commit**

```bash
git add firmware/main/face.h firmware/main/face.c firmware/host/face_test.c
git commit -m "Let the face be told which expression to rest in"
```

---

### Task 4: three settings in NVS

**Files:**
- Modify: `firmware/main/config_store.h`
- Modify: `firmware/main/config_store.c`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: three `uint8_t` fields on `device_config_t` — `volume`, `screen`, `eyes` — plus `esp_err_t config_save_settings(uint8_t volume, uint8_t screen, uint8_t eyes)`. Task 5 reads the fields and calls the save; Task 6 reads the live menu state, not these.

- [ ] **Step 1: Extend the struct and declare the save**

In `firmware/main/config_store.h`, add to `device_config_t`:

```c
typedef struct {
    char ssid[PL_SSID_MAX + 1];
    char pass[PL_PASS_MAX + 1];
    char uri[PL_URI_MAX + 1];

    // The settings the two buttons change. Step indices, not values: the
    // lookup tables in settings_menu.c can be retuned without migrating what
    // is already written here. Absent keys read as the defaults below, so a
    // device flashed with this firmware behaves exactly like the last one.
    uint8_t volume;  // 0-5, default 5 (unity, today's behaviour)
    uint8_t screen;  // 0-3, default 3 (0xcf, what ssd1306.c writes at init)
    uint8_t eyes;    // 0-3, default 0 (calm)
} device_config_t;
```

and add `#include <stdint.h>` at the top beside `<stdbool.h>`, then declare, under `config_save_uri`:

```c
// Written once when the settings menu closes, not on every press: walking the
// volume page in a circle is six presses and would otherwise be six writes.
esp_err_t config_save_settings(uint8_t volume, uint8_t screen, uint8_t eyes);
```

- [ ] **Step 2: Implement the load and the save**

In `firmware/main/config_store.c`, add above `config_load`:

```c
static uint8_t load_u8(nvs_handle_t h, const char *key, uint8_t fallback) {
    uint8_t v = 0;
    if (h != 0 && nvs_get_u8(h, key, &v) == ESP_OK) return v;
    return fallback;
}
```

then inside `config_load`, after the three `load_one` calls and before `nvs_close`:

```c
    out->volume = load_u8(h, "vol", 5);
    out->screen = load_u8(h, "bright", 3);
    out->eyes = load_u8(h, "eyes", 0);
```

and extend the log line at the end of `config_load` so a boot says what it came up with:

```c
    ESP_LOGI(TAG, "ssid \"%s\", uri \"%s\", vol %u, bright %u, eyes %u", out->ssid,
             out->uri, out->volume, out->screen, out->eyes);
```

Add the save at the end of the file:

```c
esp_err_t config_save_settings(uint8_t volume, uint8_t screen, uint8_t eyes) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_u8(h, "vol", volume);
    if (err == ESP_OK) err = nvs_set_u8(h, "bright", screen);
    if (err == ESP_OK) err = nvs_set_u8(h, "eyes", eyes);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
```

`save_pair()` is left alone rather than generalised: it takes strings, these are bytes, and a helper that handles both would be longer than either.

- [ ] **Step 3: Build**

```bash
cd firmware && idf.py -DSKETCH=voice build
```

Expected: `Project build complete.` Nothing calls the new function yet, and `-Wunused` does not fire on a non-static one.

Remember the toolchain note in `firmware/README.md`: `deactivate; unset VIRTUAL_ENV` before `. ~/esp/esp-idf/export.sh`, or the ESP-IDF Python environment collides with this repo's venv.

- [ ] **Step 4: Commit**

```bash
git add firmware/main/config_store.h firmware/main/config_store.c
git commit -m "Store the three device settings in NVS"
```

---

### Task 5: the second button, and the menu on the panel

**Files:**
- Modify: `firmware/main/CMakeLists.txt:31`
- Modify: `firmware/main/voice_main.c` — pin defines near `:54`, `audio_init()` near `:416`, `button_task()` near `:1280`, `face_task()` near `:975`

**Interfaces:**
- Consumes: everything Tasks 1-4 produce.
- Produces: file-scope `static settings_t s_settings;` and `static volatile bool s_button_b_down;`, both read by Task 6.

- [ ] **Step 1: Add the sources to the build**

In `firmware/main/CMakeLists.txt`, extend the `voice` list:

```cmake
if(SKETCH STREQUAL "voice")
    list(APPEND SKETCH_SRCS "provision_logic.c" "setup_screen.c" "config_store.c" "provision.c"
                            "settings_menu.c" "settings_screen.c")
endif()
```

- [ ] **Step 2: Define the pin and the state**

In `firmware/main/voice_main.c`, after `#define PIN_BUTTON GPIO_NUM_3`:

```c
// The second button, wired to ground exactly like the first.
//
// GPIO 20 is U0RXD, and it is free only because the console is not on UART0 -
// CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y. On a board with a CP2102 or CH340
// bridge this pin is driven by the bridge and a button on it would fight an
// output. It is not a strapping pin, so a press during reset is harmless.
#define PIN_BUTTON_B GPIO_NUM_20
```

Add the includes beside the existing ones:

```c
#include "settings_menu.h"
#include "settings_screen.h"
```

and the state, beside `s_button_down`:

```c
// Debounced second button, owned by button_task alongside the first.
static volatile bool s_button_b_down = false;
// The settings carousel. Written only by face_task, which is also the only
// task that draws, so the screen can never disagree with the state behind it.
static settings_t s_settings;
```

- [ ] **Step 3: Configure the pin**

There is no `button_init()`. The button's GPIO is configured inside
`audio_init()`, in the local `btn` config just after the mute pin. One line
changes — both buttons want the identical mode, pull and interrupt setting, so
they share the call:

```c
    gpio_config_t btn = {
        .pin_bit_mask = (1ULL << PIN_BUTTON) | (1ULL << PIN_BUTTON_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn));
```

Leave the rest of `audio_init()` untouched, including the `gpio_set_level(PIN_MUTE, 0)`
that mutes the amplifier before anything else exists.

- [ ] **Step 4: Debounce both pins in the one task**

Replace the body of `button_task()`:

```c
// Samples both buttons and nothing else, so their timing cannot be affected
// by a send that is waiting on the network. One task for two pins: the reason
// it exists in the first place is the same for both.
static void button_task(void *arg) {
    struct { gpio_num_t pin; bool stable; TickType_t changed; volatile bool *out; } b[2] = {
        {PIN_BUTTON, false, 0, &s_button_down},
        {PIN_BUTTON_B, false, 0, &s_button_b_down},
    };

    while (true) {
        const TickType_t now = xTaskGetTickCount();
        for (int i = 0; i < 2; i++) {
            const bool down = gpio_get_level(b[i].pin) == 0;
            if (down != b[i].stable && (now - b[i].changed) > pdMS_TO_TICKS(DEBOUNCE_MS)) {
                b[i].stable = down;
                b[i].changed = now;
                *b[i].out = down;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
```

- [ ] **Step 5: Drive and draw the menu from `face_task`**

`face_task` already runs at a fixed 40 ms tick and already owns the panel, so the menu needs no task and no stack of its own.

Near the top of `face_task`, after `face_init`:

```c
    settings_init(&s_settings, s_cfg.volume, s_cfg.screen, s_cfg.eyes);
    face_set_resting(&s_face, settings_eyes_emotion(s_settings.step[SETTINGS_PAGE_EYES]));
    ssd1306_set_contrast(&s_panel, settings_screen_contrast(s_settings.step[SETTINGS_PAGE_SCREEN]));
    uint8_t shown_screen_step = s_settings.step[SETTINGS_PAGE_SCREEN];
```

This needs the loaded config visible from `face_task`, which `app_main`
currently keeps in a local `cfg`. Add a file-scope copy beside the other state:

```c
// What config_load() returned, kept where face_task can seed the menu from it.
static device_config_t s_cfg;
```

Leave `app_main`'s local `cfg` exactly as it is — provisioning reloads it and
several later lines read it — and add one line right after its
`config_load(&cfg);`:

```c
    s_cfg = cfg;  // face_task seeds the settings menu from this
```

`face_task` is created before this line runs, so it must tolerate a zeroed
`s_cfg` for the few milliseconds in between: `settings_init()` clamps every
out-of-range step to 0, and 0 is a valid step for all three.

Inside the loop, before the drawing branch:

```c
        // The menu is driven from here because this task already ticks at a
        // fixed 40 ms and already owns the panel. 40 ms against a 25 ms
        // debounce and a 1 s hold has room to spare.
        settings_tick(&s_settings, s_button_down, s_button_b_down, t);

        if (s_settings.step[SETTINGS_PAGE_SCREEN] != shown_screen_step) {
            shown_screen_step = s_settings.step[SETTINGS_PAGE_SCREEN];
            ssd1306_set_contrast(&s_panel, settings_screen_contrast(shown_screen_step));
        }

        if (s_settings.wifi_requested) {
            s_settings.wifi_requested = false;
            ESP_LOGW(TAG, "settings: wifi setup requested, restarting into provisioning");
            config_request_provisioning();
            vTaskDelay(pdMS_TO_TICKS(100));  // let the log line reach the console
            esp_restart();
        }

        if (s_settings.save_requested) {
            s_settings.save_requested = false;
            face_set_resting(&s_face, settings_eyes_emotion(s_settings.step[SETTINGS_PAGE_EYES]));
            const esp_err_t serr = config_save_settings(s_settings.step[SETTINGS_PAGE_VOLUME],
                                                        s_settings.step[SETTINGS_PAGE_SCREEN],
                                                        s_settings.step[SETTINGS_PAGE_EYES]);
            if (serr != ESP_OK) {
                // The values are live and already applied; losing them at the
                // next boot is worth less than making a reboot of it.
                ESP_LOGW(TAG, "settings: nvs write failed (%s)", esp_err_to_name(serr));
            } else {
                ESP_LOGI(TAG, "settings: saved vol=%u bright=%u eyes=%u",
                         s_settings.step[SETTINGS_PAGE_VOLUME],
                         s_settings.step[SETTINGS_PAGE_SCREEN],
                         s_settings.step[SETTINGS_PAGE_EYES]);
            }
        }
```

Then the drawing branch. The existing `if (provision_is_active()) { ... } else { ... }` grows a first arm:

```c
        static uint8_t s_setup_fb[FACE_FB_BYTES];
        const uint8_t *fb;
        if (s_settings.open) {
            settings_screen_render(s_setup_fb, &s_settings);
            fb = s_setup_fb;
        } else if (provision_is_active()) {
            /* ... unchanged ... */
```

The menu reuses `s_setup_fb` rather than taking a second kilobyte: provisioning and the settings menu cannot be up at once, and both render into a buffer they are handed.

Feed the hold to the face so the bar shows on the way in, and tell the face about the second button:

```c
        const bool down = s_button_down || s_button_b_down;
        if (down != button) {
            face_set_button(&s_face, down, t);
            button = down;
        }
```

and in the `else` arm, where `face_set_reset_progress` is called, hand it the menu's hold instead of `s_face_reset_pct`:

```c
            face_set_reset_progress(&s_face, s_settings.hold_pct, t);
```

- [ ] **Step 6: Build**

```bash
cd firmware && idf.py -DSKETCH=voice build
```

Expected: `Project build complete.` If `s_cfg` is reported as unused, the `s_cfg = cfg;` line in `app_main` was missed.

- [ ] **Step 7: Flash and check it on the bench**

```bash
idf.py -p /dev/cu.usbmodem* flash monitor
```

Walk this list and write down what happened — every line is a claim this task is making:

1. Hold `B` for two seconds: a progress bar crosses the bottom of the eyes from about 40%, then `VOLUME` appears with a gauge and four dots.
2. Tap `A` four times: `SCREEN`, `EYES`, `WIFI`, back to `VOLUME`. The filled dot follows.
3. On `SCREEN`, tap `B`: the panel visibly dims and brightens as it wraps. This is the one setting you can confirm without a second sense.
4. On `EYES`, tap `B`: four different faces, and no gauge.
5. Hold `A` for a second: the menu closes and the monitor prints `settings: saved vol=… bright=… eyes=…`.
6. Reboot. `config: ssid "…", uri "…", vol … bright … eyes …` reports what you left, and the panel comes up at that brightness.
7. Open the menu, then walk away. After 20 seconds it closes on its own and saves.
8. Hold `A` alone on the face: it still records a question, and the menu does not open.
9. Ask a question and get a reply, so nothing about the second button broke the first.

- [ ] **Step 8: Commit**

```bash
git add firmware/main/CMakeLists.txt firmware/main/voice_main.c
git commit -m "Put the settings carousel on the panel, on the second button"
```

---

### Task 6: volume, and a tone to set it by

**Files:**
- Modify: `firmware/main/voice_main.c` — `audio_out_task()` near `:1195` (the decode) and `:1230` (the end-of-reply branch)

**Interfaces:**
- Consumes: `s_settings`, `settings_volume_gain()`.
- Produces: nothing new; this is the last consumer.

- [ ] **Step 1: Scale the decoded samples**

In `audio_out_task`, the block that currently reads:

```c
            const size_t n = adpcm_decode_block(coded, got, pcm);

            s_audio_level = block_level(pcm, n);

            for (size_t i = 0; i < n; i++) {
                frame[i * 2] = (int32_t)pcm[i] << 16;
                frame[i * 2 + 1] = 0;
            }
```

becomes:

```c
            const size_t n = adpcm_decode_block(coded, got, pcm);

            // Before the scaling, deliberately. This feeds the eyes, and they
            // squash on the reply's own syllables: scale first and the face
            // goes still at low volume, which would say the assistant is
            // mumbling when it is only quiet.
            s_audio_level = block_level(pcm, n);

            // The gain ramps across the block rather than stepping between
            // two samples. A step is a click - the same reason fill_tone() in
            // playback_main.c ramps its ends.
            const int32_t want = settings_volume_gain(s_settings.step[SETTINGS_PAGE_VOLUME]);
            for (size_t i = 0; i < n; i++) {
                const int32_t g = n > 1 ? gain + (int32_t)(((int64_t)(want - gain) * (int32_t)i) / (int32_t)(n - 1))
                                        : want;
                const int32_t v = ((int32_t)pcm[i] * g) >> 15;
                frame[i * 2] = v << 16;
                frame[i * 2 + 1] = 0;
            }
            gain = want;
```

and declare the carried gain with the task's other locals, at the top of `audio_out_task`:

```c
    int32_t gain = settings_volume_gain(s_settings.step[SETTINGS_PAGE_VOLUME]);
```

- [ ] **Step 2: Play the tone the menu asks for**

Add above `audio_out_task`:

```c
// A short blip at the current volume, so the knob can be set by ear.
//
// It is played from audio_out_task and nowhere else. The amplifier belongs to
// one task, the I2S channel is created once at boot and never re-initialised,
// and the drain before muting is what keeps the tail of a word. Reaching
// around that from the menu's own context is how the pop comes back.
#define BEEP_HZ 660
#define BEEP_MS 150
#define BEEP_RAMP_MS 20
#define BEEP_AMPLITUDE 8000

// `frame` is the caller's own I2S staging buffer, borrowed rather than
// duplicated: this runs on audio_out_task, nothing else can be using it, and
// a second static copy would cost 4 KB of BSS on a part whose free heap fell
// to 30 KB the moment wss:// was switched on.
static void play_beep(int32_t *frame, int32_t gain) {
    if (gain == 0) return;  // at muted, silence is the value being demonstrated

    const int total = (SAMPLE_RATE * BEEP_MS) / 1000;
    const int ramp = (SAMPLE_RATE * BEEP_RAMP_MS) / 1000;
    int phase_i = 0;

    amp_enable(true);
    for (int done = 0; done < total; done += BLOCK_SAMPLES) {
        const int n = (total - done < BLOCK_SAMPLES) ? (total - done) : BLOCK_SAMPLES;
        for (int i = 0; i < n; i++) {
            const int at = done + i;
            int32_t env = 256;
            if (at < ramp) env = (at * 256) / ramp;
            else if (at > total - ramp) env = ((total - at) * 256) / ramp;

            // A 16-step square-ish sine from a table would cost a table; this
            // is a triangle, which at 660 Hz through a small speaker is
            // indistinguishable and needs no float and no <math.h>.
            const int period = SAMPLE_RATE / BEEP_HZ;
            const int p = phase_i % period;
            const int tri = (p < period / 2) ? (p * 2 * 256) / period - 256
                                             : 256 - ((p - period / 2) * 2 * 256) / period;
            phase_i++;

            int32_t v = (BEEP_AMPLITUDE * tri) >> 8;
            v = (v * env) >> 8;
            v = (v * gain) >> 15;
            frame[i * 2] = v << 16;
            frame[i * 2 + 1] = 0;
        }
        size_t written = 0;
        i2s_channel_write(s_tx, frame, (size_t)n * 2 * sizeof(int32_t), &written, portMAX_DELAY);
    }
    vTaskDelay(pdMS_TO_TICKS(DRAIN_MS));
    amp_enable(false);
}
```

and call it near the top of `audio_out_task`'s loop, before the play-buffer receive:

```c
        if (s_settings.beep_requested) {
            s_settings.beep_requested = false;
            // Not over a reply. If audio is coming out, the reply is already
            // the demonstration.
            if (!s_playing) {
                play_beep(frame, settings_volume_gain(s_settings.step[SETTINGS_PAGE_VOLUME]));
            }
        }
```

- [ ] **Step 2a: Check what `play_beep` borrows is really in scope**

`DRAIN_MS` (150) and `BLOCK_SAMPLES` (512) are file-scope `#define`s in
`voice_main.c`, and `s_tx` and `amp_enable()` are file-scope too. `frame` is
`audio_out_task`'s own local — confirm it is declared `int32_t frame[BLOCK_SAMPLES * 2]`
there before passing it, and pass it rather than declaring a second one. If
any of these has moved, find it before inventing a replacement: a second
definition of the drain is exactly how the two get out of step.

- [ ] **Step 3: Build**

```bash
cd firmware && idf.py -DSKETCH=voice build
```

Expected: `Project build complete.`

- [ ] **Step 4: Flash and check it by ear**

```bash
idf.py -p /dev/cu.usbmodem* flash monitor
```

1. Open the menu on `VOLUME` and tap `B` through all six steps. Five tones, each louder than the last, and silence at `muted`.
2. There is no click at either end of any tone. A click means the ramp is not being applied — check `BEEP_RAMP_MS` against `total`.
3. Leave it at `-12 dB`, exit, and ask a question. The reply is audibly quieter, and **the eyes still move with the speech** — that is the `s_audio_level`-before-scaling line doing its job.
4. Set `muted`, exit, ask a question. Nothing is heard, the eyes still move, and the monitor still logs `idle: played … B`.
5. Set it back to `0 dB` and confirm the reply sounds exactly as it did before this task.
6. Open the menu *while a reply is playing* and change the volume: the reply changes level within a block or two and does not click.

- [ ] **Step 5: Commit**

```bash
git add firmware/main/voice_main.c
git commit -m "Make the volume setting audible, and settable by ear"
```

---

### Task 7: retire the five taps

**Files:**
- Modify: `firmware/main/voice_main.c` — `:1002` (the offline hint), `:1308`-`:1345` (the tap apparatus), `:1347`-`:1377` (`boot_gesture_task`), `net_task` near `:1385`-`:1410`, `app_main` near `:1640`-`:1670`
- Modify: `firmware/README.md` (the wiring table)
- Modify: `RESUME.md` (the hardware table and the gesture description)

**Interfaces:**
- Consumes: `s_button_b_down` from Task 5, `SETTINGS_OPEN_MS` and `SETTINGS_WARN_PCT` from Task 1.
- Produces: nothing. This task only removes.

- [ ] **Step 1: Convert the way out of provisioning to a `B` hold**

In `app_main`'s provisioning loop, replace the tap counting with a hold timer:

```c
            // Four ways out: a successful trial plus its grace window, five
            // minutes with nobody using the page, the same B hold that opens
            // settings everywhere else, or provisioning stopping on its own.
            // Only the first is the happy one.
            //
            // The hold is timed here rather than in face_task because the
            // settings menu is not up while the setup screen is: this screen
            // owns the panel, and SS_STATUS_LEAVING is how it says so.
            TickType_t held_since = 0;
            uint8_t shown = 0;
            while (provision_is_active() && !provision_complete() && !provision_idle_expired()) {
                const TickType_t now = xTaskGetTickCount();
                if (!s_button_b_down) {
                    held_since = 0;
                } else if (held_since == 0) {
                    held_since = now;
                }

                const uint32_t held =
                    held_since ? (uint32_t)(now - held_since) * portTICK_PERIOD_MS : 0;
                if (held >= SETTINGS_OPEN_MS) {
                    ESP_LOGW(TAG, "B held: leaving setup without configuring");
                    break;
                }

                // The same warning the entry gesture gives, in the only place
                // this screen has for it: a gesture that fires with no notice
                // is exactly what the countdown exists to prevent.
                const uint8_t pct = (uint8_t)((held * 100u) / SETTINGS_OPEN_MS);
                if (pct >= SETTINGS_WARN_PCT && shown == 0) {
                    provision_set_status(SS_STATUS_LEAVING);
                    shown = 1;
                } else if (pct < SETTINGS_WARN_PCT && shown != 0) {
                    provision_set_status(SS_STATUS_WAITING);
                    shown = 0;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
```

- [ ] **Step 2: Delete `boot_gesture_task` and its scaffolding**

Remove the whole `boot_gesture_task` function together with its comment block, and in `app_main` remove the three lines around `wifi_connect`:

```c
    TaskHandle_t boot_gesture = NULL;
    xTaskCreate(boot_gesture_task, "boot_gesture", 2048, NULL, 4, &boot_gesture);
    wifi_connect(cfg.ssid, cfg.pass);
    vTaskDelete(boot_gesture);
```

leaving:

```c
    // No gesture task is needed here any more. face_task is created at the
    // top of app_main and button_task right after it, both long before this
    // line, so holding B opens settings even while wifi_connect() is still
    // waiting for a network it will never find. That window is the entire
    // reason boot_gesture_task existed.
    wifi_connect(cfg.ssid, cfg.pass);
```

- [ ] **Step 3: Delete the tap counter and `net_task`'s use of it**

Remove `tap_counter_t`, `tap_count()`, `RESET_TAPS`, `RESET_TAPS_VISIBLE` and `RESET_WINDOW_MS`, and the comment block above them that explains the two callers.

In `net_task`, remove the local `tap_counter_t taps_in = {0};` and the whole block that begins:

```c
        const uint8_t taps = tap_count(&taps_in, down, now);
```

through the end of its `if (taps >= RESET_TAPS) { ... }` body.

Then remove `s_face_reset_pct` entirely — Task 5 replaced its only remaining reader with `s_settings.hold_pct`.

Keep `SHORT_PRESS_MS`, and correct its comment, which currently blames the tap gesture for its existence:

```c
// Below this, a press was not a question - nobody says anything in a fifth of
// a second - so the utterance is cancelled rather than ended. It was added
// when the reset gesture's taps each cost a Groq STT call; that gesture is
// gone, and this stays for the reason that outlived it. An accidental brush
// against the button should not spend a round trip on the link that is this
// project's blocking problem.
#define SHORT_PRESS_MS 200
```

- [ ] **Step 4: Teach the new gesture where the old one was taught**

At `voice_main.c:1002`, the offline hint:

```c
                ss_draw_text(s_setup_fb, 0, FACE_H - SS_GLYPH_H, "hold B: settings");
```

Not "hold B for setup": the hold opens the menu, and WiFi is a page inside it. Sixteen characters, inside the 21 a line holds. This string is the only place any of this is ever taught.

- [ ] **Step 5: Build and confirm nothing is left behind**

```bash
cd firmware && idf.py -DSKETCH=voice build
grep -n "tap_count\|RESET_TAPS\|boot_gesture\|s_face_reset_pct\|5 presses" main/voice_main.c
```

Expected: `Project build complete.`, and the `grep` prints nothing at all.

- [ ] **Step 6: Update the two documents that describe the hardware**

In `firmware/README.md`, the wiring table under "Wiring this sketch assumes" gains a row, and the sentence about GPIO10 is untouched:

```markdown
| Button (to GND) | 3 | — |
| Button B (to GND) | 20 | — |
```

In `RESUME.md`, the Hardware table gains:

```markdown
| Button B to GND | 20 | settings: hold 2 s |
```

and the line below it, `Do not use GPIO 9 (BOOT), 18/19 (USB), 2 (strapping).`, gains a sentence:

```markdown
GPIO 20 is U0RXD and is free only because the console is USB-Serial-JTAG. A
board with a CP2102 or CH340 bridge cannot use it. GPIO 21 is the last free
pin after it.
```

- [ ] **Step 7: Flash and confirm the old gesture is gone and the new one is everywhere**

```bash
idf.py -p /dev/cu.usbmodem* flash monitor
```

1. Tap `A` five times quickly on the face. Nothing happens — no provisioning, no countdown on the eyes. This is the change; confirm it deliberately.
2. Hold `B` two seconds, walk to `WIFI`, hold `B` again: the device restarts into the access point.
3. On the setup screen, hold `B` two seconds: the screen says it is leaving, then the device carries on to the saved network.
4. Power up with the router switched off. Once the eyes have been shut for a while, the panel says `hold B: settings`. Hold `B`: the menu opens, even though `wifi_connect()` is still waiting. Walk to `WIFI` and hold `B` to get out. **This is the case `boot_gesture_task` existed for** — if it does not work, the deletion in Step 2 was wrong, not the plan.

- [ ] **Step 8: Commit**

```bash
git add firmware/main/voice_main.c firmware/README.md RESUME.md
git commit -m "Retire the five-tap gesture; one hold does all of its jobs"
```

---

## After the last task

Run the whole host suite once more, since Tasks 3 and 7 touched files the earlier tests cover:

```bash
cd firmware/host && make test
```

Expected: five binaries, `0 failures` each, in the plain build and again under ASan.

Then the two checks that only a person can make, both from `RESUME.md`'s own list of what matters: hold the button, ask a real question, and get a spoken reply; and confirm `docs/superpowers/specs/2026-09-03-two-button-settings-design.md` still describes what was built. Where it does not, the spec is what gets corrected — it is the record.
