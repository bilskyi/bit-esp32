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
#define ROW_DOTS 56
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

static void clear_px(uint8_t *fb, int x, int y) {
    if (x < 0 || y < 0 || x >= FACE_W || y >= FACE_H) return;
    fb[(y / 8) * FACE_W + x] &= (uint8_t)~(1u << (y % 8));
}

static void fill_rect(uint8_t *fb, int x, int y, int w, int h) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) set_px(fb, x + i, y + j);
}

// The honest tool for "this band is about to get overlaid with something
// self-contained" - used where a pose has already filled the panel and the
// title or the dots are about to be drawn on top of whatever it left there.
static void clear_rect(uint8_t *fb, int x, int y, int w, int h) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) clear_px(fb, x + i, y + j);
}

// Two pixels thick, not one: the layout test samples a row inside the gauge
// to confirm one cell equals one run, and a single-pixel outline puts only
// the left and right edges on that row - two isolated dots, not the one
// block a glance at the panel needs to read as "a cell". A thicker top and
// bottom border keeps the sampled row solid the same way a filled cell is.
#define FRAME_T 2

static void frame_rect(uint8_t *fb, int x, int y, int w, int h) {
    fill_rect(fb, x, y, w, FRAME_T);
    fill_rect(fb, x, y + h - FRAME_T, w, FRAME_T);
    fill_rect(fb, x, y, FRAME_T, h);
    fill_rect(fb, x + w - FRAME_T, y, FRAME_T, h);
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

// Every dot shares the same top edge, current page or not: the layout test
// reads whichever page is showing off row ROW_DOTS alone, and a small dot
// dropped a couple of rows to sit "centred" in the big dot's footprint would
// be invisible on that row - which is exactly what shipped here first, and
// looked like every page but the current one having no dot at all.
static void draw_dots(uint8_t *fb, uint8_t page) {
    const int used = SETTINGS_PAGES * DOT_SIZE + (SETTINGS_PAGES - 1) * (DOT_PITCH - DOT_SIZE);
    const int x0 = (FACE_W - used) / 2;
    for (uint8_t i = 0; i < SETTINGS_PAGES; i++) {
        const int x = x0 + i * DOT_PITCH;
        if (i == page) fill_rect(fb, x, ROW_DOTS, DOT_SIZE, DOT_SIZE);
        else fill_rect(fb, x + (DOT_SIZE - 2) / 2, ROW_DOTS, 2, 2);
    }
}

void settings_screen_render(uint8_t *fb, const settings_t *s) {
    const uint8_t page = (s->page < SETTINGS_PAGES) ? s->page : 0;

    if (page == SETTINGS_PAGE_EYES) {
        // The setting is a look, so the look is the gauge. face_render_pose()
        // clears the buffer itself and fills the middle of the panel; the
        // title and the dots go in the margins it leaves - except some poses
        // reach further than others (Curious and Excited's wider eyes climb
        // as far as the title row, on a 1-bit panel that reads as clutter
        // sitting behind the word). Both margins are cleared before anything
        // is drawn over them, so the overlay never depends on how far a
        // particular pose happened to reach.
        face_render_pose(fb, face_emotion_pose(settings_eyes_emotion(s->step[SETTINGS_PAGE_EYES])));
        clear_rect(fb, 0, ROW_TITLE, FACE_W, SS_GLYPH_H);
        clear_rect(fb, 0, ROW_DOTS, FACE_W, DOT_SIZE);
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
