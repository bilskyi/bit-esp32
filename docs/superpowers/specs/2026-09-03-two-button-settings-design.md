# Settings on the device itself — design

There is a second button now, wired to GPIO 20 and pulled to ground like the
first. This is what it is for: volume, screen brightness, the face's resting
expression, and the way into WiFi setup — on the panel, with no phone and no
laptop.

It also ends an overload. One button carries four meanings today: held it
records a question, pressed during a reply it interrupts, tapped five times it
reboots into provisioning, and tapped five times while the radio is still
looking for a network it does the same from a different task. The fifth
meaning was always going to be one too many.

Scope is **four settings and one gesture to reach them**. The server knows
nothing about any of this; every value lives in NVS on the device.

## The decisions, and why

**A carousel, not a list.** One setting fills the screen, `A` walks to the
next, `B` changes the value. The alternative — a list with a cursor, entered
per item — was drawn and driven side by side with this one in a simulator
before either was written, and it lost on the thing that matters at 128×64:
the list needs two render modes and an `editing` flag, and the flag is where
the "stuck in the editor" class of bug lives. The carousel has one mode. Four
dots along the bottom say where you are, which is the only thing a list gives
that a carousel does not.

The cost is real and accepted: you cannot see the whole list, so returning to
the previous setting means walking the circle. At four pages that is three
presses. At eight it would be wrong, and eight pages is not this design.

**Button B alone opens settings — not both buttons together.** Holding both
was the first proposal and it does not survive contact with push-to-talk:
holding `A` *is* how you ask a question, so by the time a two-button gesture
completes, the device has already opened a socket, recorded a second of audio
and must throw it away. Every entry would cost a cancelled utterance and a
`cancel` frame to the server.

`B` has no other meaning. Held alone for two seconds it cannot collide with
anything, works with one hand, and needs no compensating cleanup.

**`A` exits, `B` acts.** Inside the menu each button has exactly one short
meaning and one long one, and they never swap:

| | |
|---|---|
| `B` held 2 s, from the face | open settings |
| `A` tap | next page, wrapping |
| `B` tap | next value, wrapping |
| `A` held 1 s | leave and save |
| `B` held 1 s | run this page's action — WiFi only |
| 20 s with no press | leave and save |

Entering on one button and leaving on the other is what keeps `B`'s hold free
for the WiFi page. Had exit been another `B` hold, that page would have had
two different holds on the same button.

The two holds are deliberately different lengths. Entry has to survive a
pocket and a curious hand, so it is long; exit is asked for by someone already
looking at the screen who wants out, so it is short. Symmetry here would make
one of them wrong.

**Latin labels.** `FONT5X7` in `setup_screen.c` covers `0x20-0x7e` and nothing
else. `VOLUME`, `SCREEN`, `EYES`, `WIFI` render today; `ГРОМКОСТЬ` needs
roughly 33 new glyphs, UTF-8-aware string handling in `ss_draw_text()`, and a
second look at the 21-character line, which is tighter in Russian. That is a
change worth making on its own, later, not folded into this one.

**Volume plays a tone on every change.** A volume control you cannot hear
while setting it is a guess. Nothing is playing while the menu is open, so
each `B` tap on the volume page emits a short blip at the new level.

**Five taps disappear from the product entirely.** The gesture has three
callers today — `net_task` to enter provisioning, `boot_gesture_task` to do
the same while the radio is still searching, and `app_main`'s provisioning
loop to *leave* setup without configuring. The first two are replaced by the
settings menu. The third is converted to the same `B` hold, so the whole tap
apparatus — `tap_count()`, `tap_counter_t`, `RESET_TAPS`,
`RESET_TAPS_VISIBLE` — goes with them.

Leaving the third one as taps was considered and rejected: a user who has
learned "hold B" would find that `B` does nothing on the one screen that most
needs a way out.

**`SHORT_PRESS_MS` stays.** It was introduced so the taps of the reset gesture
would not each cost a Groq STT call, and with the gesture gone that reason
goes with it. It still earns its place for the reason its own comment gives
second: a press under a fifth of a second was not a question, and treating it
as one spends a round trip on the link that is this project's blocking
problem.

## GPIO 20 is free, and this is why

GPIO 20 is `U0RXD`. It is free here because the console is not on UART0 —
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, set in `sdkconfig.defaults` with a
note that a board with a CP2102 or CH340 bridge must switch back. **On such a
board this pin is not available**: the bridge drives it, and a button pulling
it to ground would fight a driven output.

It is not a strapping pin, so a button held during reset cannot change the
boot mode. That leaves GPIO 21 as the only free pin after this one — 9 (BOOT),
18/19 (USB) and 2 (strapping) are spoken for, 0/1 are the panel, 4-7 and 10
are audio, 3 is the first button and 8 is the LED.

## The screen

128×64, one bit per pixel, drawn with the existing 5×7 font at a 6-pixel
advance. A value page:

```
y=0    <   VOLUME   >          title, centred
y=12   [####  ][####  ][    ]  gauge, x=14 w=100 h=18
y=33   -6 dB                   value, centred
y=45   hold A to exit          hint, centred
y=57   * . . .                 four page dots
```

The gauge is drawn as cells with a two-pixel gap: filled for steps reached,
outlined for steps not. Outlined rather than absent, so the scale's length is
visible at any setting.

**The hint line is permanent, and says only the non-obvious half.** Pressing
`B` moves the bar, which teaches itself. Leaving does not, and this is a
screen someone sees a few times a year.

The WiFi page has no gauge and uses the room for a second line:

```
y=0    <    WIFI    >
y=16   set up network
y=30   hold B to start
y=45   hold A to exit
y=57   . . . *
```

**Hold progress is drawn as a two-pixel bar along the bottom edge**, from 40%
of the threshold onward — a fraction rather than a fixed 700 ms, so the 2 s
entry and the 1 s exit both warn for the same share of the gesture.

On the face, this is not new code: `face_set_reset_progress()` already draws
exactly this and is about to lose its only caller. It is fed the `B` hold
instead, which also settles the awkward case for free — what the bar looks
like when the face has been asleep for three minutes is whatever it already
did for the reset gesture.

## The four pages

### VOLUME — six steps

| Step | Gain (Q15) | Shown |
|---|---|---|
| 0 | 0 | `muted` |
| 1 | 2068 | `-24 dB` |
| 2 | 4125 | `-18 dB` |
| 3 | 8231 | `-12 dB` |
| 4 | 16422 | `-6 dB` |
| 5 | 32767 | `0 dB` |

Logarithmic, because a linear scale wastes its first three steps below the
threshold where anything sounds different.

**Where it applies, exactly.** In `audio_out_task`, after
`adpcm_decode_block()` fills `pcm` and before the loop that shifts each sample
into the top half of its 32-bit slot. One multiply and a shift:
`(int16_t)(((int32_t)pcm[i] * gain) >> 15)`.

**`s_audio_level` is computed before the scaling, not after.** That line feeds
`face_feed_energy()`, and the eyes squash on the reply's own syllables. Scale
first and the face goes still at low volume, which would say the assistant is
mumbling when it is only quiet.

**A change ramps across one block.** Switching the coefficient between two
samples is a step, and a step is a click — the same reasoning that gives
`fill_tone()` in `playback_main.c` its 64 ms ramp. The gain interpolates from
old to new across the 512 samples of the next block.

Default is step 5, unity, so a device that never opens this menu sounds
exactly as it does today.

**The tone.** 150 ms at 660 Hz with 20 ms ramps at both ends, generated and
played by `audio_out_task` on a flag the menu raises — not written to I2S from
the menu's own task. The amplifier belongs to one task, the channel is
initialised once at boot and never re-initialised, and the 150 ms drain before
muting is what keeps the tail of a word. Reaching around that from a second
task is how the pop comes back.

At step 0 the tone is skipped, because silence is the value being
demonstrated.

### SCREEN — four steps

`ssd1306_set_contrast()` exists in `ssd1306.h:53` and is called from nowhere.
Four values: `0x10`, `0x40`, `0x80`, `0xCF`. Default `0xCF`, the value
`ssd1306.c:35` writes at init.

Applied the moment it changes, because this is the one setting whose effect is
the screen you are looking at.

### EYES — four resting expressions

`Calm`, `Happy`, `Curious`, `Excited`, mapping to `FACE_EMO_NEUTRAL`,
`FACE_EMO_HAPPY`, `FACE_EMO_CURIOUS` and `FACE_EMO_EXCITED`.

**This is not new geometry.** `face.c` already hard-codes a return to
`FACE_EMO_NEUTRAL` after 45 seconds of idle, on the way to sleepy at 90 and
asleep at 180. The setting replaces that constant and the matching default in
`face_init()`. Nine poses are already tuned in `EMO_POSE`; this chooses which
one is home.

The third candidate — `f->cur = EMO_POSE[FACE_EMO_NEUTRAL]` in `face_init()` —
is deliberately left alone. An earlier draft of this section said the setting
also decides "what the face wakes up as", and that was wrong twice over: the
boot animation computes its own pose and overwrites `f->cur` at handover, so
that line never reaches the panel; and waking from idle restores whatever
emotion the face was last told, never the resting one. Both were checked
against the code rather than reasoned about, once the claim was challenged in
review.

The API is one function, `face_set_resting(face_t *f, face_emotion_t e)`, and
`face.c` keeps its no-dependency rule — it is told the value, it does not read
NVS.

**The page previews itself.** Instead of a gauge, this page draws the eyes in
the selected pose, from `face_emotion_pose()`. The setting is a look; showing
a bar instead of the look would be a worse screen for no reason.

What the server sends during a conversation is unaffected. This changes only
what the face relaxes into once the conversation is over.

### WIFI — an action

Hold `B`: `config_request_provisioning()`, then `esp_restart()` into the
access point. The same path the five-tap gesture used, reached from a page
that says what it does instead of from a gesture nobody can discover.

No confirmation screen. The hold is the confirmation, it is announced on the
page, and the way back out of provisioning is one more `B` hold.

## What is stored

Three new NVS keys in `config_store`, all `uint8_t` holding the step index
rather than the value, so the tables can be retuned without a migration:

| Key | Range | Default |
|---|---|---|
| `vol` | 0-5 | 5 |
| `bright` | 0-3 | 3 |
| `eyes` | 0-3 | 0 |

**Written once, on the way out** — on the `A` hold and on the 20-second
timeout, not on each press. Walking the volume page in a circle is six presses
and would otherwise be six flash writes.

A missing key reads as its default, so a device flashed with this firmware
behaves exactly like the current one until someone changes something.

## The modules

The split already used twice in this codebase: logic and rendering as plain C
with no ESP-IDF, so `firmware/host` can build and test them, and a thin layer
in `voice_main.c` that owns the hardware.

**`settings_menu.c` / `.h`** — the pages, the value tables, and what each
gesture does. No IDF headers, no float, no allocation. Its state is a struct
the caller owns, the way `provision_logic.c` works.

```c
typedef struct {
    bool    open;
    uint8_t page;        // 0..SETTINGS_PAGES-1
    uint8_t step[3];     // vol, bright, eyes

    // Latched for the owner, cleared by the owner once acted on.
    bool    wifi_requested;
    bool    beep_requested;
    bool    save_requested;

    uint8_t hold_pct;    // 0..100, for the progress bar
} settings_t;

// Fed the two debounced buttons every tick. Returns true if anything changed.
bool settings_tick(settings_t *s, bool a_down, bool b_down, uint32_t now_ms);
```

**`settings_screen.c` / `.h`** — renders a `settings_t` into a framebuffer it
is handed, like `setup_screen.c`. Never touches I2C, never allocates. The eyes
preview calls `face_render_pose()`, which `face.h:188` already exposes for
exactly this kind of use.

**`config_store`** — three getters and one `config_save_settings()` that
writes all three keys in one commit.

**`voice_main.c`** — four small changes:

- `button_task` debounces both pins into `s_button_down` and
  `s_button_b_down`, with the same 25 ms and the same 5 ms poll. One task, two
  pins, because the reason it exists — timing that a stalled send cannot
  affect — is the same for both.
- `face_task` grows a third branch ahead of the provisioning one: when
  `s_settings.open`, render the menu instead of the face. The existing
  `s_setup_fb` is reused; it is 1 KB of BSS that is idle whenever the menu is
  not.
- `audio_out_task` reads the gain and the beep flag.
- The offline hint at `voice_main.c:1002` changes from `"5 presses = setup"`
  to `"hold B: settings"` — 16 characters, inside the 21 a line holds. Not
  "for setup": the hold opens the menu, and WiFi is a page inside it. That
  string is the only place any of this is ever taught.

**Where the menu is driven from.** `settings_tick()` is called from
`face_task`, which already runs at a fixed 40 ms tick and already owns the
panel. A 40 ms sampling interval against a 25 ms debounce and a 1 s hold is
comfortable, and it means no new task and no new stack.

## The boot order does not change, and that is the point

`face_task` is created at `app_main` line 34 and `button_task` at line 52.
`wifi_connect()`, which waits for a network with no timeout, is line 112.
The menu therefore works in the window where the device is stuck looking for a
network — which is the entire reason `boot_gesture_task` exists.

So `boot_gesture_task` is deleted, along with the `xTaskCreate` /
`vTaskDelete` pair around `wifi_connect()`.

`app_main`'s provisioning loop keeps its shape and swaps its trigger: instead
of counting taps it times a `B` hold, driving `SS_STATUS_LEAVING` on the setup
screen from hold progress rather than tap count. `provision_set_status()` and
the status enum are unchanged.

## Error handling and the awkward moments

**Settings opened while a reply is playing.** The hold is on `B`, which has no
playback meaning, so this is reachable. The reply keeps playing and the menu
opens over it; a volume change takes effect on the next block, which is the
one case where the setting is audible on real speech instead of a tone. No
beep is emitted while `s_playing` — the reply is the demonstration.

**Settings opened while listening.** Not reachable in practice, because
listening requires `A` held and this gesture is `B` alone, but if `A` is
released mid-hold the menu opens and the utterance ends normally through the
existing path. Nothing special is needed.

**The face is asleep when `B` is held.** `face_set_button()` is fed the first
button only, so today nothing about `B` reaches `face.c`. It is fed the second
one too, on the same call path, so a hold wakes the eyes and shows its
progress rather than completing behind a dark panel.

**A stuck button.** Same honest answer as the reset gesture: a stuck `B` and a
deliberate `B` hold are the same signal. The cost is bounded — the menu opens,
then the 20-second timeout closes it and saves values nobody changed.

**The timeout fires mid-hold.** The hold timer and the idle timer both reset
on any edge, so a hold in progress cannot be cut in half by the timeout.

**An NVS write that fails** is logged as a warning and nothing else. The values
are live in RAM and already applied; losing them at the next reboot is worth
less than a reboot loop.

**The panel is dead.** `ssd1306_flush()` already reports it and `face_task`
already logs the transition. A blind menu is not worth special-casing: the
device still works, and the person cannot see what they are changing anyway.

## What is testable without hardware

Both new modules build on the host, in the pattern `firmware/host` already
uses for `setup_screen` and `provision_logic`:

- every gesture through `settings_tick()`: taps wrapping at both ends, holds
  crossing and not crossing their threshold, the timeout, the flags for WiFi
  and the beep raised exactly once each;
- values persisting across an open/close cycle, and defaults when NVS is
  empty;
- the render, checked the way `setup_screen_test.c` checks its own: text
  inside the panel, nothing drawn past an edge, the gauge's cell count
  matching the step count, the page dots matching the page.

The gain table is a pure function and gets a test that walks it: monotonic,
step 0 exactly zero, step 5 exactly `0x7fff`.

**What only hardware can answer:** whether the tone is loud enough to judge by
at step 1, whether `0x10` is legible in a lit room, and whether a two-second
hold feels deliberate or slow. All three are one bench session.

## Deliberately out of scope

- **Cyrillic on the panel.** Its own change, with its own font work.
- **A "be quiet" meaning for a short `B` press outside the menu.** Interrupting
  already works on `A` and works well. Two ways to do one thing is a branch in
  `net_task` earning nothing.
- **A fifth page.** Four is what a carousel without a list can carry.
- **Reading these settings from the web app.** They are device-local. The
  playground shows what the server knows, and the server does not know these.
- **Per-conversation volume, or the server setting it.** The knob is on the
  device because the room the device is in is what decides it.
