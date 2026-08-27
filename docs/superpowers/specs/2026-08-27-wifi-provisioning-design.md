# WiFi from a phone, not from a header — design

The network is compiled in. `firmware/main/secrets.h` holds `WIFI_SSID` and
`WIFI_PASSWORD`, so moving the device to another network means a laptop, the
ESP-IDF toolchain and a reflash. The device has no keyboard and is not getting
one.

So: on demand it raises its own access point, a phone joins it, a web page asks
which network to use, and the answer goes to NVS. No app to install.

Scope is **WiFi credentials and the server URI**. `DEVICE_TOKEN` stays compiled
in — it is the secret that keeps a stranger off the Groq quota, and it does not
belong in HTML served over the air.

## The decisions, and why

**Provisioning is entered deliberately, never as a reaction to failure.** A
device that knows a network waits for it forever, exactly as it does today. It
does not fall back to an access point because the router is rebooting. The
alternative — falling back after a timeout — turns a two-minute router reboot
into a device that has stopped being a voice companion and become a hotspot,
and this project's radio is unreliable enough already.

**The trigger is a long press with the microphone quiet, not a power-on hold.**
Power-on hold is the standard pattern and it was the first choice, until the
obvious objection: on battery power there is nothing to cycle. A long press
works identically on USB and on battery.

**Speech cancels the countdown; a stuck button cannot be detected at all.**
`block_level()` already computes loudness per block into `s_audio_level`, and
`face_feed_energy()` already drives the eyes with it — so the device already
knows whether anyone is talking. Gating the countdown on silence means a long
question can never trigger a reset, because a long question is not silence.

A stuck button is a different matter and honesty is the only available answer:
a stuck button and a deliberate silent hold are the same signal — button down,
microphone quiet — and no scheme can tell them apart. It is handled by cost
rather than detection, which is what the three properties below are for.

**The access point is not a one-way door.** Nobody connects within the timeout,
it goes back to the saved network. This is what makes a stuck button cost
minutes instead of a trip for a cable.

**Credentials are never erased on entry.** They are overwritten only when new
ones arrive *and* connecting with them succeeds. Nothing about entering
provisioning can lose a working network.

**The access point is down before the socket comes up.** Not concurrent, not
APSTA. The radio is the project's blocking problem — 67% packet loss at
-49 dBm — and an access point sharing it during a conversation is a way to make
that worse for no benefit.

**WPA2, not an open network, and that follows from the scope.** With only WiFi
credentials on the page an open access point would be defensible: the window is
minutes and it is entered on purpose. With the server URI on the page it is
not — a passerby inside that window could point the microphone at their own
server. The password is derived per device from the MAC and shown on the panel.

**There is no font, and this is where the previous decision costs something.**
`face.c` draws eyes; `ssd1306.h` has no text; a search for `font|draw_text|glyph`
across the firmware returns nothing. Showing a password means writing a 5×7
ASCII font. It goes in its own file rather than into `face.c`, whose stated
discipline is eyes and nothing else — and, more usefully, because a separate
file inherits the property that made the eyes work at all: it compiles on the
laptop, so the setup screen can be looked at before it reaches glass.

## The modules

| File | Depends on | Responsibility |
|---|---|---|
| `main/provision.c`, `.h` | ESP-IDF | SoftAP, `esp_http_server`, the DNS stub, network scan, NVS read/write |
| `main/setup_screen.c`, `.h` | **nothing** | the 5×7 font and the setup screen, into the same framebuffer |
| `main/config_store.c`, `.h` | `nvs_flash` | typed get/set for `ssid`, `pass`, `server_uri`, with the `secrets.h` fallback |
| `main/face.c` | **nothing** | one addition: the reset countdown as an eye state |
| `main/voice_main.c` | all of them | boot order, mode selection, the long-press detector |
| `host/setup_screen_test.c` | `setup_screen.c` | layout invariants, no board |
| `host/form_test.c` | `provision.c`'s parser | form decoding, no board and no network |

`setup_screen.c` follows `face.c`'s rules — no ESP-IDF header, no float, no
allocation — for the same reason: `host/` has to be able to build it.

The form parser is split out of the HTTP handler so it is a pure function over
a string. That is the whole reason it can be tested without a network.

## What is stored

NVS namespace `cfg`:

| Key | Type | Notes |
|---|---|---|
| `ssid` | str | up to 32 bytes, the limit in `wifi_config_t` |
| `pass` | str | up to 64 bytes, same source |
| `server_uri` | str | up to 128 bytes |

The `nvs` partition already exists at 0x9000, 24 KB, and `nvs_flash_init()` is
already the first line of `app_main`. Nothing about the partition table changes.

**`secrets.h` becomes the built-in fallback rather than the source.** An empty
NVS falls back to `WIFI_SSID`, `WIFI_PASSWORD` and `SERVER_URI` as compiled.
That is deliberate: the bench keeps working exactly as it does today, and a
device flashed with a known-good `secrets.h` never has to be provisioned at all.

**"Configured" means a non-empty `ssid`, from either source.** This matters
more than it looks: `secrets.h.example` ships the placeholder
`"your-2.4GHz-network"`, which is non-empty and would count as configured, so
a device built from the template unchanged would try to join a network that
does not exist and wait for it forever. The template therefore changes to empty
strings, and the header gains a line saying that leaving `WIFI_SSID` empty is
how you get a device that provisions itself on first boot. Filling it in is the
bench shortcut, not the normal path.

## The state machine

```
power on
  └─ credentials in NVS or secrets.h? ──no──→ PROVISIONING
        │yes
        ↓
     connect, wait indefinitely (today's behaviour, unchanged)
        │
        └─ button held + microphone quiet for HOLD_MS ──→ PROVISIONING
                                                            │
                                  nobody joins for AP_IDLE_MS│
                                                            ↓
                                                  back to the saved network
```

Entering PROVISIONING from a running device restarts the WiFi stack rather than
adding an interface: `esp_wifi_stop()`, reconfigure to `WIFI_MODE_AP`,
`esp_wifi_start()`. Leaving it does the reverse. A reboot on transition would
also work and is simpler; it is rejected because it loses the face's continuity,
and the face is how the user knows what is happening.

**`wifi_start()` stops waiting forever, without changing how long it waits.**
Today it blocks on `xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, ...,
portMAX_DELAY)`. If that stays, a device stuck connecting can never act on the
long press, because the task holding the boot sequence never returns. It waits
on two bits instead — connected, or provisioning-requested — still with no
timeout. The device waits for its network exactly as long as it does today; it
just becomes interruptible.

Two constants, and unlike the silence threshold these are choices rather than
measurements:

| | | |
|---|---|---|
| `HOLD_MS` | 5000 | Long enough that no ordinary question reaches it even in silence, short enough to hold comfortably. A starting value; the bench may move it once the countdown animation exists, since the two have to read as one gesture. |
| `AP_IDLE_MS` | 300000 | Five minutes: enough to find the phone, join, and fumble with the page; short enough that a stuck button costs an annoyance rather than a trip for a cable. Joining the AP does **not** extend it — a client that connects and wanders off must still time out. |

Which mode to enter is a pure function of three booleans — credentials present,
hold satisfied, AP timed out — and is written as one so it can be tested on the
host.

## The boot order changes

Today `app_main` creates `button_task` **after** `wifi_start()`, and
`wifi_start()` blocks on `xEventGroupWaitBits(..., portMAX_DELAY)`. So while
the device is stuck connecting, nothing samples the button — the press is
undetectable in precisely the situation that needs it.

`button_task` moves ahead of `wifi_start()`. It touches only GPIO and its own
debounce state, so it has no dependency on the radio. `face_task` is already
created before `wifi_start()` and stays where it is.

## The countdown

While the button is down and `s_audio_level` is below a threshold, a counter
advances; any speech resets it to zero. The eyes do something unmistakably
unlike listening, and releasing before the end cancels with nothing lost.

**The threshold and `HOLD_MS` are bench numbers and this spec does not invent
them.** `face.c` normalises loudness against a decaying peak, which is the
wrong tool here: silence has no recent peak to normalise against, so this needs
an absolute threshold on the raw `block_level()` output. The procedure:

1. Log `s_audio_level` once a second with the button held in a quiet room,
   for thirty seconds. Record the range.
2. Repeat while speaking normally at conversational distance.
3. The threshold goes between them, nearer the quiet figure.

If the two ranges overlap, the silence gate does not work in that room and the
spec's premise is wrong — say so rather than picking a number that splits the
difference.

**A known and accepted consequence: in a loud room the reset will not fire.**
The countdown never completes. That is the correct failure — it fails to do
something, rather than doing it by accident.

## What the phone sees

The access point is `Voice-XXXX`, where `XXXX` is the last two bytes of the
station MAC as four hex digits, so two devices in one room are distinguishable.
The password is the last **four** bytes as eight hex digits — eight because
that is WPA2-PSK's minimum passphrase length, so anything shorter would fail to
start the AP rather than merely being weak. Both are on the panel.

This is obfuscation, not cryptography: anyone who can see the panel can read
the password, and the MAC is broadcast in every frame. It is sized to the
actual threat — a passerby during the few minutes the AP is up — and not to a
determined attacker, who is not in this device's model.

A DNS stub answers every query with `192.168.4.1`, so anything typed into the
address bar lands on the page. Whether the phone's own captive-portal detection
pops the page up by itself is **not** something this design relies on: Apple,
Android and Windows each probe different URLs and change behaviour between
releases, and chasing that is a rabbit hole with no end. The panel showing the
address is the guarantee; an automatic popup is a bonus.

The page is one screen: the scanned networks as a list, a password field, the
server URI pre-filled with what is currently in use, and a save button. A list
rather than a text field for the SSID, because typing an SSID on a phone is the
single largest source of provisioning failures.

On save the device attempts the network immediately and reports back on the
same page — success, or wrong password, or not found. **Credentials are written
to NVS only after a successful connection.** A wrong password leaves the device
in provisioning with the old configuration intact.

## Error handling

| Case | Behaviour |
|---|---|
| Wrong password | Reported on the page; NVS untouched; stays in provisioning |
| SSID not found in scan | The list refreshes; a hidden network needs the fallback text field |
| Nobody joins the AP | Times out after `AP_IDLE_MS`, returns to the saved network |
| Joined but never submits | Same timeout; a joined client does not extend it indefinitely |
| NVS write fails | Reported on the page rather than silently swallowed; stays in provisioning |
| No panel attached | Provisioning still works; the AP name and password are logged over USB. The device has run without a display for its whole life and must keep doing so |

That last row is a constraint the project already holds itself to — `voice_main`
probes the bus and carries on if nothing answers — and provisioning must not
quietly become the first feature that requires the panel.

## What is testable without hardware

On the host, alongside `face_test.c`:

- **Setup screen layout** — text fits inside 128 px, nothing clipped at the
  edges, the SSID and password are legible at 5×7, and a maximum-length SSID
  does not overrun.
- **Form parsing** — URL decoding, `+` as space, empty SSID rejected, a
  password containing `&` and `=`, an SSID at exactly 32 bytes, over-length
  input truncated rather than overflowing `wifi_config_t`.
- **Mode selection** — the pure function over (credentials present, hold
  satisfied, AP timed out).

Only the bench can settle: the silence threshold, `httpd`'s real heap cost
(estimates on this project have been wrong by a factor of three), whether the
captive-portal popup appears on the phone in the room, and whether raising an
AP disturbs the radio the way everything else on this device does.

## Deliberately out of scope

- **`DEVICE_TOKEN`.** Stays compiled in.
- **Any change to the radio's behaviour in station mode.** `listen_interval`,
  `WIFI_PS_MIN_MODEM` and the reconnect logic are measured settings with a
  history of being made worse by well-meant changes. Provisioning must leave
  them exactly as they are.
- **Over-the-air firmware update.** A single `factory` partition cannot do OTA
  and adding one is a separate decision about the partition table.
- **Multiple remembered networks.** One is what the device needs.
