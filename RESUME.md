# Voice companion — handoff

Push-to-talk AI voice companion. The ESP32-C3 is a thin audio terminal; all the
intelligence is a FastAPI server. The full design is in the project spec — this
file is what a new session needs that the code does not say.

**The device works.** Real conversations, switching between Russian, Ukrainian
and English mid-session, interruptible mid-reply, with an LED showing state.
The server is deployed. What is not solved is the radio link, and that is where
the last two days went.

**It also has a face now** — a pair of animated eyes for the OLED, written and
tested but never yet shown on glass, because the panel is not wired. Four
wires and one flash will tell. See "The face" below.

---

## Where things run

| | |
|---|---|
| Server, cloud | `wss://voice-server-production-e023.up.railway.app/ws` |
| Railway | project `voice-companion`, service `voice-server` |
| Auth | `DEVICE_TOKEN`, 43 chars, in Railway vars and `firmware/main/secrets.h` |
| Database | SQLite on a Railway volume at `/data/voice.db` |
| Server, local | `uv run uvicorn server.main:app --host 0.0.0.0 --port 8000` |
| Device | `/dev/cu.usbmodem1101`, WiFi `Xiaomi_CFB8`, gets `192.168.31.109` |

`firmware/main/secrets.h` is git-ignored and holds the WiFi password, the device
token and `SERVER_URI`. Only that last line differs between LAN and cloud.

## Hardware

| Signal | GPIO | Part |
|---|---|---|
| BCLK | 4 | INMP441 `SCK` + MAX98357A `BCLK` |
| WS / LRCLK | 5 | INMP441 `WS` + MAX98357A `LRC` |
| Mic data in | 6 | INMP441 `SD` |
| Amp data out | 7 | MAX98357A `DIN` |
| Amp shutdown | 10 | MAX98357A `SD` |
| Button to GND | 3 | push-to-talk |
| Status LED | 8 | onboard, **lights on LOW** |
| I2C SDA | 0 | SSD1306 `SDA` — **not connected yet** |
| I2C SCL | 1 | SSD1306 `SCL` — **not connected yet** |

Mic on **3V3**, amp on **5V**. The OLED takes 3V3 too. Do not use GPIO 9
(BOOT), 18/19 (USB), 2 (strapping).

GPIO 0 and 1 are the 32 kHz crystal pins, which is only a problem if the RTC
is told to use one. `CONFIG_RTC_CLK_SRC_INT_RC=y`, so they are genuinely free.

## Measured, so nobody re-derives it

| | |
|---|---|
| Speech recognition | correct essentially always, 200-570 ms |
| Upload | 3-8 ms per KB when the link is healthy |
| Playback | real time, nine consecutive turns with zero underruns |
| First audio | median 1.5 s local, 1854 ms through Railway |
| Server memory | 31 MB idle, 74 MB through a conversation |
| Free heap on device | 92-98 KB with TLS on, **before the face** |
| Codec | IMA ADPCM both directions, 4:1, verified against `audioop` |
| Emotion tag | model tagged 8/8 replies, three languages, nothing leaked |
| Tag latency cost | 60-110 ms to the first sentence, measured against a control |
| `voice` image | 1013 KB, 51% of the app partition free |

---

## The blocking problem: the radio

Everything else is tuned. This is what stands between the current state and a
device that simply works.

Measured with the board in the laptop's USB port:

    laptop  -> router:   0% loss,  3.6 ms
    laptop  -> Railway:  0% loss,   47 ms
    laptop  -> device:  67% loss, 2200 ms

Only the device's own link is broken, and it reports -49 to -63 dBm while it
happens — it hears the access point perfectly well. Strong signal with heavy
loss is interference, not distance.

Moving the board half a metre away on a USB extension **fixed the loss** (0%,
90 ms average; the 90 ms is the power-save listen interval and is expected).
But the board then vanished from USB entirely: no serial port, 100% ping loss.
That is the extension cable, not firmware — firmware cannot remove a device
from the USB bus. Thin power wires sag under WiFi transmit bursts.

**Best next move: power the board from a phone charger.** The server is in the
cloud now, so the laptop is only needed to flash and to read logs. That removes
the USB 3.0 noise, the voltage sag, and lets the board sit near the router.

Diagnosing this takes ten seconds and no firmware:

```bash
ping -c 20 192.168.31.109
```

Zero loss means the link is fine and the fault is elsewhere. Tens of percent
means radio, and no amount of code will fix it.

---

## The face

Built, tested, and **never yet seen on a real panel**. Everything below is
either verified on the laptop or verified by the compiler; nothing is verified
by looking at glass, because there is no glass connected.

### What it does

A pair of eyes, no mouth. Dropping the mouth gave the eyes all 64 pixels of
height instead of about 24, which is the difference between a lid slant reading
as an emotion and reading as a rendering artefact. Loudness still drives the
face — it drives the eyes.

| State | Eyes |
|---|---|
| idle | blink every 2.5–6 s, sometimes twice; random saccades; breathing; after 45 s the last emotion is let go, at 90 s sleepy, at 180 s asleep |
| listening | wide, pupils dilated, gaze locked forward; **your** loudness widens them |
| thinking | rolls up and away, re-aims every ~700 ms, one eye a touch narrower |
| speaking | the emotion, with the reply's own loudness squashing the eyes per syllable |
| interrupted | 350 ms flinch: snap wide, pupil to 55%, then decay |
| no connection | lids shut to a bar; **a press cracks them open and lets them fall** |

Nine emotions the server may send: `neutral happy excited curious confused
surprised sad annoyed sleepy`. The model picks one per reply.

### How it is put together

| File | Depends on | Does |
|---|---|---|
| `main/face.c` | **nothing** | framebuffer, drawing, animation, emotions |
| `main/ssd1306.c` | `driver/i2c_master` | init, probe, per-page flush |
| `main/face_main.c` | both | the `face` bring-up sketch |
| `host/face_test.c` | `face.c` | 469 invariant checks |
| `host/face_preview.c` | `face.c` | the animation, as a web page |
| `server/emotion.py` | — | tag off the stream, heuristic fallback |

`face.c` includes no ESP-IDF header and uses no float — the C3 has no FPU, so
everything is Q8 fixed point with an integer square root. That is what lets the
same code run on the laptop, which mattered a lot while the panel was in a box.

### Verified

- 469 host checks, 0 failures. `cd firmware/host && make test`.
- All four sketches build with **zero warnings**.
- 189 server tests. One reads the emotion names straight out of `face.c`, because
  the device matches them by substring and a rename would not raise anywhere —
  the face would just quietly stop changing.
- End to end against the real pipeline: `emotion happy` arrives at the same
  instant as `state speaking`, 370 ms before the first audio.
- gpt-oss tagged 8/8 replies across three languages with nothing leaking into
  the spoken text.

### Not verified, and only the panel can settle it

1. **Whether anything appears at all.** Wire it and run `idf.py -DSKETCH=face`.
   The sketch says which of three things to check if the bus stays silent.
2. **Frame time.** 23 ms for a full frame at 400 kHz is arithmetic. The sketch
   prints min/avg/max every ten seconds against the 40 ms budget. Most modules
   run happily at two or three times the datasheet clock, which would cut it to
   under 10 ms — measure before believing it.
3. **Free heap.** It was 92–98 KB with TLS on. The face adds roughly 8 KB
   (a 1 KB shadow, a 1 KB scratch, `face_t`, a 3 KB stack, the I2C driver).
   TLS handshakes want tens of KB transiently, so watch the number in the
   `socket disconnected` line.

**The device does not need any of this.** `voice_main` probes the bus before
WiFi; if nothing answers at 0x3C or 0x3D it logs one line and never starts the
task. Flashing today, with no display attached, behaves exactly as yesterday.

### Looking at it without hardware

```bash
cd firmware/host && make          # test, then write build/face-preview.html
open build/face-preview.html      # 17 scenes, scrubbable, frame by frame
```

Also published, for a phone:
<https://claude.ai/code/artifact/15e55cb6-6593-450a-be68-5ded3f50015d>

It runs the real `face.c`, not a reimplementation — a JavaScript copy would
have drifted from the original within a day. The page recomputes the
generator's checksum over its own decoded frames, so a payload that did not
survive the trip says so rather than showing plausible nonsense.

---

## Agreed next, in order

### 1. Build-time switch between LAN and cloud

The user's complaint, and it is fair: local felt instant and always worked;
through Railway it is ~300 ms slower and drops far more often. TLS needs
several round trips in succession, so it falls apart on a lossy link where
plain TCP scraped through, and it adds a DNS dependency.

Wanted: `idf.py -DSERVER=lan` and `-DSERVER=cloud`, choosing between two URIs in
`secrets.h`. Roughly twenty minutes. Do not make them pick one forever.

### 2. Cleanup, once the link is sound

- Remove instrumentation: `send of ... took`, `starved`, `rssi`, reply logging.
- `capture` uses mono slot mode while `voice` uses stereo. Both work, but they
  should agree before someone reads the wrong one and believes it.
- Facts on Railway start empty — the volume is new, so the assistant has to
  learn the user's name (Катерина) again. Nothing to fix, just surprising.

---

## Resuming

```bash
# server, local
uv run uvicorn server.main:app --host 0.0.0.0 --port 8000   # DEBUG_DUMP_PCM=1 optional

# firmware
deactivate 2>/dev/null; unset VIRTUAL_ENV      # ESP-IDF refuses to share a venv
. ~/esp/esp-idf/export.sh                      # source it, never pipe it
cd firmware && idf.py -DSKETCH=voice -p /dev/cu.usbmodem1101 flash monitor

# sketches: voice, face, capture, playback, diag, stream, mute, led
# mute silences the amplifier if it is ever left making noise
# face needs no WiFi and no server: use it the moment the panel is wired
```

```bash
# the eyes, on the laptop
cd firmware/host
make test        # 469 invariants, no board needed
make preview     # build/face-preview.html, every state and emotion
```

```bash
# cloud
railway logs --lines 50
railway up --detach -m "..."
curl -s https://voice-server-production-e023.up.railway.app/healthz
```

If `SERVER_URI` points at the LAN server, check `ipconfig getifaddr en0` first —
it is a literal address and will not follow the laptop onto a new network.

The user steps away from the desk. Call them back with a sound rather than
waiting silently:

```bash
afplay /System/Library/Sounds/Ping.aiff
```

---

## Traps already paid for

- **Restarting the server strands the device.** A supervisor now rebuilds the
  client after 15 s offline and reboots after 90 s, but a board stuck from
  before that landed still needs a manual reset.
- **Only one process may hold the serial port.** Two monitors give garbled
  output and a flash fails with `exit=2`.
- **A monitor backgrounded with `&` inside a foreground command dies with its
  parent.** Several test windows produced empty logs this way and were misread
  as "the user did not press the button".
- **ESP-IDF and this project's `uv` venv collide.** On *Cannot import module
  esp_idf_monitor*:
  `unset VIRTUAL_ENV && python3 $IDF_PATH/tools/idf_tools.py install-python-env`
- **`idf.py ... | tail` hides failures** — the exit status is `tail`'s. This
  once reported a failed build as a clean pass.
- **`railway up` uploads a directory, not a commit.** `.railwayignore` must
  exclude `firmware/`, because `secrets.h` holds the WiFi password and
  git-ignoring it is not enough.
- **Watch the disk.** A build once failed on `No space left on device`.
- **A blank OLED reads as broken, not as asleep.** A blink that closes fully
  leaves 128x64 of nothing, and this project has already lost days to symptoms
  that looked like dead hardware. The lid stops just short of shut so a lash
  line always remains, and a test fails if it ever does not.
- **On one bit, a pupil that reaches the eye's edge stops being a pupil.** Its
  black joins the black around the eye and the shape reads as a helmet. The
  first `happy` did exactly that and looked like a scowl; the lower lid now
  stops two pixels short of the pupil.

## What actually caused the trouble, in order of how long it hid

1. **The model was thinking instead of answering.** gpt-oss reasons before it
   replies and the reasoning is billed against the same budget: at
   `max_tokens=150` an open question gave 511 characters of reasoning and 12 of
   answer. `reasoning_effort=low` fixed every "long questions are ignored"
   symptom. Two days were spent looking at the radio for this.
2. **Power save parked the radio 307 ms at a time.** `listen_interval = 1` took
   the average ping from 836 ms to 108 ms. Turning power save *off* is worse and
   was tried twice — one 1 KB send took 32 seconds and the radio then failed to
   associate at all. Do not try it a third time.
3. **Raw PCM did not fit the link.** ADPCM took the uplink from 32 KB/s to
   8 KB/s and a reply from 340 KB to 86 KB.
4. **A bounded send timeout tore down healthy connections.** The value goes
   straight to the transport's `poll_write`, and an expired poll makes
   `esp_transport_write()` return 0, which the client treats as fatal.
5. **Pacing starved the buffer it was meant to protect** — 1.2 s of lead against
   six seconds of buffer.
6. **edge-tts returns silence, not an error**, for typographic characters
   (U+202F, non-breaking hyphen) that the model emits constantly, and for a
   voice asked to speak a language it does not have.
7. **Debug logging broke what it measured.** Transport logging at DEBUG blocked
   the socket task through the synchronous USB console.

## Wrong theories, for the record

WiFi power save (twice), the shared send/receive lock, unpaced audio, buffer
overflow, a per-voice TTS fault, heat damage to the microphone, USB 3.0 noise
(partly right — but the extension cable then caused worse). Each was reasoned
from how the system ought to behave; each produced a change that helped a
little or not at all.

What found the real cause every single time was printing a number: the client's
own error text, the duration of each send, the size of the reply, the
distribution of first-chunk latency, `ping`. Measure before theorising.
