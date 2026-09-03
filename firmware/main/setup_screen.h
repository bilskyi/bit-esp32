// The provisioning screen: four lines of 5x7 text on the 128x64 panel.
//
// Same rules as face.c - no ESP-IDF header, no float, no allocation - so
// host/setup_screen_test.c can build it and the layout can be checked without
// a board. It renders into a framebuffer it is handed and knows nothing about
// I2C; face_task owns the panel and calls this instead of the eyes while
// provisioning is up.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "face.h"  // FACE_W, FACE_H, FACE_FB_BYTES only

#define SS_GLYPH_W 5
#define SS_GLYPH_H 7
#define SS_ADVANCE 6  // one blank column between glyphs
#define SS_COLS (FACE_W / SS_ADVANCE)  // 21 characters per line

#define SS_LINE_MAX (SS_COLS + 1)

typedef enum {
    SS_STATUS_WAITING = 0,   // access point up, nobody has submitted
    SS_STATUS_TRYING,        // trialling the credentials
    SS_STATUS_CONNECTED,
    SS_STATUS_BAD_PASSWORD,
    SS_STATUS_NOT_FOUND,
    SS_STATUS_TIMED_OUT,
    SS_STATUS_LEAVING,       // the exit gesture is part-way through
    SS_STATUS_FLASHING,      // a firmware file is being written to the flash
    SS_STATUS_COUNT
} ss_status_t;

typedef struct {
    char ap_name[SS_LINE_MAX];  // e.g. "Voice-3f2a"
    char address[SS_LINE_MAX];  // "192.168.4.1"
    char code[SS_LINE_MAX];     // the four-digit unlock code
    ss_status_t status;
} setup_screen_t;

// Clears fb and draws the screen. Text longer than a line is truncated, never
// wrapped and never drawn past the edge.
void setup_screen_render(uint8_t *fb, const setup_screen_t *s);

// Exposed for the host test, and useful on its own.
void ss_draw_text(uint8_t *fb, int x, int y, const char *text);
const char *ss_status_text(ss_status_t status);
