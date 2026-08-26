// Runs the real animation on the laptop and writes a page you can look at.
//
// The panel arrived after the firmware did. Judging a 0.96" display by reading
// C was not working, and neither was reimplementing the eyes in JavaScript -
// the copy drifts from the original within a day. So this drives face.c itself
// over a scripted timeline, captures every frame, and emits a self-contained
// player.
//
// Frames are XOR-delta'd against their predecessor and run-length encoded,
// which is what makes a minute of 128x64 animation small enough to publish:
// consecutive frames of a blinking eye differ by a few hundred bytes, not 1024.
//
//   ./face_preview preview-template.html out.html

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/face.h"

#define FPS 25
#define TICK_MS (1000 / FPS)
#define WARP_MS 400  // uncaptured fast-forward, for the minutes-long idle decay

typedef enum { EN_NONE = 0, EN_SPEECH, EN_VOICE } energy_kind_t;

typedef struct {
    const char *label;
    const char *note;
    face_state_t state;
    face_emotion_t emotion;
    energy_kind_t energy;
    uint32_t warp_ms;     // run this long before recording, to age the face
    uint32_t capture_ms;
    uint32_t startle_at;  // ms into the capture; 0 means never
    uint32_t press_at;
} scene_t;

// The script. Order matters only in that it reads as a tour: what it does, then
// what it feels, then what goes wrong.
static const scene_t SCENES[] = {
    {"Спокойствие", "Моргание, микросаккады, дыхание. Ничего не происходит — и это видно.",
     FACE_ST_IDLE, FACE_EMO_NEUTRAL, EN_NONE, 0, 9000, 0, 0},

    {"Слушает", "Глаза шире, зрачок расширен, взгляд заперт вперёд. Ширина идёт за громкостью твоего голоса.",
     FACE_ST_LISTENING, FACE_EMO_NEUTRAL, EN_VOICE, 0, 6000, 0, 0},

    {"Думает", "Взгляд вверх и в сторону, перенаводится каждые ~700 мс. Один глаз чуть уже другого.",
     FACE_ST_THINKING, FACE_EMO_NEUTRAL, EN_NONE, 0, 5200, 0, 0},

    {"Говорит: neutral", "Громкость ответа сжимает глаза на каждом слоге. Рот не понадобился.",
     FACE_ST_SPEAKING, FACE_EMO_NEUTRAL, EN_SPEECH, 0, 3600, 0, 0},
    {"Говорит: happy", "Полумесяц срезан снизу — веко поднято, а не глаз уменьшен.",
     FACE_ST_SPEAKING, FACE_EMO_HAPPY, EN_SPEECH, 0, 3600, 0, 0},
    {"Говорит: excited", "Шире и выше, зрачок сжат, намёк на полумесяц.",
     FACE_ST_SPEAKING, FACE_EMO_EXCITED, EN_SPEECH, 0, 3600, 0, 0},
    {"Говорит: curious", "Взгляд уведён в сторону и вверх, глаза чуть разной высоты.",
     FACE_ST_SPEAKING, FACE_EMO_CURIOUS, EN_SPEECH, 0, 3600, 0, 0},
    {"Говорит: confused", "Асимметрия сильнее всего: один глаз почти на полторы высоты другого.",
     FACE_ST_SPEAKING, FACE_EMO_CONFUSED, EN_SPEECH, 0, 3600, 0, 0},
    {"Говорит: surprised", "Самая высокая поза, 56 из 64 пикселей, и самый мелкий зрачок.",
     FACE_ST_SPEAKING, FACE_EMO_SURPRISED, EN_SPEECH, 0, 3600, 0, 0},
    {"Говорит: sad", "Внешнее веко опущено, взгляд вниз, зрачок расширен.",
     FACE_ST_SPEAKING, FACE_EMO_SAD, EN_SPEECH, 0, 3600, 0, 0},
    {"Говорит: annoyed", "Зеркало грусти: опущено внутреннее веко. Это единственное отличие, и его проверяет тест.",
     FACE_ST_SPEAKING, FACE_EMO_ANNOYED, EN_SPEECH, 0, 3600, 0, 0},
    {"Говорит: sleepy", "Плоское веко на 43% высоты, взгляд вниз.",
     FACE_ST_SPEAKING, FACE_EMO_SLEEPY, EN_SPEECH, 0, 3600, 0, 0},

    {"Перебили", "Кнопка нажата посреди ответа: глаза распахиваются, зрачок сжимается, 350 мс — и отпускает.",
     FACE_ST_SPEAKING, FACE_EMO_HAPPY, EN_SPEECH, 1200, 4200, 1400, 0},

    {"Нет связи", "Веки сомкнуты до полоски. Нажатие приоткрывает их и отпускает: услышал, сделать ничего не могу.",
     FACE_ST_OFFLINE, FACE_EMO_NEUTRAL, EN_NONE, 3000, 5200, 0, 1400},

    {"Минута молчания", "Через 45 с последняя эмоция отпущена — лицо вернулось к neutral само.",
     FACE_ST_IDLE, FACE_EMO_EXCITED, EN_NONE, 50000, 3200, 0, 0},
    {"Полторы минуты", "Через 90 с — sleepy.",
     FACE_ST_IDLE, FACE_EMO_EXCITED, EN_NONE, 95000, 3200, 0, 0},
    {"Три минуты", "Через 180 с глаза закрыты. Полоска остаётся: пустая панель читается не как «спит», а как «сломалось».",
     FACE_ST_IDLE, FACE_EMO_EXCITED, EN_NONE, 185000, 3200, 0, 0},
};

#define NSCENES (sizeof(SCENES) / sizeof(SCENES[0]))

// ------------------------------------------------------------ fake loudness

// A syllabic envelope: phrases of about 1.7 s separated by a breath, syllables
// at roughly 4.5 Hz with the stress varying so it does not look mechanical.
static uint16_t synth_rms(uint32_t t, uint16_t peak) {
    if ((t % 2100u) > 1700u) return 0;

    const uint32_t syl = t % 222u;
    int32_t env = (int32_t)(syl < 111u ? syl : 222u - syl) * 255 / 111;

    static const int32_t stress[7] = {255, 178, 232, 120, 255, 158, 205};
    env = env * stress[(t / 222u) % 7u] / 255;

    return (uint16_t)((int32_t)peak * env / 255);
}

// ------------------------------------------------------------------ encoding

typedef struct {
    uint8_t *data;
    size_t len, cap;
} buf_t;

static void bput(buf_t *b, uint8_t v) {
    if (b->len == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 65536;
        b->data = realloc(b->data, b->cap);
        if (!b->data) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
    }
    b->data[b->len++] = v;
}

// One frame: alternating counts of unchanged and changed bytes, the changed
// ones followed by their XOR values. A run longer than 255 simply splits, with
// a zero-length run of the other kind between the halves.
static void encode_frame(buf_t *out, const uint8_t *prev, const uint8_t *cur) {
    uint8_t x[FACE_FB_BYTES];
    for (int i = 0; i < FACE_FB_BYTES; i++) x[i] = (uint8_t)(prev[i] ^ cur[i]);

    buf_t body = {0};
    size_t pos = 0;
    while (pos < FACE_FB_BYTES) {
        size_t z = 0;
        while (pos + z < FACE_FB_BYTES && x[pos + z] == 0 && z < 255) z++;
        bput(&body, (uint8_t)z);
        pos += z;
        if (pos >= FACE_FB_BYTES) break;

        size_t l = 0;
        while (pos + l < FACE_FB_BYTES && x[pos + l] != 0 && l < 255) l++;
        bput(&body, (uint8_t)l);
        for (size_t i = 0; i < l; i++) bput(&body, x[pos + i]);
        pos += l;
    }

    bput(out, (uint8_t)(body.len & 0xFF));
    bput(out, (uint8_t)((body.len >> 8) & 0xFF));
    for (size_t i = 0; i < body.len; i++) bput(out, body.data[i]);
    free(body.data);
}

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void write_base64(FILE *f, const uint8_t *d, size_t n) {
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        const uint32_t v = ((uint32_t)d[i] << 16) | ((uint32_t)d[i + 1] << 8) | d[i + 2];
        fputc(B64[(v >> 18) & 63], f);
        fputc(B64[(v >> 12) & 63], f);
        fputc(B64[(v >> 6) & 63], f);
        fputc(B64[v & 63], f);
    }
    if (i < n) {
        const uint32_t v = ((uint32_t)d[i] << 16) | ((i + 1 < n) ? ((uint32_t)d[i + 1] << 8) : 0);
        fputc(B64[(v >> 18) & 63], f);
        fputc(B64[(v >> 12) & 63], f);
        fputc((i + 1 < n) ? B64[(v >> 6) & 63] : '=', f);
        fputc('=', f);
    }
}

// --------------------------------------------------------------------- main

typedef struct {
    int start, count;
} range_t;

// A checksum over every rendered frame, in order. The page recomputes it from
// what its own decoder produced and complains if the two disagree - which is
// the only way to know that the XOR-delta stream survived base64, a text
// template and whatever a browser does to it.
typedef struct {
    uint32_t hash;
    uint32_t lit;
} check_t;

static void check_frame(check_t *c, const uint8_t *fb) {
    for (int i = 0; i < FACE_FB_BYTES; i++) {
        c->hash = (c->hash ^ fb[i]) * 16777619u;
        uint8_t b = fb[i];
        while (b) {
            c->lit += (uint32_t)(b & 1u);
            b >>= 1;
        }
    }
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <template.html> <out.html>\n", argv[0]);
        return 2;
    }

    buf_t stream = {0};
    range_t ranges[NSCENES];
    uint8_t prev[FACE_FB_BYTES];
    memset(prev, 0, sizeof(prev));
    int total = 0;
    check_t check = {2166136261u, 0};

    for (size_t s = 0; s < NSCENES; s++) {
        const scene_t *sc = &SCENES[s];

        // Every scene starts from a fresh face at the same clock, so the whole
        // page is reproducible: rerun this and you get byte-identical frames.
        face_t f;
        uint32_t t = 100000;
        face_init(&f, t);
        face_set_emotion(&f, sc->emotion, t);
        face_set_state(&f, sc->state, t);

        for (uint32_t w = 0; w < sc->warp_ms; w += WARP_MS) {
            t += WARP_MS;
            face_tick(&f, t);
        }
        // Aging the face must not also age the state change out of existence.
        face_set_state(&f, sc->state, t);

        ranges[s].start = total;
        bool startled = false, pressed = false;

        for (uint32_t e = 0; e < sc->capture_ms; e += TICK_MS) {
            t += TICK_MS;

            if (sc->startle_at && !startled && e >= sc->startle_at) {
                face_startle(&f, t);
                startled = true;
            }
            if (sc->press_at && !pressed && e >= sc->press_at) {
                face_set_button(&f, true, t);
                pressed = true;
            } else if (pressed && e >= sc->press_at + 200u) {
                face_set_button(&f, false, t);
            }

            if (sc->energy == EN_SPEECH) face_feed_energy(&f, synth_rms(e, 9000));
            else if (sc->energy == EN_VOICE) face_feed_energy(&f, synth_rms(e + 700u, 420));

            face_tick(&f, t);

            encode_frame(&stream, prev, f.fb);
            check_frame(&check, f.fb);
            memcpy(prev, f.fb, FACE_FB_BYTES);
            total++;
        }
        ranges[s].count = total - ranges[s].start;
    }

    // -- read the template, split it at the marker
    FILE *tf = fopen(argv[1], "rb");
    if (!tf) {
        fprintf(stderr, "cannot open template %s\n", argv[1]);
        return 1;
    }
    fseek(tf, 0, SEEK_END);
    const long tlen = ftell(tf);
    fseek(tf, 0, SEEK_SET);
    char *tpl = malloc((size_t)tlen + 1);
    if (!tpl || fread(tpl, 1, (size_t)tlen, tf) != (size_t)tlen) {
        fprintf(stderr, "cannot read template\n");
        return 1;
    }
    tpl[tlen] = 0;
    fclose(tf);

    // The marker swallows the `null` after it, so the template on disk is
    // valid JavaScript that an editor can still parse and lint.
    const char *marker = "/*__FACE_DATA__*/null";
    char *at = strstr(tpl, marker);
    if (!at) {
        fprintf(stderr, "template has no %s marker\n", marker);
        return 1;
    }

    FILE *out = fopen(argv[2], "wb");
    if (!out) {
        fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    fwrite(tpl, 1, (size_t)(at - tpl), out);

    fprintf(out,
            "{\"w\":%d,\"h\":%d,\"fps\":%d,\"frames\":%d,"
            "\"hash\":%u,\"lit\":%u,\"scenes\":[",
            FACE_W, FACE_H, FPS, total, check.hash, check.lit);
    for (size_t s = 0; s < NSCENES; s++) {
        fprintf(out,
                "%s{\"label\":\"%s\",\"note\":\"%s\",\"state\":\"%s\","
                "\"emotion\":\"%s\",\"start\":%d,\"count\":%d}",
                s ? "," : "", SCENES[s].label, SCENES[s].note,
                face_state_name(SCENES[s].state),
                face_emotion_name(SCENES[s].emotion), ranges[s].start,
                ranges[s].count);
    }
    fputs("],\"data\":\"", out);
    write_base64(out, stream.data, stream.len);
    fputs("\"}", out);

    fputs(at + strlen(marker), out);
    fclose(out);

    fprintf(stderr,
            "%d frames, %zu bytes encoded (%.1f%% of raw), %zu scenes, "
            "hash %u, %u lit pixels\n",
            total, stream.len,
            100.0 * (double)stream.len / ((double)total * FACE_FB_BYTES),
            (size_t)NSCENES, check.hash, check.lit);
    free(stream.data);
    free(tpl);
    return 0;
}
