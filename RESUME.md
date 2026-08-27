# Voice companion — handoff

Push-to-talk AI voice companion. The ESP32-C3 is a thin audio terminal; all the
intelligence is a FastAPI server. The full design is in the project spec — this
file is what a new session needs that the code does not say.

**The device works.** Real conversations, switching between Russian, Ukrainian
and English mid-session, interruptible mid-reply, with an LED showing state.
The server is deployed. What is not solved is the radio link, and that is where
the last two days went.

**It also has a face now** — a pair of animated eyes on the OLED, wired,
flashed and confirmed on real glass. Five conversations through it with zero
dropped blocks and zero send failures, including an interruption. See "The
face" below.

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
| I2C SDA | 0 | SSD1306 `SDA` |
| I2C SCL | 1 | SSD1306 `SCL` |

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
| Free heap on device | 172 KB at boot, **55 KB mid-conversation** — see below |
| Codec | IMA ADPCM both directions, 4:1, verified against `audioop` |
| Emotion tag | model tagged 8/8 replies, three languages, nothing leaked |
| Tag latency cost | 60-110 ms to the first sentence, measured against a control |
| `voice` image | 1013 KB, 51% of the app partition free |
| OLED | answers at **0x3C**, found 232 ms into boot, before the radio |
| Face heap cost | **4660 B**, printed by the device at boot |
| Face frame time | 92 µs / 14-19 ms / 24.4 ms against a 40 ms budget at 400 kHz |

**The old 92-98 KB free-heap figure did not reproduce.** It is 55 KB during a
turn now, and the face is not the reason: the device prints its own cost at
boot and it is 4.7 KB. Between boot and a live conversation the heap goes from
172 KB to 55 KB, which is WiFi, mbedTLS and the full root bundle — the same
appetite the traps section below already records. Why the earlier number was so
much higher is unexplained; the old build is gone. 55 KB is evidently enough:
TLS handshakes succeed and five turns ran with no failures.

The frame time is worth reading twice. 24.4 ms is the worst case, a full
1 KB frame, and it matches the 23 ms the arithmetic predicted. The 92 µs
minimum is a frame where no page changed at all, so the flush returns without
touching the bus. Raising `FACE_I2C_HZ` past the datasheet's 400 kHz is
possible but has not been needed.

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

Wired, flashed, and working. The panel answers at 0x3C and the eyes are alive
on it, inside the full `voice` firmware alongside WiFi and TLS.

### What it does

A pair of eyes, no mouth. Dropping the mouth gave the eyes all 64 pixels of
height instead of about 24, which is the difference between a lid slant reading
as an emotion and reading as a rendering artefact. Loudness still drives the
face — it drives the eyes.

| State | Eyes |
|---|---|
| powering on | two sparks shoot outward into a bar across the panel, the bar warms up and collapses back into eyes, then **how far the eyes open is how far the device has got** — see below |
| idle | blink every 2.5–6 s, sometimes twice; random saccades; breathing; after 45 s the last emotion is let go, at 90 s sleepy, at 180 s asleep |
| listening | wide, pupils dilated, gaze locked forward; **your** loudness widens them |
| thinking | **squints hard** (46 → 38 px), brow furrows, gaze holds up and away and rolls to a new place every 1.4–2.4 s |
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
| `host/face_test.c` | `face.c` | 487 invariant checks |
| `host/face_preview.c` | `face.c` | the animation, as a web page |
| `server/emotion.py` | — | tag off the stream, heuristic fallback |

`face.c` includes no ESP-IDF header and uses no float — the C3 has no FPU, so
everything is Q8 fixed point with an integer square root. That is what lets the
same code run on the laptop, which mattered a great deal while the panel was
still in its box and is still the fastest way to change how the eyes look.

### Verified on the bench

- The eyes are alive on the panel, in the `voice` firmware, with WiFi and TLS up.
- Five conversations through it: **0 dropped blocks, 0 send failures**, slowest
  send 5-6 ms, playback never starved, 0 bytes dropped. One of them interrupted
  mid-reply, so the flinch fired too.
- The face costs 4660 B of heap. The device prints it at boot.
- Frame time 92 µs / 14-19 ms / 24.4 ms against a 40 ms budget.
- **The face did not make the radio worse**: 66.7% ping loss with it running,
  against the 67% recorded before it existed. I2C switching GPIO 0 and 1 next
  to the antenna was a fair thing to suspect, and it was not the cause.

### Powering on, and why it is not decoration

Five seconds pass between the panel answering and the socket connecting, and
before this the face said nothing about them: the eyes appeared open, shut
immediately because `s_ws` was still `NULL`, lay shut, and opened again. It
read as "woke up, changed its mind, woke up again".

Now the openness *is* the progress. `face_boot_stage()` takes three values and
they are set by the three places that already log those very milestones:

| | | |
|---|---|---|
| `FACE_BOOT_PANEL` | 232 ms | the display answered; eyes are a slit, pupils sweeping |
| `FACE_BOOT_WIFI` | 2127 ms | an IP arrived; eyes half open |
| `FACE_BOOT_LINK` | 5002 ms | socket connected; eyes open, one blink, a look around |

That is the whole payoff: **stuck on WiFi and stuck on the server look
different, and both look different from working.** Measured on the preview,
the three hold at 964, 1896 and 3200 lit pixels and do not drift. A boot
animation on a timer would have shown the same thing in all three cases.

The stage only moves forward, so a socket that dies later is the ordinary
offline state rather than a claim that the device rebooted. A test checks that.

Three things the sequence is built on, all of which fall out of the renderer
rather than being added to it:

- `hs` far above 1 makes the two eyes overlap into a single bar across the
  panel. That is the one shape in the whole vocabulary that cannot be mistaken
  for a mood, so "power came on" is unambiguous from the first frame — where
  two slits would have been indistinguishable from "no connection".
- No pupil is drawn below 14 px of eye height, so the pupils vanish for the bar
  and arrive by themselves as the eyes grow.
- `vs` is floored at 12, so the warm-up flicker can thin the bar but never
  blank it. Found by measuring rather than by reading: the first attempt tried
  to blank a frame and the clamp quietly refused.

Width collapses before height grows, and deliberately not on one curve. Run
them together and the pupils arrive while the shape is still a full-width bar,
which reads as a letterbox with two holes rather than as eyes being born.

### Why "thinking" reads, and what nearly went wrong

The first version moved only the pupil, and it did that well: measured, it held
the gaze up 82% of the time and swung the pupil 10 px sideways. It still read
as idle. The reason was one number nobody would have guessed — its eyes were
**46.3 px tall against idle's 45.7**, so the two states had the same
silhouette and the pupil was carrying the whole message alone.

Shape is read before pupils. The intuitive fix — make the gaze travel further —
would have changed nothing. What fixed it was squinting: 38 px against idle's
46, plus a brow furrow and a gaze that re-aims half as often, because a face
flicking about once a second reads as nervous rather than thoughtful.

`face_test.c` now fails if thinking is not at least 5 px shorter than idle.

### Verified off the bench

- 487 host checks, 0 failures. `cd firmware/host && make test`.
- All four sketches build with **zero warnings**.
- 189 server tests. One reads the emotion names straight out of `face.c`, because
  the device matches them by substring and a rename would not raise anywhere —
  the face would just quietly stop changing.
- End to end against the real pipeline: `emotion happy` arrives at the same
  instant as `state speaking`, 370 ms before the first audio.
- gpt-oss tagged 8/8 replies across three languages with nothing leaking into
  the spoken text.

**The device still does not need any of it.** `voice_main` probes the bus before
WiFi; if nothing answers at 0x3C or 0x3D it logs one line and never starts the
task. Unplug the panel and the firmware behaves exactly as it did before.

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
make test        # 487 invariants, no board needed
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
- **The same error also has a second cause, and that remedy makes it worse.**
  `export.sh` picks the first `python3` on PATH and then looks for an IDF
  environment built for *that* version. The project venv is 3.12 and the built
  environment is `idf5.3_py3.14_env`, so if `.venv/bin` is anywhere on PATH -
  `VIRTUAL_ENV` unset or not - it hunts for `idf5.3_py3.12_env` and fails.
  Running `install-python-env` then builds a third environment instead of
  fixing anything. Check `python3 --version` before believing the message, and
  drop the venv from PATH:

  ```bash
  export PATH=$(echo "$PATH" | tr ':' '\n' | grep -v "Documents/BIT/.venv" | paste -sd: -)
  ```
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
