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

static void close_and_save(settings_t *s) {
    s->open = false;
    s->save_requested = true;
    s->hold_pct = 0;
}

// A press means one thing, decided when it started, for as long as it lasts.
// hold_target() depends on s->open, so the moment open changes - by any
// route: an exit-hold, an open-hold, or the idle timeout - every button
// still down is holding stale time against a target that no longer means
// what it meant when that time started accumulating. Left alone, that stale
// duration can satisfy the new target on the spot: a neighbour resting on a
// value page reopens the menu in the very tick an exit-hold closed it,
// because its target only became SETTINGS_OPEN_MS partway through that
// tick; push-to-talk held since before the menu existed closes it again on
// the very next tick, because its target only became SETTINGS_EXIT_MS after
// open flipped. Spending every button that is down right now, at every
// place open can change, closes all of those doors at once: from here a
// spent press does nothing until it is released and pressed again. This is
// the one place that rule lives - a fourth way in or out of the menu should
// call this rather than reimplement it.
static void spend_on_open_change(settings_t *s, bool open_before, bool a_down, bool b_down) {
    if (s->open == open_before) return;
    if (a_down) s->a_used = true;
    if (b_down) s->b_used = true;
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
        const bool open_before = s->open;

        if (edges[i] && !*was[i]) {  // press
            *at[i] = now_ms;
            *used[i] = false;
            s->last_input = now_ms;
        } else if (edges[i] && *was[i]) {  // still held
            const uint32_t held = now_ms - *at[i];
            if (target && !*used[i] && held >= target) {
                *used[i] = true;
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
        } else if (!edges[i] && *was[i]) {  // release
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

        // If this button's own action just changed open, the other button
        // may still be mid-iteration (i == 0 runs before i == 1) or may
        // simply be resting on whatever is currently down - either way it
        // needs to be caught before it acts under the new meaning.
        spend_on_open_change(s, open_before, a_down, b_down);
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

    const bool open_before_idle = s->open;
    if (s->open && (uint32_t)(now_ms - s->last_input) > SETTINGS_IDLE_MS) {
        close_and_save(s);
    }
    spend_on_open_change(s, open_before_idle, a_down, b_down);

    return before.open != s->open || before.page != s->page ||
           before.hold_pct != s->hold_pct ||
           before.step[0] != s->step[0] || before.step[1] != s->step[1] ||
           before.step[2] != s->step[2] || before.wifi_requested != s->wifi_requested ||
           before.beep_requested != s->beep_requested ||
           before.save_requested != s->save_requested;
}
