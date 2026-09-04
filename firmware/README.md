# Firmware — bring-up sketches

Two sketches, matching steps 2 and 3 of the build order. Neither touches WiFi.
They exist so that a dead microphone or a miswired amp is caught here, on a
serial console, rather than three layers later inside a WebSocket session.

| Sketch | Step | Proves |
|---|---|---|
| `capture` | 2 | the INMP441 produces sound-dependent samples |
| `playback` | 3 | the MAX98357A makes a clean tone, switched by the mute pin |

## Build and flash

```bash
deactivate 2>/dev/null; unset VIRTUAL_ENV    # see the warning below
. ~/esp/esp-idf/export.sh
cd firmware
idf.py set-target esp32c3

idf.py build                                  # capture (default)
idf.py -DSKETCH=playback build                # playback

idf.py -p /dev/cu.usbmodem* flash monitor
```

> **Do not run `idf.py` with this project's `uv` virtualenv active.** ESP-IDF
> keeps its own Python environment, and it refuses to build one while another
> venv is active — during setup it silently skips creating it, then every later
> `idf.py` fails with *"Cannot import module esp_idf_monitor"*. The server half
> of this repo activates a venv, so the two collide easily. If it happens:
>
> ```bash
> unset VIRTUAL_ENV
> python3 $IDF_PATH/tools/idf_tools.py install-python-env
> ```
>
> Also note `. export.sh` must be *sourced*, never piped — `. export.sh | tail`
> runs it in a subshell and silently discards the PATH it just set up.

macOS does not get `cmake` and `ninja` from `install.sh`. If `idf.py` says
cmake is missing:

```bash
python3 $IDF_PATH/tools/idf_tools.py install cmake ninja
```

`Ctrl-]` exits the monitor. Switching `SKETCH` changes which `*_main.c` is
compiled; the pin definitions and `sdkconfig` are shared, so the two sketches
cannot drift apart on wiring.

## Wiring this sketch assumes

| Signal | GPIO | INMP441 |
|---|---|---|
| BCLK | 4 | SCK |
| WS | 5 | WS |
| Mic data in | 6 | SD |
| Button (to GND) | 3 | — |
| Button B (to GND) | 20 | — |

`L/R` to GND, `VDD` to **3V3**. The amp is not involved yet — leave it
unpowered, or at least leave GPIO10 disconnected, so nothing can make noise
while you are trying to read numbers.

## What good looks like

Measured on real hardware, INMP441 at roughly 30 cm:

```
rms     59  peak    164  dc    -8  raw   -891   -54.8 dBFS  btn up   <- quiet
rms    157  peak    475  dc    22  raw   3435   -46.4 dBFS  btn up   <- speech
```

The columns:

| Column | Meaning |
|---|---|
| `rms` | level after DC blocking — the number that should track your voice |
| `peak` | largest sample in the window; must stay well under 32767 |
| `dc` | residual offset after filtering; should hover near zero |
| `raw` | the mic's own offset before filtering; large and wandering is normal |

Three things matter, and none of them is the absolute value:

- **The idle floor is not zero.** A real microphone in a silent room produces
  noise — expect `rms` around 20–150. Exact zero is a wiring fault, not a
  quiet room.
- **Speech lifts it 8–18 dB.** Measured: median `rms` 59 quiet → 157 speaking,
  peaking near 475. That is roughly −46 dBFS, which is what the INMP441
  datasheet predicts for conversational volume (−26 dBFS at 94 dB SPL).
  **Do not expect `rms` in the thousands** — that would take ~100 dB SPL.
  If speech moves the number by only 1–2 dB, the mic is hearing its own noise
  floor and not you.
- **`dc` stays near zero while `raw` wanders.** That contrast is the DC blocker
  working. If `dc` starts tracking `rms` in magnitude, the filter is not
  running and the meter is measuring offset instead of sound.

## Why the DC blocker exists

The INMP441 carries a large, slowly drifting DC offset — measured on this
hardware at about a quarter of full scale, wandering over seconds. Shift
straight to int16 and compute a level from it, and you measure the offset, not
the room: a silent bench reads −10 dBFS.

`capture_main.c` runs a one-pole high-pass (corner ≈20 Hz — below speech, above
the drift) in the 24-bit domain, before the shift to int16, so no headroom is
spent. This is not a diagnostic hack: the networked firmware needs it too.
Handing the STT a waveform that is mostly offset wastes dynamic range.

## Failure signatures

| What you see | Almost always means |
|---|---|
| `rms 0  peak 0`, and the all-zero warning fires | Data line dead. Check `SD`→GPIO6, and that the mic has 3V3. |
| Zeros with a correct-looking `SD` wire | `L/R` floating or tied to 3V3. It must go to GND for the left slot this sketch reads. |
| `peak` pinned at 32767 constantly | Reading the wrong bits — the `>> 16` is picking up the empty low byte of the slot, or slot width is not 32-bit. |
| Numbers look plausible but never react to sound | `SD` floating: you are reading pin noise. Wiggle the wire; real noise changes, floating noise usually does not. |
| Loud hiss, `dc` drifting hundreds | Mic on 5V instead of 3V3, or a long unshielded `SD` run next to BCLK. |
| `btn DOWN` always | GPIO3 shorted to GND, or wired to the wrong pin. |
| `btn up` always, even pressed | Button not reaching GPIO3, or wired to 3V3 instead of GND. |
| Boots then resets repeatedly | Amp is connected and drawing on 3V3. The MAX98357A needs 5V and peaks near 700 mA. |

## Do not skip this

The next steps assume the microphone works. If you flash step 3 or step 4 with
a dead mic, the symptom is "the assistant never answers", which has roughly
fifteen plausible causes across two machines and a network. Here it has three,
and they are all on this table.

---

# Step 3 — playback

```bash
idf.py -DSKETCH=playback build && idf.py -p /dev/cu.usbmodem* flash monitor
```

A 440 Hz tone for one second, one second of silence, forever:

```
tone  (mute pin high)
silence (mute pin low)
```

## Extra wiring for this sketch

| Signal | GPIO | MAX98357A |
|---|---|---|
| BCLK | 4 | BCLK |
| WS | 5 | LRC |
| Amp data out | 7 | DIN |
| Amp shutdown | 10 | SD |

The amp runs from **5V**, not 3V3, and peaks near 700 mA. Powering it from the
board's 3V3 regulator is the usual cause of a brownout reset loop.

## What good looks like

A steady, clean 440 Hz tone. No buzz under it, no click at either end, and
**silence during the gap** — not a hiss that stops when you touch the wire.

The tone ramps up over the first 64 ms and down over the last. That ramp is
deliberate: a waveform starting at full amplitude is a step, and a step is a
click.

## Mute discipline

The point of this sketch, and the part that carries into step 4:

- The I2S channel is initialised **once**, at boot, and never re-initialised.
- Playback state is switched **only** by GPIO10.
- GPIO10 is driven low before the first sample exists, and returns low whenever
  audio is not playing.
- The 150 ms wait before muting lets the DMA ring drain. Muting early cuts the
  tail off the last word.

If you find yourself calling `i2s_channel_disable()` between utterances, that
is the pop.

## Failure signatures

| What you see | Almost always means |
|---|---|
| Board reboots when the tone starts | Amp on 3V3, or a supply that cannot deliver 700 mA. |
| Continuous hiss, no tone | `DIN`→GPIO7 not connected; the amp is on but receiving nothing. |
| Tone plays during the gap too | GPIO10 not reaching `SD`, so the amp never shuts down. |
| Nothing at all, ever | `SD` tied low in hardware, or GPIO10 on the wrong pin. |
| Click at each tone boundary | The ramp is not being applied — check `fill_tone` gains. |
| Tone is there but distorted or thin | Wrong slot width, or `AMPLITUDE` raised near full scale. |
| Tail of the tone is clipped off | `DRAIN_MS` too short for your DMA configuration. |

## Not yet implemented

Step 4: WiFi, the WebSocket client, the ring buffer, and the three-task split
(`audio_in` / `net` / `audio_out`). Both halves above become tasks there; the
mute rule and the `>> 16` shift carry over unchanged.
