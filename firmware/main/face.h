// An animated pair of eyes for a 128x64 one-bit panel.
//
// This header and face.c deliberately depend on nothing: no ESP-IDF, no libm,
// no allocation, no floating point. The ESP32-C3 has no FPU, so every
// coordinate here is Q8 fixed point - 256 means one pixel - and the curves come
// from an integer square root and a parabolic sine approximation.
//
// The point of that discipline is not purity. It is that the same code which
// drives the panel also compiles on the laptop, where host/face_preview.c turns
// it into an animation you can actually look at and host/face_test.c asserts it
// behaves. The display arrived after the firmware did, so for a while the only
// way to judge any of this was on the host.
//
// The framebuffer is already in the panel's own layout - 8 pages of 128
// columns, bit n of a byte being row page*8+n - so flushing it is a copy rather
// than a transpose.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FACE_W 128
#define FACE_H 64
#define FACE_PAGES (FACE_H / 8)
#define FACE_FB_BYTES (FACE_W * FACE_PAGES)

// What the assistant feels. This is the whole vocabulary the server may send;
// anything else is ignored rather than guessed at.
typedef enum {
    FACE_EMO_NEUTRAL = 0,
    FACE_EMO_HAPPY,
    FACE_EMO_EXCITED,
    FACE_EMO_CURIOUS,
    FACE_EMO_CONFUSED,
    FACE_EMO_SURPRISED,
    FACE_EMO_SAD,
    FACE_EMO_ANNOYED,
    FACE_EMO_SLEEPY,
    FACE_EMO_COUNT
} face_emotion_t;

// What the device is doing. Mirrors state_t in voice_main.c, plus the case that
// firmware tracks separately: the socket being down.
typedef enum {
    FACE_ST_IDLE = 0,
    FACE_ST_LISTENING,
    FACE_ST_THINKING,
    FACE_ST_SPEAKING,
    FACE_ST_OFFLINE,
    FACE_ST_COUNT
} face_state_t;

// How far the device has got since power came on.
//
// The face plays a power-on sequence until it reaches FACE_BOOT_LINK, and how
// far the eyes open is how far the device has got. That is not decoration:
// five seconds pass between the panel answering and the socket connecting, and
// this is the only thing that can say where in those five seconds it is. Stuck
// on WiFi and stuck on the server look different from each other, and both
// look different from working.
//
// The stage only ever moves forward. A socket that drops later is the ordinary
// FACE_ST_OFFLINE, not a reboot.
typedef enum {
    FACE_BOOT_PANEL = 0,  // the display answered; nothing else is up
    FACE_BOOT_WIFI,       // an IP arrived
    FACE_BOOT_LINK,       // the socket is connected - ready to talk
} face_boot_t;

// One eye's shape, all Q8. Emotions are rows of this; blinking, breathing,
// gaze and loudness are modulations applied on top.
typedef struct {
    int16_t hs;     // horizontal scale, 256 = rest
    int16_t vs;     // vertical scale
    int16_t gx;     // gaze bias, fraction of available travel, signed
    int16_t gy;
    int16_t slant;  // > 0 inner lid down (angry), < 0 outer lid down (sad)
    int16_t happy;  // crescent cut away from below, 0..256
    int16_t lid;    // flat upper lid, 0 open, 256 shut
    int16_t pup;    // pupil scale
    int16_t asym;   // how much the two eyes differ, for a lopsided look
} face_pose_t;

typedef struct {
    uint8_t fb[FACE_FB_BYTES];

    // -- what the owner told us
    face_state_t state;
    face_emotion_t emotion;
    bool button;

    // -- the power-on sequence, until the socket comes up
    bool booting;
    uint8_t boot_stage;
    uint32_t boot_start;   // when the panel came up and drawing became possible
    uint32_t boot_reached; // when the current stage was reached

    // -- timing, all in milliseconds from the same clock the owner passes in
    uint32_t now;
    uint32_t last_tick;
    uint32_t state_since;
    uint32_t last_activity;   // last state change, button or startle
    uint32_t startle_until;
    uint32_t stir_until;      // a press while offline cracks the lids briefly

    // -- pose, slewed toward its target rather than snapped
    face_pose_t cur;

    // -- gaze: ballistic saccades plus a permanent microtremor
    int16_t sacc_from_x, sacc_from_y;
    int16_t sacc_to_x, sacc_to_y;
    uint32_t sacc_start, sacc_dur;
    uint32_t next_sacc;
    int16_t think_dir;
    uint32_t next_think_aim;

    // -- blink
    uint32_t blink_start;     // 0 when not blinking
    uint32_t next_blink;
    uint8_t blinks_left;      // a double blink now and then

    // -- loudness, normalised against a decaying peak
    uint32_t peak;
    uint32_t last_energy;
    uint8_t e_fast;
    uint8_t e_slow;

    uint32_t rng;
} face_t;

// now_ms is whatever monotonic millisecond clock the caller has; face.c only
// ever takes differences, so the epoch does not matter.
void face_init(face_t *f, uint32_t now_ms);

void face_set_state(face_t *f, face_state_t state, uint32_t now_ms);
void face_set_emotion(face_t *f, face_emotion_t emotion, uint32_t now_ms);
void face_set_button(face_t *f, bool down, uint32_t now_ms);

// A press that interrupted a reply. Worth its own call rather than inferring it
// from the button, because by the time the state has changed the reason for the
// change is gone.
void face_startle(face_t *f, uint32_t now_ms);

// How far the device has got. Safe to call every frame with the same value;
// only an advance does anything, and the stage never goes backwards.
void face_boot_stage(face_t *f, face_boot_t stage, uint32_t now_ms);

// True until the power-on sequence has finished playing. Nothing depends on
// this inside face.c - it is here so the owner can tell whether the face is
// still reporting the boot or has taken over the ordinary states.
static inline bool face_is_booting(const face_t *f) { return f->booting; }

// Raw block RMS, in int16 sample units. Normalised internally against a slowly
// decaying peak, so the microphone near -46 dBFS and a reply near full scale
// both swing the eyes across their whole range.
void face_feed_energy(face_t *f, uint16_t rms);

// Advance the animation to now_ms and render into f->fb. Call at ~25 fps.
void face_tick(face_t *f, uint32_t now_ms);

static inline const uint8_t *face_framebuffer(const face_t *f) { return f->fb; }

// Names, for logs and for the preview.
const char *face_emotion_name(face_emotion_t e);
const char *face_state_name(face_state_t s);

// The first known emotion name occurring anywhere in the buffer, or
// FACE_EMO_COUNT if there is none. This is how the device reads the server's
// control frame: the same substring matching the rest of the protocol uses,
// because a JSON parser would be weight for no benefit on a part with 400 KB of
// RAM.
face_emotion_t face_emotion_scan(const char *buf, size_t len);

// Exposed for the preview and the tests, which want to render a pose directly
// without going through the state machine.
void face_render_pose(uint8_t *fb, const face_pose_t *pose);
const face_pose_t *face_emotion_pose(face_emotion_t e);
