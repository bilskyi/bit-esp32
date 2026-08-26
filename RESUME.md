# Voice companion — handoff

Push-to-talk AI voice companion. The ESP32-C3 is a thin audio terminal; all the
intelligence is a FastAPI server. The full design is in the project spec — this
file is what a new session needs that the code does not say.

**The device works.** Real conversations, switching between Russian, Ukrainian
and English mid-session, interruptible mid-reply, with an LED showing state.
The server is deployed. What is not solved is the radio link, and that is where
the last two days went.

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
| *free for I2C* | 0, 1 | proposed SDA/SCL for the OLED |

Mic on **3V3**, amp on **5V**. Do not use GPIO 9 (BOOT), 18/19 (USB), 2
(strapping).

## Measured, so nobody re-derives it

| | |
|---|---|
| Speech recognition | correct essentially always, 200-570 ms |
| Upload | 3-8 ms per KB when the link is healthy |
| Playback | real time, nine consecutive turns with zero underruns |
| First audio | median 1.5 s local, 1854 ms through Railway |
| Server memory | 31 MB idle, 74 MB through a conversation |
| Free heap on device | 92-98 KB with TLS on |
| Codec | IMA ADPCM both directions, 4:1, verified against `audioop` |

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

## Agreed next, in order

### 1. Build-time switch between LAN and cloud

The user's complaint, and it is fair: local felt instant and always worked;
through Railway it is ~300 ms slower and drops far more often. TLS needs
several round trips in succession, so it falls apart on a lossy link where
plain TCP scraped through, and it adds a DNS dependency.

Wanted: `idf.py -DSERVER=lan` and `-DSERVER=cloud`, choosing between two URIs in
`secrets.h`. Roughly twenty minutes. Do not make them pick one forever.

### 2. A face on the 0.96" OLED

The user has an SSD1306 128x64 I2C display and wants the assistant to have a
face. Researched; conclusions:

- **Do not pull Arduino into the project.**
  [FluxGarage RoboEyes](https://github.com/FluxGarage/RoboEyes) is the
  well-known animated-eyes library but is Arduino/C++ on Adafruit GFX with no
  ESP-IDF port; same for [Irisoled](https://github.com/orji123/Irisoled) and
  [RobotEyes-animation](https://github.com/pstarz7/RobotEyes-animation-for-Arduino).
  There is a [MicroPython port](https://github.com/mchobby/micropython-roboeyes),
  which does not help us either.
- **Use** [`espressif/esp_lcd`](https://components.espressif.com/components/espressif/esp_lvgl_port/versions/2.3.2/examples/i2c_oled?language=en)
  with its stock SSD1306 driver, or
  [`k0i05/esp_ssd1306`](https://components.espressif.com/components/k0i05/esp_ssd1306),
  and draw the eyes directly. RoboEyes' logic is rounded rectangles plus blink
  timers — a couple of hundred lines, and it stays in our C.
- **It fits the existing architecture exactly.** `led_task` already ticks every
  100 ms off `s_state`; the face is the same shape.

| State | Face |
|---|---|
| idle | calm eyes, occasional blink, slow drift |
| listening | wide open, pupils forward |
| thinking | glance up and away |
| speaking | **mouth moving in time with loudness** |
| no connection | eyes closed, asleep |

The speaking case is the one worth doing properly: `audio_out_task` already
holds the decoded samples, so a per-block RMS costs almost nothing and the
mouth tracks real speech instead of faking it.

Wiring: four wires — power, ground, **SDA on GPIO 0, SCL on GPIO 1**.

### 3. Cleanup, once the link is sound

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

# sketches: voice, capture, playback, diag, stream, mute, led
# mute silences the amplifier if it is ever left making noise
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
