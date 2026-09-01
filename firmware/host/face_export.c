// Frames for the browser's Face panel, generated from the same face.c that
// drives the panel - see face_preview.c's header for the argument this is
// built on: reimplementing the eyes in JavaScript drifts from face.c within
// a day, so the browser has to play frames this module renders instead.
//
// Unlike face_preview.c's scripted tour - one long timeline, XOR-delta
// encoded because it has to be seekable and a minute long - this writes a
// short, independent loop of frames for every state the browser connection
// can be in, crossed with every emotion the server can tag: `web/src/Face.tsx`
// just switches which loop it is stepping through and never seeks inside
// one, so there is nothing to gain from a delta stream and every frame is
// plain base64.
//
// Output shape: {"w":128,"h":64,"fps":25,"loops":{"<state>:<emotion>":[...]}}
// where each loop is an array of base64 frames, one FACE_FB_BYTES buffer per
// frame, in the panel's own layout (see face.h).
//
//   ./face_export > ../../web/src/face-frames.json

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../main/face.h"

#define FPS 25
#define TICK_MS (1000 / FPS)

// 1s: long enough to play out a whole blink (300ms, see face.c's blink_lid)
// and still leave a beat of the state's own motion after it - gaze saccades
// for thinking, eye width tracking loudness for listening and speaking. 36
// loops (4 states x 9 emotions) of independent base64 frames at this length
// keep web/src/face-frames.json comfortably under the ~1.5MB budget - see
// this file's own header and the Makefile's face-data target.
#define FRAMES_PER_LOOP 25

// Matches face_preview.c's own guard against a boot sequence that never
// reaches FACE_BOOT_LINK.
#define BOOT_CAP_MS 20000u

// How long to run face.c, uncaptured, before giving up on seeing a blink and
// starting the capture anyway. blink_interval() in face.c caps out under
// 10s for FACE_ST_LISTENING (its quietest state, deliberately - "attention
// means fewer blinks"), so this has to clear that with room to spare.
#define BLINK_WAIT_CAP_MS 12000u

typedef enum { EN_NONE = 0, EN_VOICE, EN_SPEECH } energy_kind_t;

typedef struct {
    const char *wire;  // exactly what useTurn.ts's ConversationState carries
    face_state_t state;
    energy_kind_t energy;
} state_entry_t;

// The four states a browser connection can be in. FACE_ST_OFFLINE is not
// here on purpose - Face.tsx falls back to the idle loop when the socket is
// down rather than needing a fifth set of loops for it.
static const state_entry_t STATES[] = {
    {"idle", FACE_ST_IDLE, EN_NONE},
    {"listening", FACE_ST_LISTENING, EN_VOICE},
    {"thinking", FACE_ST_THINKING, EN_NONE},
    {"speaking", FACE_ST_SPEAKING, EN_SPEECH},
};
#define NSTATES (sizeof(STATES) / sizeof(STATES[0]))

// A syllabic envelope, the same shape face_preview.c synthesises to give
// listening/speaking something to react to. Copied rather than shared
// because it is a fake microphone standing in for real audio, not part of
// face.c's contract.
static uint16_t synth_rms(uint32_t t, uint16_t peak) {
    if ((t % 2100u) > 1700u) return 0;

    const uint32_t syl = t % 222u;
    int32_t env = (int32_t)(syl < 111u ? syl : 222u - syl) * 255 / 111;

    static const int32_t stress[7] = {255, 178, 232, 120, 255, 158, 205};
    env = env * stress[(t / 222u) % 7u] / 255;

    return (uint16_t)((int32_t)peak * env / 255);
}

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void write_base64(const uint8_t *d, size_t n) {
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        const uint32_t v = ((uint32_t)d[i] << 16) | ((uint32_t)d[i + 1] << 8) | d[i + 2];
        putchar(B64[(v >> 18) & 63]);
        putchar(B64[(v >> 12) & 63]);
        putchar(B64[(v >> 6) & 63]);
        putchar(B64[v & 63]);
    }
    if (i < n) {
        const uint32_t v = ((uint32_t)d[i] << 16) | ((i + 1 < n) ? ((uint32_t)d[i + 1] << 8) : 0);
        putchar(B64[(v >> 18) & 63]);
        putchar(B64[(v >> 12) & 63]);
        putchar((i + 1 < n) ? B64[(v >> 6) & 63] : '=');
        putchar('=');
    }
}

// Drives face.c through boot, into one state:emotion pair, and past its own
// startup transient - then waits (uncaptured) for a blink to actually begin
// before recording FRAMES_PER_LOOP frames from there. Every scene in
// face_preview.c starts capturing immediately because its scenes run
// several seconds; a 1.2s loop that did the same would show a blink only by
// luck, and FACE_ST_LISTENING would need minutes of them before one landed.
static void emit_loop(face_emotion_t emotion, const state_entry_t *st) {
    face_t f;
    uint32_t t = 100000;  // fixed epoch - see face_preview.c's comment on why
    face_init(&f, t);

    face_boot_stage(&f, FACE_BOOT_LINK, t);
    while (face_is_booting(&f) && (t - f.boot_start) < BOOT_CAP_MS) {
        t += TICK_MS;
        face_tick(&f, t);
    }

    face_set_emotion(&f, emotion, t);
    face_set_state(&f, st->state, t);

    const uint32_t wait_deadline = t + BLINK_WAIT_CAP_MS;
    bool capturing = false;
    int recorded = 0;

    putchar('[');
    while (recorded < FRAMES_PER_LOOP) {
        t += TICK_MS;
        if (st->energy == EN_VOICE) face_feed_energy(&f, synth_rms(t, 420));
        else if (st->energy == EN_SPEECH) face_feed_energy(&f, synth_rms(t, 9000));

        face_tick(&f, t);

        if (!capturing && (f.blink_start != 0 || t >= wait_deadline)) capturing = true;
        if (!capturing) continue;

        if (recorded) putchar(',');
        putchar('"');
        write_base64(f.fb, FACE_FB_BYTES);
        putchar('"');
        recorded++;
    }
    putchar(']');
}

int main(void) {
    printf("{\"w\":%d,\"h\":%d,\"fps\":%d,\"loops\":{", FACE_W, FACE_H, FPS);

    bool first = true;
    for (size_t s = 0; s < NSTATES; s++) {
        for (int e = 0; e < FACE_EMO_COUNT; e++) {
            if (!first) putchar(',');
            first = false;
            printf("\"%s:%s\":", STATES[s].wire, face_emotion_name((face_emotion_t)e));
            emit_loop((face_emotion_t)e, &STATES[s]);
        }
    }

    printf("}}\n");
    return 0;
}
