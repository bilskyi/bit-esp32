// The eyes. See face.h for why there is no ESP-IDF, no libm and no float here.
//
// Everything is drawn as horizontal spans. For each row a shape yields one
// x0..x1 range, and every pixel in a span shares a page and a bit mask, so the
// inner loop is `row[x] |= mask`. That is both faster and simpler than testing
// every pixel against a distance function, and the shapes stay smooth under
// sub-pixel animation because the geometry is Q8 and only the span ends round.

#include "face.h"

#include <string.h>

#define FP 8
#define ONE 256

// ------------------------------------------------------------------- integers

// Floor division that behaves the same for negative numerators. Plain `/`
// truncates toward zero and `>>` on a negative value is implementation
// defined; a pixel at x = -1 has to round the same way as one at x = 41.
static int32_t floor_div(int32_t a, int32_t b) {
    int32_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

// Pixel centres sit at (index << FP) + ONE/2.
static int px_first(int32_t q) { return (int)(-floor_div(-(q - ONE / 2), ONE)); }
static int px_last(int32_t q) { return (int)floor_div(q - ONE / 2, ONE); }

static uint32_t isqrt32(uint32_t n) {
    uint32_t res = 0, bit = 1u << 30;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= res + bit) {
            n -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

// A parabola standing in for a sine: phase is Q16 over one period, the result
// is Q8 in [-256, 256]. It is about 5% off at the shoulders, which nothing here
// can see - it drives breathing and eye drift, not audio.
static int32_t fsin(uint32_t phase) {
    uint32_t p = phase & 0xFFFFu;
    int32_t sign = 1;
    if (p >= 32768u) {
        p -= 32768u;
        sign = -1;
    }
    const int32_t t = (int32_t)p;                         // 0..32767 covers 0..pi
    int32_t y = (4 * t * (32768 - t)) >> 15;              // 0..32768
    y = (y * ONE) >> 15;                                  // 0..256
    return sign * y;
}

static uint32_t phase_of(uint32_t ms, uint32_t period) {
    const uint32_t m = ms % period;
    return (uint32_t)(((uint64_t)m << 16) / period);
}

// 3t^2 - 2t^3 over 0..256, for blinks and saccades.
static int32_t smoothstep(uint32_t elapsed, uint32_t duration) {
    if (duration == 0 || elapsed >= duration) return ONE;
    const int32_t t = (int32_t)((elapsed * (uint32_t)ONE) / duration);
    return (t * t * (3 * ONE - 2 * t)) >> 16;
}

// The caller's clock is FreeRTOS ticks, which wrap after 49 days. Comparing
// them as unsigned differences is wrap-safe; comparing them directly is not.
static inline bool due(uint32_t now, uint32_t when) { return (int32_t)(now - when) >= 0; }
static inline uint32_t since(uint32_t now, uint32_t then) { return now - then; }

static uint32_t rnd(face_t *f) {
    uint32_t x = f->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    f->rng = x;
    return x;
}

// Interpolate a to b, k in 0..256.
static int32_t lerp_q8(int32_t a, int32_t b, int32_t k) {
    return a + (((b - a) * k) >> FP);
}

static int16_t clamp16(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (int16_t)v;
}

// ---------------------------------------------------------------- framebuffer

static void fb_span(uint8_t *fb, int y, int x0, int x1, int v) {
    if (y < 0 || y >= FACE_H) return;
    if (x0 < 0) x0 = 0;
    if (x1 > FACE_W - 1) x1 = FACE_W - 1;
    if (x0 > x1) return;

    uint8_t *row = fb + (y >> 3) * FACE_W;
    const uint8_t m = (uint8_t)(1u << (y & 7));
    if (v) {
        for (int x = x0; x <= x1; x++) row[x] |= m;
    } else {
        const uint8_t n = (uint8_t)~m;
        for (int x = x0; x <= x1; x++) row[x] &= n;
    }
}

// A filled rounded rectangle. x, y is the top-left corner; everything is Q8.
static void fb_rrect(uint8_t *fb, int32_t x, int32_t y, int32_t w, int32_t h,
                     int32_t r, int v) {
    if (w <= 0 || h <= 0) return;
    if (r < 0) r = 0;
    const int32_t maxr = (w < h ? w : h) / 2;
    if (r > maxr) r = maxr;

    const int32_t hw = w / 2;
    const int32_t cx = x + hw;
    const int32_t bottom = y + h;

    int py0 = px_first(y);
    int py1 = px_last(bottom);
    if (py0 < 0) py0 = 0;
    if (py1 > FACE_H - 1) py1 = FACE_H - 1;

    for (int py = py0; py <= py1; py++) {
        const int32_t pc = ((int32_t)py << FP) + ONE / 2;
        const int32_t dtop = pc - y;
        const int32_t dbot = bottom - pc;
        if (dtop < 0 || dbot < 0) continue;

        const int32_t d = dtop < dbot ? dtop : dbot;
        int32_t half;
        if (d >= r) {
            half = hw;
        } else {
            const int32_t k = r - d;
            const int32_t s = r * r - k * k;
            half = hw - r + (s > 0 ? (int32_t)isqrt32((uint32_t)s) : 0);
        }
        if (half <= 0) continue;
        fb_span(fb, py, px_first(cx - half), px_last(cx + half), v);
    }
}

static void fb_ellipse(uint8_t *fb, int32_t cx, int32_t cy, int32_t rx,
                       int32_t ry, int v) {
    if (rx <= 0 || ry <= 0) return;

    int py0 = px_first(cy - ry);
    int py1 = px_last(cy + ry);
    if (py0 < 0) py0 = 0;
    if (py1 > FACE_H - 1) py1 = FACE_H - 1;

    for (int py = py0; py <= py1; py++) {
        const int32_t pc = ((int32_t)py << FP) + ONE / 2;
        int32_t dy = pc - cy;
        if (dy < 0) dy = -dy;
        if (dy > ry) continue;
        const int32_t s = ry * ry - dy * dy;                    // Q16
        const int32_t half = (rx * (int32_t)isqrt32((uint32_t)s)) / ry;
        if (half <= 0) continue;
        fb_span(fb, py, px_first(cx - half), px_last(cx + half), v);
    }
}

// The lower lid rising, bowed up in the middle - the way a smile actually
// changes an eye. Drawn as the top edge of a very wide ellipse sitting below
// the eye, clipped to this eye's own columns: the bow needs an ellipse wider
// than the whole face, which would otherwise nick the other eye's bottom.
//
// The first attempt cut a narrow dome out of the bottom instead. That leaves
// the eye thin in the middle and thick at the sides, which reads as a scowl.
static void fb_lower_lid(uint8_t *fb, int32_t bx, int32_t w, int32_t cx,
                         int32_t ccy, int32_t rx, int32_t ry) {
    if (rx <= 0 || ry <= 0) return;

    const int xa = px_first(bx);
    const int xb = px_last(bx + w);

    int py0 = px_first(ccy - ry);
    int py1 = px_last(ccy + ry);
    if (py0 < 0) py0 = 0;
    if (py1 > FACE_H - 1) py1 = FACE_H - 1;

    for (int py = py0; py <= py1; py++) {
        const int32_t pc = ((int32_t)py << FP) + ONE / 2;
        int32_t dy = pc - ccy;
        if (dy < 0) dy = -dy;
        if (dy > ry) continue;
        const int32_t s = ry * ry - dy * dy;
        const int32_t half = (rx * (int32_t)isqrt32((uint32_t)s)) / ry;
        int x0 = px_first(cx - half), x1 = px_last(cx + half);
        if (x0 < xa) x0 = xa;
        if (x1 > xb) x1 = xb;
        fb_span(fb, py, x0, x1, 0);
    }
}

// Clears everything above the line from (bx, yL) to (bx + bw, yR), within that
// column range. One span per row, so a slanted eyelid costs the same as a flat
// one.
static void fb_lid(uint8_t *fb, int32_t bx, int32_t bw, int32_t yL, int32_t yR) {
    if (bw <= 0) return;

    const int32_t ylo = yL < yR ? yL : yR;
    const int32_t yhi = yL > yR ? yL : yR;

    int py1 = px_last(yhi);
    if (py1 > FACE_H - 1) py1 = FACE_H - 1;

    const int xa = px_first(bx);
    const int xb = px_last(bx + bw);
    const int32_t slope = yR - yL;

    for (int py = 0; py <= py1; py++) {
        const int32_t pc = ((int32_t)py << FP) + ONE / 2;
        if (pc >= yhi) continue;
        if (pc < ylo) {
            fb_span(fb, py, xa, xb, 0);
            continue;
        }
        // Somewhere along the slope: find where the lid crosses this row.
        // slope cannot be zero here, because then ylo == yhi and one of the
        // two branches above has already taken the row.
        const int32_t xt = bx + ((pc - yL) * bw) / slope;
        if (slope > 0) {
            fb_span(fb, py, px_first(xt), xb, 0);
        } else {
            fb_span(fb, py, xa, px_last(xt), 0);
        }
    }
}

// ----------------------------------------------------------------- the eyes

#define EYE_W (46 * ONE)
#define EYE_H (46 * ONE)
#define EYE_CY (32 * ONE)
#define EYE_CX_L (34 * ONE)
#define EYE_CX_R (94 * ONE)
#define EYE_R (14 * ONE)
#define PUPIL_R (11 * ONE)
#define GAZE_X (7 * ONE)
#define GAZE_Y (7 * ONE)
#define HILITE_R (3 * ONE)

static void draw_eye(uint8_t *fb, const face_pose_t *p, int i) {
    // The left eye's inner side is its right edge, and vice versa. Every lid
    // slant depends on knowing which way the nose is.
    const bool inner_is_right = (i == 0);

    const int32_t vs = clamp16(p->vs + (i == 0 ? p->asym : -p->asym), 12, 900);
    const int32_t hs = clamp16(p->hs, 12, 900);

    const int32_t w = (EYE_W * hs) >> FP;
    const int32_t h = (EYE_H * vs) >> FP;
    const int32_t cx = (i == 0) ? EYE_CX_L : EYE_CX_R;
    const int32_t bx = cx - w / 2;
    const int32_t by = EYE_CY - h / 2;

    int32_t r = (w < h ? w : h) / 2;
    if (r > EYE_R) r = EYE_R;

    fb_rrect(fb, bx, by, w, h, r, 1);

    const int32_t pup = p->pup < 40 ? 40 : p->pup;
    const int32_t prx = (PUPIL_R * pup) >> FP;
    const int32_t pry = (vs < ONE) ? (prx * vs) >> FP : prx;
    const int32_t px = cx + ((GAZE_X * p->gx) >> FP);
    const int32_t py = EYE_CY + ((GAZE_Y * p->gy) >> FP);

    // The pupil, while there is enough eye left to hold one.
    if (p->lid < 190 && h > 14 * ONE) {
        fb_ellipse(fb, px, py, prx, pry, 0);

        // Two highlights, a big one up and left and a small one down and
        // right. Clamped so they can never escape a pupil that has contracted.
        if (p->lid < 120) {
            int32_t hr = (HILITE_R * pup) >> FP;
            const int32_t hmax_x = (prx * 115) >> FP;
            const int32_t hmax_y = (pry * 115) >> FP;
            if (hr > hmax_x) hr = hmax_x;
            if (hr > hmax_y) hr = hmax_y;
            if (hr > 0) {
                fb_ellipse(fb, px - ((prx * 90) >> FP), py - ((pry * 100) >> FP),
                           hr, hr, 1);
                const int32_t hr2 = hr / 2;
                if (hr2 > 0)
                    fb_ellipse(fb, px + ((prx * 100) >> FP),
                               py + ((pry * 95) >> FP), hr2, hr2, 1);
            }
        }
    }

    // Annoyed drops the inner lid, sad the outer one. Two expressions that are
    // each other mirrored, which is why face_test.c checks the sign.
    if (p->slant > 2 || p->slant < -2) {
        const int32_t mag = p->slant < 0 ? -p->slant : p->slant;
        const int32_t drop = (h * mag) >> FP;
        const bool left_low = (p->slant > 0) ? !inner_is_right : inner_is_right;
        fb_lid(fb, bx, w, by + (left_low ? drop : 0), by + (left_low ? 0 : drop));
    }

    // A flat lid: sleepiness, and every blink. Widened a couple of pixels so
    // the rounded corners go with it.
    if (p->lid > 2) {
        const int32_t ly = by + ((h * p->lid) >> FP);
        fb_lid(fb, bx - 2 * ONE, w + 4 * ONE, ly, ly);
    }

    // Happiness. `rise` is how far the lower lid comes up, `bow` how much
    // higher it sits in the middle than at the corners. Both scale with
    // `happy`, so a partial value is a hint of a smile rather than a
    // differently shaped eye.
    if (p->happy > 2) {
        const int32_t rise = (95 * p->happy) >> FP;  // up to 37% of the height
        const int32_t bow = (26 * p->happy) >> FP;   // up to 10%
        if (bow > 0) {
            int32_t lid_y = by + h - ((h * rise) >> FP);

            // The lid must stop below the pupil. On a one-bit panel a pupil
            // that reaches the bottom edge stops being a pupil and becomes a
            // notch: its black merges with the black around the eye and the
            // whole shape reads as a helmet. Two pixels of white underneath is
            // the difference between a smile and a scowl.
            const int32_t floor_y = py + pry + 2 * ONE;
            if (lid_y < floor_y) lid_y = floor_y;

            if (lid_y < by + h) {
                // ry chosen so the ellipse's top is `bow` higher at the centre
                // than at the eye's edge, given rx = 1.1 w. Shifted in two
                // steps rather than one, so a tall pose cannot overflow int32.
                const int32_t ry = (((h * bow) >> FP) * 2342) >> FP;
                fb_lower_lid(fb, bx, w, cx, lid_y + ry, (w * 282) >> FP, ry);
            }
        }
    }
}

void face_render_pose(uint8_t *fb, const face_pose_t *pose) {
    memset(fb, 0, FACE_FB_BYTES);
    draw_eye(fb, pose, 0);
    draw_eye(fb, pose, 1);
}

// -------------------------------------------------------------- the emotions

//                                 hs   vs   gx   gy slant happy lid  pup asym
static const face_pose_t EMO_POSE[FACE_EMO_COUNT] = {
    [FACE_EMO_NEUTRAL]   = {256, 256,   0,   0,    0,   0,   0, 256,  0},
    [FACE_EMO_HAPPY]     = {258, 236,   0,  -8,    0, 256,   0, 262,  0},
    [FACE_EMO_EXCITED]   = {272, 296,   0, -18,    0,  70,   0, 180,  0},
    [FACE_EMO_CURIOUS]   = {256, 268, 150, -76,    0,   0,   0, 238, 26},
    [FACE_EMO_CONFUSED]  = {256, 250,  86, -24,   26,   0,   0, 246, 60},
    [FACE_EMO_SURPRISED] = {278, 312,   0, -12,    0,   0,   0, 150,  0},
    [FACE_EMO_SAD]       = {252, 226,   0, 104, -110,   0,   0, 272,  0},
    [FACE_EMO_ANNOYED]   = {256, 236,   0,  14,  130,   0,   0, 224, 12},
    [FACE_EMO_SLEEPY]    = {252, 190,   0,  70,    0,   0, 110, 256,  0},
};

// Poses the model cannot ask for: they belong to the device's own situation.
static const face_pose_t POSE_ASLEEP = {250, 210, 0, 90, 0, 0, 238, 256, 0};
static const face_pose_t POSE_STARTLED = {286, 326, 0, -10, 0, 0, 0, 140, 0};

static const char *const EMO_NAME[FACE_EMO_COUNT] = {
    "neutral", "happy", "excited", "curious", "confused",
    "surprised", "sad", "annoyed", "sleepy",
};

static const char *const ST_NAME[FACE_ST_COUNT] = {
    "idle", "listening", "thinking", "speaking", "offline",
};

const char *face_emotion_name(face_emotion_t e) {
    return (e < FACE_EMO_COUNT) ? EMO_NAME[e] : "?";
}

const char *face_state_name(face_state_t s) {
    return (s < FACE_ST_COUNT) ? ST_NAME[s] : "?";
}

const face_pose_t *face_emotion_pose(face_emotion_t e) {
    return (e < FACE_EMO_COUNT) ? &EMO_POSE[e] : &EMO_POSE[FACE_EMO_NEUTRAL];
}

face_emotion_t face_emotion_scan(const char *buf, size_t len) {
    if (buf == NULL) return FACE_EMO_COUNT;
    for (int e = 0; e < FACE_EMO_COUNT; e++) {
        const char *name = EMO_NAME[e];
        const size_t n = strlen(name);
        if (n > len) continue;
        for (size_t i = 0; i + n <= len; i++) {
            if (memcmp(buf + i, name, n) == 0) return (face_emotion_t)e;
        }
    }
    return FACE_EMO_COUNT;
}

// ------------------------------------------------------------------ the life

static uint32_t blink_interval(face_t *f) {
    switch (f->state) {
        // Attention means fewer blinks, not more. A face that flutters while
        // you are talking to it looks nervous rather than interested.
        case FACE_ST_LISTENING: return 5000u + rnd(f) % 5000u;
        case FACE_ST_THINKING:  return 1400u + rnd(f) % 1600u;
        case FACE_ST_SPEAKING:  return 2200u + rnd(f) % 2600u;
        default:                return 2500u + rnd(f) % 3500u;
    }
}

// A blink is the upper lid sweeping down - `lid` driven almost shut and back -
// not a symmetrical squash. The bottom edge staying put is what makes it read
// as an eyelid. It stops just short of closed so a lash line always remains:
// a blank 128x64 panel does not read as "blinking", it reads as "broken".
static int32_t blink_lid(face_t *f, uint32_t now) {
    if (f->state == FACE_ST_OFFLINE) return 0;

    if (f->blink_start == 0 && due(now, f->next_blink)) {
        f->blink_start = now ? now : 1;
        f->blinks_left = (rnd(f) % 5u == 0u) ? 1 : 0;  // a double blink now and then
    }
    if (f->blink_start == 0) return 0;

    const uint32_t e = since(now, f->blink_start);
    const uint32_t close = 110, hold = 60, open = 130;

    int32_t lid;
    if (e < close) {
        lid = smoothstep(e, close);
    } else if (e < close + hold) {
        lid = ONE;
    } else if (e < close + hold + open) {
        lid = ONE - smoothstep(e - close - hold, open);
    } else {
        f->blink_start = 0;
        if (f->blinks_left) {
            f->blinks_left--;
            f->next_blink = now + 90u;
        } else {
            f->next_blink = now + blink_interval(f);
        }
        return 0;
    }
    return (lid * 250) >> FP;
}

// Ballistic saccades: a fast move to a new target, then a dead stop. Drifting
// smoothly between points looks like a camera pan; eyes do not do that.
static void saccade(face_t *f, uint32_t now, int32_t *out_x, int32_t *out_y) {
    if (due(now, f->next_sacc)) {
        int32_t ax, ay;
        uint32_t every, dur;
        switch (f->state) {
            case FACE_ST_LISTENING: ax = 26; ay = 20; every = 2500u + rnd(f) % 1800u; dur = 90u + rnd(f) % 60u;  break;
            // Fewer and slower than anything else: the gaze rolls to a new
            // place and stays there, rather than darting.
            case FACE_ST_THINKING:  ax = 62; ay = 40; every = 1100u + rnd(f) %  900u; dur = 230u + rnd(f) % 90u; break;
            case FACE_ST_SPEAKING:  ax = 44; ay = 34; every = 1200u + rnd(f) % 1400u; dur = 90u + rnd(f) % 60u;  break;
            case FACE_ST_OFFLINE:   ax = 0;  ay = 0;  every = 4000u;                  dur = 200u;                break;
            default:                ax = 92; ay = 60; every = 1500u + rnd(f) % 2600u; dur = 90u + rnd(f) % 60u;  break;
        }
        f->sacc_from_x = f->sacc_to_x;
        f->sacc_from_y = f->sacc_to_y;
        f->sacc_to_x = ax ? (int16_t)((int32_t)(rnd(f) % (uint32_t)(2 * ax + 1)) - ax) : 0;
        f->sacc_to_y = ay ? (int16_t)((int32_t)(rnd(f) % (uint32_t)(2 * ay + 1)) - ay) : 0;
        f->sacc_start = now;
        f->sacc_dur = dur;
        f->next_sacc = now + every;
    }

    const int32_t s = smoothstep(since(now, f->sacc_start), f->sacc_dur);
    *out_x = f->sacc_from_x + (((int32_t)(f->sacc_to_x - f->sacc_from_x) * s) >> FP);
    *out_y = f->sacc_from_y + (((int32_t)(f->sacc_to_y - f->sacc_from_y) * s) >> FP);
}

static void overlay_state(face_t *f, face_pose_t *t) {
    switch (f->state) {
        case FACE_ST_LISTENING:
            // Wide, pupils dilated, gaze pulled forward: leaning in.
            t->vs = (int16_t)(t->vs + 32);
            t->pup = (int16_t)(t->pup + 26);
            t->gx = (int16_t)(t->gx / 3);
            t->gy = (int16_t)(t->gy / 3 - 12);
            break;

        case FACE_ST_THINKING:
            // Up and away, and - the part that took a measurement to find -
            // narrowed.
            //
            // The first version moved only the pupil: it held the gaze up 82%
            // of the time and swung 10 px sideways, and still read as idle.
            // Its eyes were 46.3 px tall against idle's 45.7, so the silhouette
            // was identical and the pupil was carrying the whole message on its
            // own. From across the room shape is read first and pupils second.
            //
            // So the eyes now genuinely squint, the inner lids come down into a
            // furrow, and the gaze re-aims half as often, because a face that
            // flicks about once a second reads as nervous rather than
            // thoughtful.
            if (due(f->now, f->next_think_aim)) {
                f->think_dir = (int16_t)((rnd(f) & 1u) ? 210 : -210);
                f->next_think_aim = f->now + 1400u + rnd(f) % 1000u;
            }
            t->gx = f->think_dir;
            t->gy = -185;
            t->pup = (int16_t)((t->pup * 218) >> FP);
            t->asym = (int16_t)(t->asym + 22);
            t->slant = (int16_t)(t->slant + 45);  // a furrow, not a scowl
            t->vs = (int16_t)((t->vs * 205) >> FP);
            break;

        case FACE_ST_SPEAKING:
            // The emotion stands as it is; loudness does the work below.
            break;

        default:
            break;
    }
}

// ------------------------------------------------------------------ power on

// Two sparks shoot outward into a bar, the bar warms up and collapses back
// into eyes, and then the eyes open exactly as far as the device is ready.
//
// Everything here is the ordinary pose model; nothing new is drawn. Three
// things fall out of the renderer for free and the sequence is built around
// them:
//
//   `hs` far above 1 makes the two eyes overlap into a single bar across the
//   whole panel. That is the one shape in the entire vocabulary that cannot be
//   mistaken for a mood, so "power came on" is unambiguous from the very first
//   frame the panel is able to draw.
//
//   No pupil is drawn below 14 px of eye height, so the pupils vanish for the
//   bar and arrive by themselves as the eyes grow.
//
//   `vs` is floored at 12, so the bar can thin but never vanish - the same
//   clamp that stops a blink leaving a dead panel. The warm-up flicker is six
//   rows to two and back, which is the deepest pulse the renderer allows.
//
// The waiting pose is a slit with the pupil riding low and sweeping, because
// two motionless slits are precisely what "no connection" already looks like,
// and the first thing shown after power-on must not be that.
#define BOOT_SPARK 90    // the filaments light
#define BOOT_SHOOT 190   // they race outward and meet
#define BOOT_WARM 140    // the bar catches
#define BOOT_SNAP 180    // the bar collapses back to two dashes
#define BOOT_GROW 160    // and they grow upward into eyes
#define BOOT_FLOURISH (BOOT_SPARK + BOOT_SHOOT + BOOT_WARM + BOOT_SNAP + BOOT_GROW)
#define BOOT_OPEN 420    // opening once the socket lands
#define BOOT_SETTLE 1500 // the blink and the look around after that

// How ready the device is, 0..256. It holds rather than easing between
// stages: the device has no idea how far along an association is, and a bar
// creeping along would be inventing progress it cannot know about.
static int32_t boot_readiness(const face_t *f) {
    switch (f->boot_stage) {
        case FACE_BOOT_LINK: {
            const uint32_t e = since(f->now, f->boot_reached);
            return lerp_q8(140, ONE, smoothstep(e, BOOT_OPEN));
        }
        case FACE_BOOT_WIFI: return 140;
        default:             return 38;
    }
}

static void boot_pose(face_t *f, face_pose_t *p) {
    p->hs = ONE; p->vs = ONE; p->gx = 0; p->gy = 0;
    p->slant = 0; p->happy = 0; p->lid = 0; p->pup = ONE; p->asym = 0;

    const uint32_t e = since(f->now, f->boot_start);
    const int32_t r = boot_readiness(f);

    if (e < BOOT_SPARK) {
        p->hs = (int16_t)lerp_q8(44, 120, smoothstep(e, BOOT_SPARK));
        p->vs = 7;
        return;
    }
    if (e < BOOT_SPARK + BOOT_SHOOT) {
        p->hs = (int16_t)lerp_q8(120, 900, smoothstep(e - BOOT_SPARK, BOOT_SHOOT));
        p->vs = 7;
        return;
    }
    if (e < BOOT_SPARK + BOOT_SHOOT + BOOT_WARM) {
        const uint32_t k = e - BOOT_SPARK - BOOT_SHOOT;
        p->hs = 900;
        p->vs = (int16_t)(k < 40u ? 34 : k < 80u ? 12 : 38);
        return;
    }
    if (e < BOOT_FLOURISH - BOOT_GROW) {
        // Width first, height second, and deliberately not on one curve: run
        // them together and the pupils arrive while the shape is still a
        // full-width bar, which reads as a letterbox with two holes in it.
        const int32_t k = smoothstep(e - BOOT_SPARK - BOOT_SHOOT - BOOT_WARM, BOOT_SNAP);
        p->hs = (int16_t)(k < 190 ? lerp_q8(900, 228, k * ONE / 190)
                                  : lerp_q8(228, ONE, (k - 190) * ONE / 66));
        p->vs = 38;
        return;
    }
    if (e < BOOT_FLOURISH) {
        const int32_t k = smoothstep(e - (BOOT_FLOURISH - BOOT_GROW), BOOT_GROW);
        p->vs = (int16_t)lerp_q8(38, 225, k);
        p->lid = (int16_t)lerp_q8(0, 165, k);
        p->gy = (int16_t)lerp_q8(0, 140, k);
        return;
    }

    // Waiting, then opening. `away` is how far from ready it still is.
    const int32_t away = ONE - r;
    const int32_t span = ONE - 38;
    p->vs = (int16_t)lerp_q8(ONE, 225, away * ONE / span);
    p->lid = (int16_t)(165 * away / span);
    p->gy = (int16_t)(140 * away / span);
    p->gx = (int16_t)((fsin(phase_of(f->now, 1800u)) * 200 * away / span) >> FP);
    p->pup = (int16_t)lerp_q8(ONE, 218, away * ONE / span);

    if (f->boot_stage != FACE_BOOT_LINK) return;

    // Ready: one blink, then a look around, then hand over to the ordinary
    // states with the pose already where they expect it.
    const uint32_t done = since(f->now, f->boot_reached);
    if (done < BOOT_OPEN) return;

    const uint32_t s = done - BOOT_OPEN;
    if (s < 260u) {
        if (s < 110u) p->lid = (int16_t)((smoothstep(s, 110) * 250) >> FP);
        else if (s < 160u) p->lid = 250;
        else p->lid = (int16_t)(((ONE - smoothstep(s - 160u, 100)) * 250) >> FP);
    } else if (s < BOOT_SETTLE) {
        const uint32_t u = s - 260u;
        p->gx = (int16_t)((fsin(phase_of(u, 2480u)) * 150) >> FP);
        p->gy = (int16_t)(-((fsin(phase_of(u, 1240u)) * 50) >> FP));
    } else {
        f->booting = false;   // the ordinary state machine takes it from here
        f->cur = *p;
        f->last_activity = f->now;
        f->next_blink = f->now + blink_interval(f);
        f->next_sacc = f->now;
    }
}

static void slew(face_pose_t *c, const face_pose_t *t, int32_t fast, int32_t slow) {
#define TOWARD(field, a) c->field = (int16_t)(c->field + (((int32_t)(t->field - c->field) * (a)) >> FP))
    TOWARD(hs, fast);
    TOWARD(vs, fast);
    TOWARD(pup, fast);
    TOWARD(gx, fast);
    TOWARD(gy, fast);
    TOWARD(slant, slow);
    TOWARD(happy, slow);
    TOWARD(lid, slow);
    TOWARD(asym, slow);
#undef TOWARD
}

// ------------------------------------------------------------------- the API

void face_init(face_t *f, uint32_t now_ms) {
    memset(f, 0, sizeof(*f));

    f->state = FACE_ST_IDLE;
    f->emotion = FACE_EMO_NEUTRAL;
    f->now = now_ms;
    f->last_tick = now_ms;
    f->state_since = now_ms;
    f->last_activity = now_ms;
    f->last_energy = now_ms;

    // Seeded from the clock the caller handed us, so the same start time gives
    // the same animation. The preview on the laptop and the panel on the desk
    // have to agree, which rules out rand() and any wall clock.
    f->rng = now_ms * 2654435761u + 12345u;
    if (f->rng == 0) f->rng = 1;

    f->cur = EMO_POSE[FACE_EMO_NEUTRAL];
    f->next_blink = now_ms + 1200u + rnd(f) % 2000u;
    f->next_sacc = now_ms + 400u;
    f->next_think_aim = now_ms;
    f->think_dir = 150;

    // face_init is called the moment the panel answers, which is exactly when
    // the power-on sequence should start.
    f->booting = true;
    f->boot_stage = FACE_BOOT_PANEL;
    f->boot_start = now_ms;
    f->boot_reached = now_ms;

    face_pose_t first;
    boot_pose(f, &first);
    face_render_pose(f->fb, &first);
}

void face_set_state(face_t *f, face_state_t state, uint32_t now_ms) {
    f->now = now_ms;
    if (state >= FACE_ST_COUNT || state == f->state) return;

    f->state = state;
    f->state_since = now_ms;
    f->last_activity = now_ms;
    f->next_sacc = now_ms;  // re-aim at once rather than finishing the old plan
    f->next_blink = now_ms + blink_interval(f);
}

void face_set_emotion(face_t *f, face_emotion_t emotion, uint32_t now_ms) {
    f->now = now_ms;
    if (emotion >= FACE_EMO_COUNT) return;
    f->emotion = emotion;
    f->last_activity = now_ms;  // a fresh emotion means the conversation is alive
}

void face_set_button(face_t *f, bool down, uint32_t now_ms) {
    f->now = now_ms;
    if (down && !f->button) {
        f->last_activity = now_ms;
        if (f->state == FACE_ST_OFFLINE) {
            // Heard you, and can do nothing about it. The lids crack open and
            // fall shut again, which is more honest than ignoring the press.
            f->stir_until = now_ms + 400u;
            if (f->stir_until == 0) f->stir_until = 1;
        }
    }
    f->button = down;
}

void face_boot_stage(face_t *f, face_boot_t stage, uint32_t now_ms) {
    f->now = now_ms;
    if (!f->booting) return;                  // the sequence has already ended
    if ((uint8_t)stage <= f->boot_stage) return;  // forward only
    f->boot_stage = (uint8_t)stage;
    f->boot_reached = now_ms;
}

void face_startle(face_t *f, uint32_t now_ms) {
    f->now = now_ms;
    f->startle_until = now_ms + 350u;
    if (f->startle_until == 0) f->startle_until = 1;
    f->last_activity = now_ms;
    f->blink_start = 0;
    f->next_blink = now_ms + 700u;  // do not blink through a flinch
}

void face_feed_energy(face_t *f, uint16_t rms) {
    // Normalise against a slowly decaying peak. The microphone sits near
    // -46 dBFS and a TTS reply near full scale; without this one direction
    // would barely move the eyes and the other would pin them.
    if (rms > f->peak) f->peak = rms;
    else if (f->peak > 0) f->peak -= (f->peak >> 6) + 1;

    uint32_t ref = f->peak;
    if (ref < 96u) ref = 96u;  // a floor, so room noise is not amplified to a shout
    uint32_t v = ((uint32_t)rms * 255u) / ref;
    if (v > 255u) v = 255u;

    if (v > f->e_fast) {
        f->e_fast = (uint8_t)v;  // instant attack: a syllable starts abruptly
    } else if (f->e_fast > v) {
        uint8_t step = (uint8_t)((f->e_fast - v) >> 2);
        if (step == 0) step = 1;
        f->e_fast = (uint8_t)(f->e_fast - step);
    }

    if (f->e_slow != (uint8_t)v) {
        const int32_t d = (int32_t)v - (int32_t)f->e_slow;
        int32_t step = d >> 3;
        if (step == 0) step = (d > 0) ? 1 : -1;
        f->e_slow = (uint8_t)((int32_t)f->e_slow + step);
    }

    f->last_energy = f->now;
}

void face_tick(face_t *f, uint32_t now_ms) {
    uint32_t dt = since(now_ms, f->last_tick);
    if (dt > 500u) dt = 500u;  // a stalled task must not make the face lurch
    f->now = now_ms;
    f->last_tick = now_ms;

    // Loudness decays on its own. Playback stops without sending a final block
    // of silence, so without this the eyes would freeze mid-syllable.
    if (since(now_ms, f->last_energy) > 80u) {
        if (f->e_fast) f->e_fast = (uint8_t)(f->e_fast - (f->e_fast >> 1) - 1);
        if (f->e_slow) f->e_slow = (uint8_t)(f->e_slow - (f->e_slow >> 2) - 1);
    }

    // The power-on sequence owns the face until it hands over. It is not a
    // state the server can ask for and not an emotion: it reports where the
    // device itself has got to, and nothing else may talk over it.
    if (f->booting) {
        face_pose_t boot;
        boot_pose(f, &boot);
        boot.lid = clamp16(boot.lid, 0, 250);
        boot.vs = clamp16(boot.vs, 12, 900);
        boot.hs = clamp16(boot.hs, 12, 900);
        boot.pup = clamp16(boot.pup, 40, 700);
        face_render_pose(f->fb, &boot);
        return;
    }

    if (f->startle_until && due(now_ms, f->startle_until)) f->startle_until = 0;
    if (f->stir_until && due(now_ms, f->stir_until)) f->stir_until = 0;

    const bool startled = f->startle_until != 0;
    const bool stirring = f->stir_until != 0;

    // Left alone the face lets go of the last thing it felt, gets sleepy, and
    // eventually shuts its eyes. A press or anything from the server resets it.
    const uint32_t idle_ms = since(now_ms, f->last_activity);
    face_emotion_t emo = f->emotion;
    bool asleep = (f->state == FACE_ST_OFFLINE);
    if (f->state == FACE_ST_IDLE) {
        if (idle_ms > 180000u) {
            asleep = true;
        } else if (idle_ms > 90000u) {
            emo = FACE_EMO_SLEEPY;
        } else if (idle_ms > 45000u) {
            emo = FACE_EMO_NEUTRAL;
        }
    }

    face_pose_t tgt = asleep ? POSE_ASLEEP : EMO_POSE[emo];
    if (!asleep) overlay_state(f, &tgt);

    if (stirring) {
        const int32_t open = (int32_t)((since(f->stir_until, now_ms) * 256u) / 400u);
        const int32_t o = open > ONE ? ONE : open;
        tgt.lid = (int16_t)(POSE_ASLEEP.lid - ((110 * o) >> FP));
        tgt.vs = (int16_t)(POSE_ASLEEP.vs + ((40 * o) >> FP));
    }
    if (startled) tgt = POSE_STARTLED;

    // Slewing every parameter toward its target instead of snapping to it is
    // most of what separates a face from a slideshow. A flinch is the exception
    // and has to arrive at once.
    const int32_t scale = (int32_t)dt;
    int32_t fast = (startled || stirring) ? 190 : 46;
    int32_t slow = (startled || stirring) ? 190 : 34;
    fast = fast * scale / 40;
    slow = slow * scale / 40;
    if (fast > ONE) fast = ONE;
    if (slow > ONE) slow = ONE;
    slew(&f->cur, &tgt, fast, slow);

    face_pose_t out = f->cur;

    // Blinks and saccades sit on top of the slewed pose: they are events, not
    // states, and filtering them would blunt exactly what makes them read.
    const int32_t blink = blink_lid(f, now_ms);
    if (blink > out.lid) out.lid = (int16_t)blink;

    int32_t sx, sy;
    saccade(f, now_ms, &sx, &sy);
    // Ocular microtremor: the eyes are never quite still, even mid-fixation.
    const int32_t tx = ((fsin(phase_of(now_ms, 1700u)) * 11) >> FP) +
                       ((fsin(phase_of(now_ms, 610u)) * 4) >> FP);
    const int32_t ty = (fsin(phase_of(now_ms, 2300u)) * 8) >> FP;
    out.gx = clamp16(out.gx + sx + tx, -ONE, ONE);
    out.gy = clamp16(out.gy + sy + ty, -ONE, ONE);

    // Breathing.
    if (!startled) {
        const int32_t s = fsin(phase_of(now_ms, asleep ? 6000u : 4500u));
        out.vs = (int16_t)(out.vs + ((out.vs * 8 * s) >> 16));
        if (asleep) out.lid = (int16_t)(out.lid - ((s * 6) >> FP));
    }

    // Loudness. This is the whole reason the mouth was not needed: a reply
    // squashes the eyes per syllable and dilates the pupils, and your own voice
    // widens them.
    if (!startled) {
        if (f->state == FACE_ST_SPEAKING) {
            out.vs = (int16_t)(out.vs - ((out.vs * (int32_t)f->e_fast * 46) >> 16));
            out.hs = (int16_t)(out.hs + ((out.hs * (int32_t)f->e_fast * 16) >> 16));
            out.pup = (int16_t)(out.pup + (((int32_t)f->e_slow * 34) >> FP));
            out.gy = clamp16(out.gy + (((int32_t)f->e_fast * 22) >> FP), -ONE, ONE);
        } else if (f->state == FACE_ST_LISTENING) {
            out.vs = (int16_t)(out.vs + ((out.vs * (int32_t)f->e_slow * 34) >> 16));
            out.pup = (int16_t)(out.pup - (((int32_t)f->e_fast * 30) >> FP));
        }
    }

    // The lid never quite reaches shut, so there is always a lash line to see.
    out.lid = clamp16(out.lid, 0, 250);
    out.vs = clamp16(out.vs, 12, 900);
    out.hs = clamp16(out.hs, 12, 900);
    out.pup = clamp16(out.pup, 40, 700);

    face_render_pose(f->fb, &out);
}
