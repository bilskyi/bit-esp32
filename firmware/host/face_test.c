// Invariants for face.c, run on the laptop.
//
// The panel spent its first days in a box, so these assertions were the only
// thing standing between "the animation is written" and "the animation works".
// They are still the cheaper place to catch a regression: a wrong sign in a
// lid slant is one line here and a reflash plus a squint at a 0.96" screen
// there.
//
//   cd firmware/host && make test

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/face.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        checks++;                                              \
        if (!(cond)) {                                         \
            failures++;                                        \
            printf("  FAIL %s:%d  ", __func__, __LINE__);       \
            printf(__VA_ARGS__);                               \
            printf("\n");                                      \
        }                                                      \
    } while (0)

#define TICK_MS 40  // the 25 fps the firmware runs at

// ------------------------------------------------------------------ helpers

static int lit_count(const uint8_t *fb) {
    int n = 0;
    for (int i = 0; i < FACE_FB_BYTES; i++) {
        uint8_t b = fb[i];
        while (b) { n += b & 1; b >>= 1; }
    }
    return n;
}

static bool lit_at(const uint8_t *fb, int x, int y) {
    if (x < 0 || y < 0 || x >= FACE_W || y >= FACE_H) return false;
    return (fb[(y / 8) * FACE_W + x] >> (y % 8)) & 1;
}

static uint32_t since_ms(uint32_t now, uint32_t then) { return now - then; }

typedef struct { int x0, y0, x1, y1; bool any; } bbox_t;

static bbox_t bounds(const uint8_t *fb) {
    bbox_t b = {FACE_W, FACE_H, -1, -1, false};
    for (int y = 0; y < FACE_H; y++) {
        for (int x = 0; x < FACE_W; x++) {
            if (!lit_at(fb, x, y)) continue;
            b.any = true;
            if (x < b.x0) b.x0 = x;
            if (x > b.x1) b.x1 = x;
            if (y < b.y0) b.y0 = y;
            if (y > b.y1) b.y1 = y;
        }
    }
    return b;
}

static int hamming(const uint8_t *a, const uint8_t *b) {
    int n = 0;
    for (int i = 0; i < FACE_FB_BYTES; i++) {
        uint8_t d = a[i] ^ b[i];
        while (d) { n += d & 1; d >>= 1; }
    }
    return n;
}

// Get a fresh face through the power-on sequence, which owns the display until
// it hands over. Everything below except the boot tests themselves is about
// behaviour afterwards, and the device gets there the same way.
static void boot_through(face_t *f) {
    uint32_t t = f->now;
    face_boot_stage(f, FACE_BOOT_LINK, t);
    while (face_is_booting(f) && since_ms(t, f->boot_start) < 20000u) {
        t += TICK_MS;
        face_tick(f, t);
    }
}

static void init_ready(face_t *f, uint32_t now_ms) {
    face_init(f, now_ms);
    boot_through(f);
}

// Run a face in one state for a while, calling back on every frame.
static void run(face_t *f, face_state_t st, uint32_t ms, void (*each)(face_t *, uint32_t)) {
    uint32_t t = f->now;
    face_set_state(f, st, t);
    const uint32_t end = t + ms;
    while (t < end) {
        t += TICK_MS;
        face_tick(f, t);
        if (each) each(f, t);
    }
}

// ------------------------------------------------------- every emotion draws

static void test_emotions_render(void) {
    uint8_t frames[FACE_EMO_COUNT][FACE_FB_BYTES];

    for (int e = 0; e < FACE_EMO_COUNT; e++) {
        const face_pose_t *p = face_emotion_pose((face_emotion_t)e);
        CHECK(p != NULL, "no pose for %s", face_emotion_name((face_emotion_t)e));
        memset(frames[e], 0, FACE_FB_BYTES);
        face_render_pose(frames[e], p);

        const int n = lit_count(frames[e]);
        CHECK(n > 200, "%s lights only %d pixels", face_emotion_name((face_emotion_t)e), n);

        // Both eyes present, and neither has escaped its own side or the panel.
        const bbox_t b = bounds(frames[e]);
        CHECK(b.any, "%s is blank", face_emotion_name((face_emotion_t)e));
        CHECK(b.x0 >= 1 && b.x1 <= FACE_W - 2,
              "%s touches the left/right edge: x %d..%d",
              face_emotion_name((face_emotion_t)e), b.x0, b.x1);
        CHECK(b.y0 >= 1 && b.y1 <= FACE_H - 2,
              "%s touches the top/bottom edge: y %d..%d",
              face_emotion_name((face_emotion_t)e), b.y0, b.y1);

        // A gap down the middle: two eyes, not one blob.
        bool gap = false;
        for (int x = FACE_W / 2 - 4; x <= FACE_W / 2 + 4 && !gap; x++) {
            bool col = false;
            for (int y = 0; y < FACE_H; y++) col |= lit_at(frames[e], x, y);
            if (!col) gap = true;
        }
        CHECK(gap, "%s has no gap between the eyes", face_emotion_name((face_emotion_t)e));

        // A pupil: some cleared pixel enclosed by lit ones on the same row.
        bool hole = false;
        for (int y = 8; y < FACE_H - 8 && !hole; y++) {
            bool seen_lit = false, seen_gap_after_lit = false;
            for (int x = 8; x < FACE_W / 2; x++) {
                const bool on = lit_at(frames[e], x, y);
                if (on && seen_gap_after_lit) { hole = true; break; }
                if (on) seen_lit = true;
                else if (seen_lit) seen_gap_after_lit = true;
            }
        }
        CHECK(hole, "%s shows no pupil", face_emotion_name((face_emotion_t)e));
    }

    // Every emotion must be visibly different from every other one, or the
    // server is sending a distinction the user cannot see.
    for (int a = 0; a < FACE_EMO_COUNT; a++) {
        for (int b = a + 1; b < FACE_EMO_COUNT; b++) {
            const int d = hamming(frames[a], frames[b]);
            CHECK(d > 40, "%s and %s differ by only %d pixels",
                  face_emotion_name((face_emotion_t)a),
                  face_emotion_name((face_emotion_t)b), d);
        }
    }
}

// --------------------------------------------------- sad and angry mirror it

static void test_slant_direction(void) {
    // Angry drops the inner lid, sad the outer one. Getting the sign wrong
    // produces two expressions that each read as the other, which is exactly
    // the sort of thing that survives a glance at the real panel.
    uint8_t angry[FACE_FB_BYTES] = {0}, sad[FACE_FB_BYTES] = {0};
    face_render_pose(angry, face_emotion_pose(FACE_EMO_ANNOYED));
    face_render_pose(sad, face_emotion_pose(FACE_EMO_SAD));

    // Topmost lit row on the outer and inner side of the left eye.
    // Left eye spans roughly x 11..57; inner side is the right half.
    int outer_top_angry = FACE_H, inner_top_angry = FACE_H;
    int outer_top_sad = FACE_H, inner_top_sad = FACE_H;
    for (int y = 0; y < FACE_H; y++) {
        for (int x = 12; x < 24; x++) {
            if (lit_at(angry, x, y) && y < outer_top_angry) outer_top_angry = y;
            if (lit_at(sad, x, y) && y < outer_top_sad) outer_top_sad = y;
        }
        for (int x = 44; x < 56; x++) {
            if (lit_at(angry, x, y) && y < inner_top_angry) inner_top_angry = y;
            if (lit_at(sad, x, y) && y < inner_top_sad) inner_top_sad = y;
        }
    }

    CHECK(inner_top_angry > outer_top_angry + 3,
          "annoyed should drop the inner lid: inner top %d, outer top %d",
          inner_top_angry, outer_top_angry);
    CHECK(outer_top_sad > inner_top_sad + 3,
          "sad should drop the outer lid: outer top %d, inner top %d",
          outer_top_sad, inner_top_sad);
}

// ------------------------------------------------ the lower lid clears the pupil

static void test_lower_lid_never_opens_the_pupil(void) {
    // A raised lower lid that cuts into the pupil destroys it: the pupil's
    // black joins the black below the eye and the whole shape stops reading as
    // an eye at all - it becomes a helmet. The first version of `happy` did
    // exactly this and looked like a scowl.
    //
    // An upper lid over the pupil is fine and normal, because the top of the
    // eye has gone with it. Only the lower lid has to keep its distance.
    for (int e = 0; e < FACE_EMO_COUNT; e++) {
        const face_pose_t *p = face_emotion_pose((face_emotion_t)e);
        if (p->happy <= 2) continue;

        uint8_t fb[FACE_FB_BYTES] = {0};
        face_render_pose(fb, p);

        // Walk down the left eye's centre column. Below the pupil there must
        // be lit pixels again before the eye ends.
        const int x = 34;
        int runs = 0;
        bool prev = false;
        for (int y = 0; y < FACE_H; y++) {
            const bool on = lit_at(fb, x, y);
            if (on && !prev) runs++;
            prev = on;
        }
        CHECK(runs >= 2, "%s lets the lower lid cut the pupil open (%d lit runs)",
              face_emotion_name((face_emotion_t)e), runs);
    }
}

// ------------------------------------------------------- gaze moves the pupil

static void test_gaze_moves_pupil(void) {
    face_pose_t p = *face_emotion_pose(FACE_EMO_NEUTRAL);
    uint8_t left[FACE_FB_BYTES] = {0}, right[FACE_FB_BYTES] = {0};

    p.gx = -256;
    face_render_pose(left, &p);
    p.gx = 256;
    face_render_pose(right, &p);

    // Find the pupil's horizontal centre in the left eye by locating the
    // cleared run inside the lit body on the eye's middle row.
    int cen[2];
    const uint8_t *fbs[2] = {left, right};
    for (int i = 0; i < 2; i++) {
        int x0 = -1, x1 = -1;
        bool seen_lit = false;
        for (int x = 8; x < FACE_W / 2; x++) {
            const bool on = lit_at(fbs[i], x, FACE_H / 2);
            if (on) { if (x0 >= 0 && x1 < 0) x1 = x; seen_lit = true; }
            else if (seen_lit && x0 < 0) x0 = x;
        }
        CHECK(x0 > 0 && x1 > x0, "no pupil run found in frame %d", i);
        cen[i] = (x0 + x1) / 2;
    }
    CHECK(cen[1] > cen[0] + 6,
          "gaze right should move the pupil right: %d then %d", cen[0], cen[1]);
}

// ------------------------------------------------------------- powering on

static void test_boot_starts_as_a_bar_not_as_eyes(void) {
    // The bar across the whole panel is the one shape in the vocabulary that
    // cannot be mistaken for a mood, which is the entire reason the sequence
    // opens with it: "power came on" has to be unambiguous from the first
    // frame. Two slits would be indistinguishable from "no connection".
    face_t f;
    face_init(&f, 1000);

    int widest = 0;
    uint32_t t = 1000;
    for (int i = 0; i < 12; i++) {  // the first 480 ms
        t += TICK_MS;
        face_tick(&f, t);
        const bbox_t b = bounds(f.fb);
        if (b.any && b.x1 - b.x0 > widest) widest = b.x1 - b.x0;
    }
    CHECK(widest > 110, "the power-on bar is only %d px wide", widest);

    // ...and it is a bar, not eyes: no gap down the middle while it is one.
    face_t g;
    face_init(&g, 1000);
    uint32_t gt = 1000;
    bool merged = false;
    for (int i = 0; i < 12; i++) {
        gt += TICK_MS;
        face_tick(&g, gt);
        bool col = false;
        for (int y = 0; y < FACE_H; y++) col |= lit_at(g.fb, FACE_W / 2, y);
        if (col) merged = true;
    }
    CHECK(merged, "the two halves never met in the middle");
}

static void test_boot_never_leaves_the_panel_dark(void) {
    // Same rule as everywhere else: a blank 128x64 reads as broken hardware,
    // and the very first thing a user ever sees must not read that way.
    face_t f;
    face_init(&f, 1000);
    uint32_t t = 1000, dark = 0, worst = 0;
    for (int i = 0; i < 250; i++) {
        t += TICK_MS;
        face_tick(&f, t);
        if (lit_count(f.fb) == 0) {
            dark += TICK_MS;
            if (dark > worst) worst = dark;
        } else {
            dark = 0;
        }
    }
    CHECK(worst <= 80, "the panel was dark for %u ms during power-on", worst);
}

static void test_boot_holds_where_it_got_stuck(void) {
    // The whole point of tying the animation to progress. WiFi up but no
    // server has to look different from both "still connecting" and "ready",
    // or the animation is decoration and the five seconds tell you nothing.
    face_t panel, wifi, ready;
    face_init(&panel, 1000);
    face_init(&wifi, 1000);
    face_init(&ready, 1000);

    uint32_t t = 1000;
    face_boot_stage(&wifi, FACE_BOOT_WIFI, t);
    face_boot_stage(&ready, FACE_BOOT_LINK, t);
    for (int i = 0; i < 250; i++) {  // ten seconds, well past the flourish
        t += TICK_MS;
        face_tick(&panel, t);
        face_tick(&wifi, t);
        face_tick(&ready, t);
    }

    const int p = lit_count(panel.fb), w = lit_count(wifi.fb), r = lit_count(ready.fb);
    CHECK(p < w, "stuck on the panel should show less eye than stuck on WiFi: %d vs %d", p, w);
    CHECK(w < r, "stuck on WiFi should show less eye than ready: %d vs %d", w, r);
    CHECK(r - p > 400, "the three stages are barely distinguishable: %d..%d", p, r);

    // Stuck means stuck: it must not creep open on its own.
    const int before = lit_count(wifi.fb);
    for (int i = 0; i < 250; i++) { t += TICK_MS; face_tick(&wifi, t); }
    const int after = lit_count(wifi.fb);
    const int drift = after > before ? after - before : before - after;
    CHECK(drift < 260, "a stuck boot drifted by %d pixels over ten seconds", drift);
}

static void test_boot_ends_and_hands_over(void) {
    face_t f;
    face_init(&f, 1000);
    CHECK(face_is_booting(&f), "should be booting straight after init");

    uint32_t t = 1000;
    face_boot_stage(&f, FACE_BOOT_LINK, t);
    for (int i = 0; i < 120; i++) { t += TICK_MS; face_tick(&f, t); }
    CHECK(!face_is_booting(&f), "the power-on sequence never finished");

    // And the ordinary machinery is alive again: idle has to start blinking.
    int lo = 1 << 30, hi = 0;
    face_set_state(&f, FACE_ST_IDLE, t);
    for (int i = 0; i < 400; i++) {
        t += TICK_MS;
        face_tick(&f, t);
        const int n = lit_count(f.fb);
        if (n < lo) lo = n;
        if (n > hi) hi = n;
    }
    CHECK(lo < hi / 4, "no blinking after handover: lit ranged %d..%d", lo, hi);
}

static void test_boot_does_not_restart_when_the_socket_drops(void) {
    // A link that dies later is the ordinary offline state. Replaying the
    // power-on animation would claim the device had rebooted when it had not.
    face_t f;
    init_ready(&f, 1000);
    CHECK(!face_is_booting(&f), "still booting after init_ready");

    uint32_t t = f.now;
    face_set_state(&f, FACE_ST_OFFLINE, t);
    for (int i = 0; i < 200; i++) { t += TICK_MS; face_tick(&f, t); }
    CHECK(!face_is_booting(&f), "going offline restarted the power-on sequence");

    // And the stage cannot be wound back either.
    face_boot_stage(&f, FACE_BOOT_PANEL, t);
    CHECK(!face_is_booting(&f), "the boot stage went backwards");
}

// -------------------------------------------------------- where the pupil is

// The centroid of the enclosed dark pixels in the left eye, relative to the
// centre of that eye's lit bounding box. Enclosed means "has a lit pixel above
// and below it in the same column", which finds the pupil without caring what
// the lids are doing to the outline.
static bool pupil_offset(const uint8_t *fb, double *dx, double *dy, int *height) {
    int bx0 = FACE_W, bx1 = -1, by0 = FACE_H, by1 = -1;
    for (int y = 0; y < FACE_H; y++) {
        for (int x = 2; x < FACE_W / 2; x++) {
            if (!lit_at(fb, x, y)) continue;
            if (x < bx0) bx0 = x;
            if (x > bx1) bx1 = x;
            if (y < by0) by0 = y;
            if (y > by1) by1 = y;
        }
    }
    if (bx1 < 0) return false;
    *height = by1 - by0 + 1;

    double sx = 0, sy = 0;
    int n = 0;
    for (int x = bx0; x <= bx1; x++) {
        int top = -1, bot = -1;
        for (int y = by0; y <= by1; y++) {
            if (!lit_at(fb, x, y)) continue;
            if (top < 0) top = y;
            bot = y;
        }
        if (top < 0) continue;
        for (int y = top + 1; y < bot; y++) {
            if (lit_at(fb, x, y)) continue;
            sx += x;
            sy += y;
            n++;
        }
    }
    if (n < 8) return false;
    *dx = sx / n - (bx0 + bx1) / 2.0;
    *dy = sy / n - (by0 + by1) / 2.0;
    return true;
}

typedef struct {
    double held_up;   // fraction of frames with the gaze clearly raised
    double reach;     // furthest the pupil gets from centre, sideways
    double height;    // mean height of the lit eye
} gaze_stats_t;

static gaze_stats_t survey(face_state_t st, uint32_t ms) {
    face_t f;
    init_ready(&f, 5000);
    // Carry on from where the boot sequence left the clock, not from the
    // literal it started at: going backwards hands face_tick a negative dt.
    uint32_t t = f.now;
    face_set_state(&f, st, t);
    gaze_stats_t s = {0, 0, 0};
    int n = 0, held = 0;
    double sum_h = 0;

    for (uint32_t e = 0; e < ms; e += TICK_MS) {
        t += TICK_MS;
        face_tick(&f, t);
        double ox, oy;
        int eh;
        if (!pupil_offset(f.fb, &ox, &oy, &eh)) continue;
        n++;
        sum_h += eh;
        if (oy < -3.0) held++;
        const double a = ox < 0 ? -ox : ox;
        if (a > s.reach) s.reach = a;
    }
    if (n) {
        s.held_up = (double)held / n;
        s.height = sum_h / n;
    }
    return s;
}

static void test_thinking_is_unmistakable(void) {
    // Measured before this was tuned: thinking already held the gaze up 82% of
    // the time and swung the pupil 10 px sideways, and it still did not read
    // as thinking. The reason was the silhouette - the eye was 46.3 px tall
    // against idle's 45.7, so from across the room the two states were the
    // same shape and only the pupil differed. Shape is read first.
    const gaze_stats_t idle = survey(FACE_ST_IDLE, 12000);
    const gaze_stats_t think = survey(FACE_ST_THINKING, 12000);

    CHECK(think.height < idle.height - 5.0,
          "thinking must narrow the eyes, or it is idle with a moving pupil: "
          "%.1f px vs idle %.1f px", think.height, idle.height);
    CHECK(think.held_up > 0.70,
          "thinking should hold the gaze up, not bob: %.0f%% of frames",
          think.held_up * 100);
    CHECK(idle.held_up < 0.20,
          "idle should not be staring upward: %.0f%% of frames",
          idle.held_up * 100);
    // Deliberately an absolute floor rather than a comparison against idle.
    // Measured, idle reaches 6.5 px sideways and thinking 9.3, which is not a
    // margin worth asserting on - idle wanders on purpose. Sideways travel
    // turned out to be a poor discriminator; height and the held gaze are the
    // ones that carry the difference, and they are checked above. This only
    // guards against the glance being removed altogether.
    CHECK(think.reach > 7.0,
          "thinking stopped looking aside: %.1f px", think.reach);
}

static void test_thinking_looks_different_from_listening(void) {
    // Both are "waiting" states and both follow a button press, so if they
    // look alike the face is telling the user nothing.
    face_t a, b;
    init_ready(&a, 5000);
    init_ready(&b, 5000);
    uint32_t t = a.now;
    face_set_state(&a, FACE_ST_LISTENING, t);
    face_set_state(&b, FACE_ST_THINKING, t);
    int worst = 1 << 30;
    for (int i = 0; i < 200; i++) {
        t += TICK_MS;
        face_tick(&a, t);
        face_tick(&b, t);
        if (i < 40) continue;  // let both settle
        const int d = hamming(a.fb, b.fb);
        if (d < worst) worst = d;
    }
    CHECK(worst > 90, "listening and thinking look alike: %d pixels apart at closest",
          worst);
}

// ------------------------------------------------------------------- blinking

static int blank_run_ms = 0, worst_blank_run_ms = 0;
static int min_lit = 1 << 30, max_lit = 0;

static void watch(face_t *f, uint32_t now) {
    (void)now;
    const int n = lit_count(f->fb);
    if (n < min_lit) min_lit = n;
    if (n > max_lit) max_lit = n;
    if (n == 0) {
        blank_run_ms += TICK_MS;
        if (blank_run_ms > worst_blank_run_ms) worst_blank_run_ms = blank_run_ms;
    } else {
        blank_run_ms = 0;
    }
}

static void test_blinks_and_never_dark(void) {
    // Every state, thirty seconds each. The panel may go dark for a blink and
    // for nothing else: a 128x64 display showing nothing does not read as
    // "idle", it reads as "broken", and this project has already lost days to
    // symptoms that looked like dead hardware.
    for (int s = 0; s < FACE_ST_COUNT; s++) {
        face_t f;
        init_ready(&f, 1000);
        min_lit = 1 << 30;
        max_lit = 0;
        blank_run_ms = worst_blank_run_ms = 0;

        run(&f, (face_state_t)s, 30000, watch);

        CHECK(worst_blank_run_ms <= 200,
              "%s went dark for %d ms", face_state_name((face_state_t)s),
              worst_blank_run_ms);
        CHECK(max_lit > 200, "%s never lights up (max %d)",
              face_state_name((face_state_t)s), max_lit);

        // Idle must actually blink: something has to close the eyes.
        if (s == FACE_ST_IDLE) {
            CHECK(min_lit < max_lit / 4,
                  "idle never blinks: lit ranged %d..%d", min_lit, max_lit);
        }
    }
}

static void test_offline_keeps_a_sliver(void) {
    // Asleep is drawn as a thin bar rather than an empty screen, for the same
    // reason as above.
    face_t f;
    init_ready(&f, 1000);
    run(&f, FACE_ST_OFFLINE, 6000, NULL);

    const bbox_t b = bounds(f.fb);
    CHECK(b.any, "offline is blank");
    CHECK(b.y1 - b.y0 <= 14, "offline lids are not shut: rows %d..%d", b.y0, b.y1);
    CHECK(lit_count(f.fb) > 30, "offline sliver is too faint: %d pixels",
          lit_count(f.fb));
}

static void test_press_while_offline_stirs(void) {
    // Pressing the button with the socket down should crack the lids and let
    // them fall again: the device has heard you and can do nothing about it.
    face_t f;
    init_ready(&f, 1000);
    run(&f, FACE_ST_OFFLINE, 6000, NULL);
    const int shut = lit_count(f.fb);

    uint32_t t = f.now;
    face_set_button(&f, true, t);
    int peak = shut;
    for (int i = 0; i < 8; i++) {
        t += TICK_MS;
        face_tick(&f, t);
        const int n = lit_count(f.fb);
        if (n > peak) peak = n;
    }
    CHECK(peak > shut + 60, "a press while offline did not stir the eyes: %d then %d",
          shut, peak);

    face_set_button(&f, false, t);
    for (int i = 0; i < 40; i++) { t += TICK_MS; face_tick(&f, t); }
    CHECK(lit_count(&f.fb[0]) < peak, "the eyes never closed again after stirring");
}

// -------------------------------------------------------------- listening etc

static void test_listening_opens_wider_than_idle(void) {
    face_t a, b;
    init_ready(&a, 1000);
    init_ready(&b, 1000);

    // Settle both, then compare at a moment neither is blinking.
    run(&a, FACE_ST_IDLE, 2000, NULL);
    run(&b, FACE_ST_LISTENING, 2000, NULL);

    // Take the widest frame each reaches over a second, to sidestep blinks.
    int wa = 0, wb = 0;
    uint32_t t = a.now;
    for (int i = 0; i < 25; i++) {
        t += TICK_MS;
        face_tick(&a, t);
        face_tick(&b, t);
        const int na = lit_count(a.fb), nb = lit_count(b.fb);
        if (na > wa) wa = na;
        if (nb > wb) wb = nb;
    }
    CHECK(wb > wa, "listening should open wider than idle: %d vs %d", wa, wb);
}

static void test_startle_is_visible_and_temporary(void) {
    face_t f;
    init_ready(&f, 1000);
    run(&f, FACE_ST_SPEAKING, 3000, NULL);
    const int calm = lit_count(f.fb);

    uint32_t t = f.now;
    face_startle(&f, t);
    int peak = 0;
    for (int i = 0; i < 8; i++) {  // 320 ms
        t += TICK_MS;
        face_tick(&f, t);
        const int n = lit_count(f.fb);
        if (n > peak) peak = n;
    }
    CHECK(peak > calm + 100, "startle is not visible: %d then %d", calm, peak);

    for (int i = 0; i < 50; i++) { t += TICK_MS; face_tick(&f, t); }
    CHECK(lit_count(f.fb) < peak, "startle never wore off");
}

static void test_idle_falls_asleep(void) {
    // Left alone, the emotion decays to neutral, then sleepy, then shut.
    face_t f;
    init_ready(&f, 1000);
    face_set_emotion(&f, FACE_EMO_EXCITED, 1000);
    run(&f, FACE_ST_IDLE, 4000, NULL);
    const bbox_t awake = bounds(f.fb);

    run(&f, FACE_ST_IDLE, 200000, NULL);  // well past the asleep threshold
    const bbox_t asleep = bounds(f.fb);

    CHECK(asleep.y1 - asleep.y0 < awake.y1 - awake.y0,
          "idling for three minutes did not close the eyes: %d rows then %d",
          awake.y1 - awake.y0, asleep.y1 - asleep.y0);
    CHECK(asleep.any, "asleep is blank");

    // ...and a press wakes them.
    uint32_t t = f.now;
    face_set_button(&f, true, t);
    for (int i = 0; i < 20; i++) { t += TICK_MS; face_tick(&f, t); }
    const bbox_t woken = bounds(f.fb);
    CHECK(woken.y1 - woken.y0 > asleep.y1 - asleep.y0 + 8,
          "a press did not wake the eyes: %d rows then %d",
          asleep.y1 - asleep.y0, woken.y1 - woken.y0);
}

// ---------------------------------------------------------------- loudness

static void test_energy_normalises_both_levels(void) {
    // The microphone sits near -46 dBFS and a TTS reply near full scale. Both
    // have to drive the eyes across the same range, or one direction barely
    // moves and the other pins.
    const uint16_t levels[] = {150, 12000};
    for (int i = 0; i < 2; i++) {
        face_t f;
        init_ready(&f, 1000);
        for (int n = 0; n < 200; n++) face_feed_energy(&f, levels[i]);
        CHECK(f.e_slow > 200, "rms %u only reached e_slow %u", levels[i], f.e_slow);
    }

    // And silence has to come back down.
    face_t f;
    init_ready(&f, 1000);
    for (int n = 0; n < 200; n++) face_feed_energy(&f, 8000);
    for (int n = 0; n < 400; n++) face_feed_energy(&f, 0);
    CHECK(f.e_fast < 20, "silence left e_fast at %u", f.e_fast);
    CHECK(f.e_slow < 20, "silence left e_slow at %u", f.e_slow);
}

static void test_energy_changes_the_speaking_face(void) {
    face_t loud, quiet;
    init_ready(&loud, 1000);
    init_ready(&quiet, 1000);
    uint32_t t = loud.now;
    face_set_state(&loud, FACE_ST_SPEAKING, t);
    face_set_state(&quiet, FACE_ST_SPEAKING, t);
    int diff = 0;
    for (int i = 0; i < 60; i++) {
        t += TICK_MS;
        face_feed_energy(&loud, 9000);
        face_feed_energy(&quiet, 0);
        face_tick(&loud, t);
        face_tick(&quiet, t);
        const int d = hamming(loud.fb, quiet.fb);
        if (d > diff) diff = d;
    }
    CHECK(diff > 60, "loudness barely moves the eyes: %d pixels", diff);
}

static void test_energy_decays_without_feeding(void) {
    // Playback stops without a final "silence" block, so the eyes have to
    // settle on their own or they freeze mid-syllable.
    face_t f;
    init_ready(&f, 1000);
    uint32_t t = f.now;
    face_set_state(&f, FACE_ST_SPEAKING, t);
    for (int i = 0; i < 40; i++) { face_feed_energy(&f, 9000); t += TICK_MS; face_tick(&f, t); }
    CHECK(f.e_fast > 100, "energy never rose: %u", f.e_fast);
    for (int i = 0; i < 60; i++) { t += TICK_MS; face_tick(&f, t); }
    CHECK(f.e_fast < 20, "energy did not decay when the feed stopped: %u", f.e_fast);
}

// ------------------------------------------------------------ protocol names

static void test_emotion_scan(void) {
    for (int e = 0; e < FACE_EMO_COUNT; e++) {
        char frame[96];
        const int n = snprintf(frame, sizeof(frame), "{\"type\":\"emotion\",\"value\":\"%s\"}",
                               face_emotion_name((face_emotion_t)e));
        CHECK(face_emotion_scan(frame, (size_t)n) == (face_emotion_t)e,
              "did not recognise %s", face_emotion_name((face_emotion_t)e));
    }

    const char *no[] = {
        "{\"type\":\"state\",\"value\":\"speaking\"}",
        "{\"type\":\"state\",\"value\":\"thinking\"}",
        "{\"type\":\"state\",\"value\":\"listening\"}",
        "{\"type\":\"done\"}",
        "{\"type\":\"emotion\",\"value\":\"smug\"}",
        "",
    };
    for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++) {
        CHECK(face_emotion_scan(no[i], strlen(no[i])) == FACE_EMO_COUNT,
              "false positive on %s", no[i]);
    }

    // A truncated frame must not read past the length it was given.
    const char *cut = "{\"type\":\"emotion\",\"value\":\"curious\"}";
    CHECK(face_emotion_scan(cut, 28) == FACE_EMO_COUNT,
          "matched an emotion that was cut off");

    for (int e = 0; e < FACE_EMO_COUNT; e++) {
        CHECK(face_emotion_name((face_emotion_t)e) != NULL, "emotion %d has no name", e);
    }
    for (int s = 0; s < FACE_ST_COUNT; s++) {
        CHECK(face_state_name((face_state_t)s) != NULL, "state %d has no name", s);
    }
}

// -------------------------------------------------------------- determinism

static void test_deterministic(void) {
    // The preview on the laptop and the panel on the desk have to agree, so
    // there is no wall clock and no rand() anywhere in here.
    face_t a, b;
    face_init(&a, 7);
    face_init(&b, 7);
    uint32_t t = 7;
    for (int i = 0; i < 500; i++) {
        t += TICK_MS;
        if (i == 40) { face_set_state(&a, FACE_ST_SPEAKING, t); face_set_state(&b, FACE_ST_SPEAKING, t); }
        if (i == 120) { face_startle(&a, t); face_startle(&b, t); }
        face_feed_energy(&a, (uint16_t)(i * 37 % 6000));
        face_feed_energy(&b, (uint16_t)(i * 37 % 6000));
        face_tick(&a, t);
        face_tick(&b, t);
    }
    CHECK(memcmp(a.fb, b.fb, FACE_FB_BYTES) == 0, "two identical runs diverged");
}

static void test_survives_a_hostile_clock(void) {
    // The caller's clock is FreeRTOS ticks. A stalled task, or the 32-bit
    // wrap after 49 days, must not wedge the animation or hang a loop.
    face_t f;
    face_init(&f, 0xFFFFFF00u);
    uint32_t t = 0xFFFFFF00u;
    for (int i = 0; i < 300; i++) {
        t += 40;  // wraps through zero partway
        face_tick(&f, t);
        CHECK(lit_count(f.fb) >= 0, "unreachable");
    }
    CHECK(lit_count(f.fb) > 0, "the face died across a clock wrap");

    // A single enormous jump between ticks, as after a long stall.
    face_t g;
    face_init(&g, 100);
    face_tick(&g, 100);
    face_tick(&g, 100 + 600000);
    CHECK(lit_count(g.fb) > 0, "a ten minute stall blanked the face");
    face_tick(&g, 100 + 600000);  // no time passing at all
    CHECK(lit_count(g.fb) > 0, "a zero-length tick blanked the face");
}

// -------------------------------------------------------------------- main

int main(void) {
    struct { const char *name; void (*fn)(void); } tests[] = {
        {"every emotion renders two distinct eyes", test_emotions_render},
        {"annoyed and sad slant opposite ways", test_slant_direction},
        {"a raised lower lid never cuts the pupil open", test_lower_lid_never_opens_the_pupil},
        {"gaze moves the pupil", test_gaze_moves_pupil},
        {"power-on opens as a bar, not as eyes", test_boot_starts_as_a_bar_not_as_eyes},
        {"power-on never leaves the panel dark", test_boot_never_leaves_the_panel_dark},
        {"power-on holds where it got stuck", test_boot_holds_where_it_got_stuck},
        {"power-on ends and hands over", test_boot_ends_and_hands_over},
        {"a dropped socket does not replay power-on", test_boot_does_not_restart_when_the_socket_drops},
        {"thinking is unmistakable", test_thinking_is_unmistakable},
        {"thinking and listening are not twins", test_thinking_looks_different_from_listening},
        {"blinks happen and the panel is never dark", test_blinks_and_never_dark},
        {"offline keeps a visible sliver", test_offline_keeps_a_sliver},
        {"a press while offline stirs the eyes", test_press_while_offline_stirs},
        {"listening opens wider than idle", test_listening_opens_wider_than_idle},
        {"startle is visible and temporary", test_startle_is_visible_and_temporary},
        {"idling falls asleep and a press wakes it", test_idle_falls_asleep},
        {"loudness normalises from either level", test_energy_normalises_both_levels},
        {"loudness changes the speaking face", test_energy_changes_the_speaking_face},
        {"loudness decays when the feed stops", test_energy_decays_without_feeding},
        {"the emotion frame is parsed, and nothing else is", test_emotion_scan},
        {"the animation is deterministic", test_deterministic},
        {"a wrapping or stalled clock is survivable", test_survives_a_hostile_clock},
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        const int before = failures;
        tests[i].fn();
        printf("%s %s\n", failures == before ? "ok  " : "FAIL", tests[i].name);
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
