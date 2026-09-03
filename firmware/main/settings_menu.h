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
