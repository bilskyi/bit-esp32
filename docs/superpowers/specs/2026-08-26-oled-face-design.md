# A face on the OLED — design

A pair of animated eyes on the 0.96" SSD1306, driven by what the device is
actually doing and by what the assistant is actually saying.

Scope is deliberately narrow: **eyes and emotion only**. No mouth, and nothing
about the LAN/cloud build switch — that stays open as item 1 of the handoff.

## The decisions, and why

**Eyes only, no mouth.** The handoff planned a mouth moving to reply loudness.
Dropping it gives the eyes the full 64 pixels of height instead of about 24,
which is the difference between a lid slant reading as an emotion and reading as
a rendering artefact. Loudness still drives the face — it drives the eyes.

**Style: a filled rounded-rect eye with a cut-out pupil and a highlight.**
Compared against three alternatives in a browser mockup at true 128×64: solid
blocks (no gaze), hollow HUD outlines (reads as an instrument, and 1 px strokes
shimmer when they move), and an organic almond with a dithered iris (dithering
turns to mush at this size). The pupil is what earns its keep: it gives the face
a *direction*, so it can follow, glance away, and contract in surprise.

**The model picks the emotion.** A face that only knows `idle / listening /
thinking / speaking` is an indicator lamp with eyes. The interesting version
knows whether the answer it is giving is a happy one. So the LLM tags each reply
and the server forwards the tag, with a text heuristic as a fallback.

**Nothing about this may break the voice device.** The display is not wired yet.
The panel is probed at boot; absent, the face task never starts and the firmware
behaves exactly as it does today. This is not a nicety — the device works, and
two days went into making it work.

## Modules

Five pieces, and the important line is between the first two.

| File | Depends on | Responsibility |
|---|---|---|
| `main/face.c`, `face.h` | nothing | the 1-bit framebuffer, the drawing, the animation, the emotions |
| `main/ssd1306.c`, `.h` | `driver/i2c_master` | init sequence, per-page flush, bus probe |
| `main/face_main.c` | both | bring-up sketch: every state and emotion, no WiFi |
| `main/voice_main.c` | both | one more task, fed from the existing state and audio |
| `host/face_preview.c`, `face_test.c` | `face.c` only | runs the real renderer on the laptop |

`face.c` includes no ESP-IDF header, uses no float, and allocates nothing. That
is what lets the same code that drives the panel also drive the preview and the
tests on the laptop — which matters more than usual here, because the panel is
still in a box and the visual has to be judged somehow.

No floating point anywhere: the ESP32-C3 has no FPU, so every coordinate is
Q8 fixed point (256 = one pixel) and the curves come from an integer square root
and a 256-entry sine table.

### Framebuffer

1024 bytes, in the panel's own layout: 8 pages of 128 columns, bit *n* of a byte
being row `page*8 + n`. Drawing straight into that format means the flush is a
memcpy, not a transpose.

Everything is drawn as horizontal spans. For each row, the shape gives one
`x0..x1` range, and every pixel in a span shares a page and a bit mask, so the
inner loop is `fb[base + x] |= mask`. A rounded rect is a span per row with the
corner arc from `isqrt`; an ellipse likewise; a slanted eyelid is a span per row
cleared from one side. This is both faster and simpler than per-pixel distance
tests, and the shapes stay smooth under sub-pixel animation.

## The face

Two eyes, 46×46 px at rest, centres at x = 34 and x = 94, y = 32. At the tallest
pose (`surprised`, 1.22×) an eye is 56 px, which leaves 4 px of margin.

### Pose

Nine numbers describe an eye, all Q8:

| | |
|---|---|
| `hs`, `vs` | scale of the eye box |
| `gx`, `gy` | gaze, as a fraction of available travel |
| `slant` | `> 0` inner lid down (angry), `< 0` outer lid down (sad) |
| `happy` | crescent cut away from below |
| `lid` | flat upper lid, `0` open, `256` shut |
| `pup` | pupil scale |
| `asym` | how much the two eyes differ, for a lopsided look |

Emotion is a row in a table of those. Blinking, breathing, gaze and speech
energy are modulations on top. Everything except saccades and blinks is
**slewed** toward its target with a first-order filter rather than snapped,
which is most of what separates a face from a slideshow.

### Emotions

Nine, and they are the whole vocabulary the server may send:

`neutral` `happy` `excited` `curious` `confused` `surprised` `sad` `annoyed`
`sleepy`

`asleep` and `startled` also exist, but as *state* poses the model cannot
choose.

### States

| State | Behaviour |
|---|---|
| idle | blink every 2.5–6 s, sometimes twice; random small saccades every 1.5–4 s; breathing at ±3%; emotion decays to `neutral` after 45 s, to `sleepy` at 90 s, to `asleep` at 180 s |
| listening | eyes widen, pupils dilate, gaze locks forward, saccades suppressed, blink rate halved; mic loudness widens the eyes and contracts the pupil a little — leaning in |
| thinking | gaze rolls up and away and re-aims every ~700 ms, pupil slightly small, one eye a touch narrower |
| speaking | the emotion pose, with reply loudness squashing the eyes per syllable, dilating the pupil and adding a small vertical bob |
| startled | 350 ms after the button interrupts a reply: eyes snap wide, pupil to 55%, gaze to centre, then decay into whatever comes next |
| asleep | socket down, or idle far too long: lids shut to a 3 px bar with slow breathing; a press opens them in 200 ms |

A blink is the **upper lid sweeping down** — `lid` driven to 256 over 110 ms,
held 60, back up over 130 — not a symmetrical squash. The bottom edge staying
put is what makes it read as an eyelid.

The `asleep` pose keeps a visible bar on purpose. A blank 128×64 panel does not
read as "asleep", it reads as "broken", and this project has already lost days
to symptoms that looked like dead hardware.

### Loudness

`face_feed_energy()` takes a raw block RMS and normalises it against a slowly
decaying peak envelope, so the eyes swing across their full range whether the
source is the microphone at −46 dBFS or a TTS reply near full scale. Without
that, one direction would barely move and the other would clip.

RMS is computed per 512-sample block — once every 32 ms, one `isqrt` — in
`audio_in_task` while listening and in `audio_out_task` while speaking.

## Protocol

One new server-to-device text frame:

```json
{"type": "emotion", "value": "curious"}
```

Sent immediately before the first `{"type":"state","value":"speaking"}`, so the
face is already right when the first word arrives. The device matches it by
substring, as it already does for `state` and `done`; no emotion name contains
`done`, `speaking` or `thinking`, so the existing matching stays unambiguous.

### Where the tag comes from

The persona prompt gains one rule: begin the reply with exactly one of the nine
names in square brackets. The server then:

1. Sniffs the head of the token stream — until a `]` appears within the first 24
   characters, or 24 characters arrive without a `[`.
2. Strips a leading `[word]` whether or not `word` is known, so the tag can
   never reach TTS and be read aloud.
3. Also strips stray `[word]` anywhere in a sentence, as insurance.
4. Falls back to a heuristic on the first sentence when there is no usable tag:
   ends in `?` → `curious`, contains `!` → `excited`, matches an apology or a
   "don't know" phrase in any of the three languages → `sad`, otherwise
   `neutral`.

Step 3 is not paranoia. edge-tts happily pronounces "curious"; the handoff
already records that the model emits characters nobody expected.

## Timing

| | |
|---|---|
| Frame rate | 25 fps |
| I2C clock | 400 kHz, a compile-time knob |
| One full flush | ~23 ms of bus time at 400 kHz, against a 40 ms frame |
| Dirty pages | only changed pages are written, so `asleep` costs almost nothing |
| Task | priority 2, stack 3072, below audio and the button |

400 kHz is the datasheet figure. Most modules run happily at two to three times
that, which would cut the flush to under 10 ms — but that gets measured on the
bench before it gets believed, not the other way round.

The face task must never be able to stall audio. It owns the panel and nothing
else; it reads `s_state`, the energy counters and the button through plain
volatile reads, and writes nothing the audio path reads.

## Wiring

Four wires. SDA on **GPIO 0**, SCL on **GPIO 1**, plus power and ground. Both
pins were already reserved for this in the handoff.

## Verification

Without the panel, the visual is judged two ways:

- `host/face_test.c` — the real renderer, compiled with `cc`, asserting
  invariants: every pixel inside bounds; the framebuffer never entirely blank in
  any state; every emotion producing a distinguishable frame; a blink actually
  closing and reopening; the energy normaliser reaching both rails from either
  input level.
- `host/face_preview.c` — the real renderer again, run over a scripted timeline
  of every state and emotion, emitting a self-contained HTML player. Frames are
  XOR-delta and run-length encoded, which is what makes a minute of animation
  small enough to publish and open on a phone.

On the device: `idf.py -DSKETCH=face` shows the whole repertoire with no WiFi
and no server, so wiring is confirmed in one flash rather than inside a
WebSocket session. That is the same reason `capture` and `playback` exist.

Server side: unit tests for tag splitting, the unknown-tag case, stray-tag
stripping and each heuristic branch, plus a session test asserting that an
emotion frame precedes the speaking state and that no bracket ever reaches TTS.

## Deliberately not done

- The mouth.
- The LAN/cloud build switch — untouched, still item 1.
- Removing the instrumentation logging — item 3, and it wants the radio fixed
  first.
- Gaze that tracks a person. There is no camera, and two microphones would be
  needed to infer direction from sound.
