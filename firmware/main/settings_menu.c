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

// The step cannot leave its range through this file - settings_init() clamps
// what NVS hands over and bump_value() takes a modulus - but the struct
// belongs to the caller, and every other accessor here guards its table
// anyway. This one does too, so writing step[] directly is a bad idea rather
// than an out-of-bounds read.
//
// Each page clamps the way the accessor for the same page clamps, not
// uniformly to zero: what the panel prints has to be the value actually in
// force. settings_volume_gain() and settings_screen_contrast() saturate at the
// top step, settings_eyes_emotion() falls back to the first, and the text
// follows each of them.
const char *settings_value_text(const settings_t *s, uint8_t page) {
    switch (page) {
        case SETTINGS_PAGE_VOLUME: {
            const uint8_t step = s->step[SETTINGS_PAGE_VOLUME];
            return VOLUME_TEXT[(step < SETTINGS_VOLUME_STEPS) ? step
                                                             : SETTINGS_VOLUME_STEPS - 1];
        }
        case SETTINGS_PAGE_SCREEN: {
            const uint8_t step = s->step[SETTINGS_PAGE_SCREEN];
            return SCREEN_TEXT[(step < SETTINGS_SCREEN_STEPS) ? step
                                                              : SETTINGS_SCREEN_STEPS - 1];
        }
        case SETTINGS_PAGE_EYES: {
            const uint8_t step = s->step[SETTINGS_PAGE_EYES];
            return EYES_TEXT[(step < SETTINGS_EYES_STEPS) ? step : 0];
        }
        default: return "";
    }
}

// Out-of-range input (garbage from a corrupted NVS entry) clamps to 0 rather
// than to the top step - 0 is the value every table treats as the quiet,
// unsurprising default.
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
    if (!s->open) return is_a ? 0 : SETTINGS_OPEN_MS;  // only B opens
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

// The only place s->open is assigned - other than settings_init()'s
// memset(), which is construction zeroing a struct, not a way in or out of
// the menu, so it is not a bypass of this rule even though it also touches
// the field. hold_target() depends on s->open, so the instant it changes,
// every button currently down is holding time that was measured against a
// target that no longer applies - one that only just became applicable.
//
// A held duration that already meets or exceeds the newly applicable target
// is demonstrably stale: that press is spent (a_used/b_used), exactly as if
// it had just fired a gesture of its own, and does nothing more until it is
// released and pressed again. A held duration that falls short has earned
// nothing under the new meaning - rather than let it sit at partial credit
// (which would let it borrow time from before the flip and fire a shorter
// gesture than the one actually running under the new meaning), its clock
// restarts from this instant, so the gesture it is now part of begins
// honestly, at the moment the new meaning began. A newly applicable target
// of 0 can never be met, so it always restarts - harmless, and it keeps
// this branch simple rather than adding a case for "can't fire anyway."
//
// A restarted press may still finish a hold - that is the point of
// restarting it - but it must never complete as a tap: the release path's
// tap check only sees the restarted clock, with no memory of time spent
// under the old meaning, so a press held for half a second before the menu
// existed and released shortly after it opens would otherwise look exactly
// like a deliberate quick tap. a_no_tap/b_no_tap record that distinction
// (used forbids everything; no_tap forbids only a tap) and are cleared on
// the next press, alongside used, so this bars exactly the remainder of the
// physical press that carried it - not the button.
//
// This is what keeps an exit-hold's close from being undone in the same
// tick by a neighbour's stale rest, and an open-hold's open from being
// undone on the next tick by push-to-talk's stale hold - without also
// discarding a press that has barely started, started on the very tick of
// the flip, or predates the flip and is released just after it. A fourth
// way in or out of the menu must come through here to inherit the rule
// rather than reimplement it.
static void set_open(settings_t *s, bool new_open, bool a_down, bool b_down, uint32_t now_ms) {
    if (s->open == new_open) return;
    s->open = new_open;

    if (a_down && !s->a_used) {
        const uint32_t target = hold_target(s, true);
        if (target && (now_ms - s->a_at) >= target) {
            s->a_used = true;
        } else {
            s->a_at = now_ms;
            s->a_no_tap = true;
        }
    }
    if (b_down && !s->b_used) {
        const uint32_t target = hold_target(s, false);
        if (target && (now_ms - s->b_at) >= target) {
            s->b_used = true;
        } else {
            s->b_at = now_ms;
            s->b_no_tap = true;
        }
    }
}

static void close_and_save(settings_t *s, bool a_down, bool b_down, uint32_t now_ms) {
    set_open(s, false, a_down, b_down, now_ms);
    s->save_requested = true;
    s->hold_pct = 0;
}

bool settings_tick(settings_t *s, bool a_down, bool b_down, uint32_t now_ms) {
    const settings_t before = *s;

    const bool edges[2] = {a_down, b_down};
    bool *was[2] = {&s->a_was, &s->b_was};
    bool *used[2] = {&s->a_used, &s->b_used};
    bool *no_tap[2] = {&s->a_no_tap, &s->b_no_tap};
    uint32_t *at[2] = {&s->a_at, &s->b_at};

    for (int i = 0; i < 2; i++) {
        const bool is_a = (i == 0);
        const uint32_t target = hold_target(s, is_a);

        if (edges[i] && !*was[i]) {  // press
            *at[i] = now_ms;
            *used[i] = false;
            *no_tap[i] = false;
            s->last_input = now_ms;
        } else if (edges[i] && *was[i]) {  // still held
            const uint32_t held = now_ms - *at[i];
            if (target && !*used[i] && held >= target) {
                *used[i] = true;
                s->last_input = now_ms;
                if (!s->open) {
                    set_open(s, true, a_down, b_down, now_ms);
                    s->page = SETTINGS_PAGE_VOLUME;
                } else if (is_a) {
                    close_and_save(s, a_down, b_down, now_ms);
                } else {
                    s->wifi_requested = true;
                }
            }
        } else if (!edges[i] && *was[i]) {  // release
            const uint32_t held = now_ms - *at[i];
            if (!*used[i] && !*no_tap[i] && s->open && was_a_tap(held, target)) {
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
        // Once held reaches target the share is 100% regardless of how much
        // further it grows, so clamping here loses nothing - and it is what
        // keeps held * 100u from overflowing uint32_t on a hold somewhere
        // past the twelve-hour mark. Not reachable given the targets in this
        // file (at most SETTINGS_OPEN_MS), but cheap enough to make
        // impossible rather than merely unlikely.
        const uint32_t capped = (held < target) ? held : target;
        const uint32_t share = (capped * 100u) / target;
        if (share >= SETTINGS_WARN_PCT) pct = (uint8_t)share;
    }
    s->hold_pct = pct;

    if (s->open && (uint32_t)(now_ms - s->last_input) > SETTINGS_IDLE_MS) {
        close_and_save(s, a_down, b_down, now_ms);
    }

    return before.open != s->open || before.page != s->page ||
           before.hold_pct != s->hold_pct ||
           before.step[0] != s->step[0] || before.step[1] != s->step[1] ||
           before.step[2] != s->step[2] || before.wifi_requested != s->wifi_requested ||
           before.beep_requested != s->beep_requested ||
           before.save_requested != s->save_requested;
}
