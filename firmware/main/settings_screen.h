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
