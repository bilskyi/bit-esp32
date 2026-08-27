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

> **This is the second draft.** The first was reviewed against the ESP-IDF
> source and three of its decisions did not survive. They are recorded under
> "What the first draft got wrong" at the end, because two of them are the kind
> of mistake that is easy to make twice.

## The decisions, and why

**Provisioning is entered deliberately, never as a reaction to failure.** A
device that knows a network waits for it forever, exactly as it does today. It
does not fall back to an access point because the router is rebooting. The
alternative — falling back after a timeout — turns a two-minute router reboot
into a device that has stopped being a voice companion and become a hotspot,
and this project's radio is unreliable enough already.

**The trigger is a long press with the microphone quiet, not a power-on hold.**
Power-on hold is the standard pattern and was the first choice, until the
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
rather than detection, which is what the next two properties are for.

**The access point is not a one-way door.** Nobody uses it within the timeout,
it goes back to the saved network. This is what makes a stuck button cost
minutes instead of a trip for a cable.

**Credentials are never erased on entry.** They are overwritten only when new
ones arrive *and* connecting with them succeeds. Nothing about entering
provisioning can lose a working network.

**The access point is open, and the dangerous field is what carries the lock.**
WiFi credentials on a page that anyone nearby can reach is a small risk: the
window is minutes, it is entered on purpose, and a stranger setting your WiFi
would need your WiFi password anyway. The server URI is a different matter — it
points the microphone somewhere, and a passerby who changed it would be
listening to the room.

So the access point takes no password, and the **server URI field is locked
until a four-digit code from the panel is entered**. The common case, joining a
home network, has no friction at all. The rare and dangerous case needs
physical sight of the device.

The code is **random per provisioning session**, not derived from the MAC: the
MAC is broadcast in every frame, so anything derived from it is public. It is
regenerated every time provisioning is entered.

Four digits is ten thousand possibilities, which an HTTP client scripts through
in well under a minute — so the digits are not what protects the field. **Five
wrong codes lock it for the rest of the session**, and the only way to get more
attempts is physical: hold the button again, which requires standing next to
the device. That is the actual control; the code just makes it inconvenient to
guess in one try.

The access point is named `Voice-XXXX`, where `XXXX` is the last two bytes of
the station MAC as four hex digits, so two devices in one room are
distinguishable in the phone's network list. It is read, never typed, so hex
costs nothing here — which is exactly the argument that failed for a password.

This also removes a dead end the first draft had. With a WPA2 access point, a
device with no panel and no laptop could not be provisioned at all — the
password was undiscoverable. An open access point means the common case works
with no screen. Only changing the server URI needs one, and that is the correct
thing to lose.

**The access point coexists with the station only during provisioning.** Never
during a conversation. The radio is the project's blocking problem — 67% packet
loss at -49 dBm — and an access point sharing it while audio is streaming is a
way to make that worse for no benefit. But provisioning *requires* both at once,
for a reason the first draft missed; see the mode section below.

**The panel is the source of truth about success, not the page.** This follows
from a hardware fact rather than a preference, and it is the reason the whole
flow is shaped the way it is. See "Reporting the result".

**Running an access point puts the radio in the state that has already broken
this board, and that has to be said out loud.** Modem-sleep is a station
feature — *"When station connects to AP, Modem-sleep will start"*
(`wifi.rst:1773`) — and sleep in the disconnected state is *"supported ... if
running at station mode"* (`wifi.rst:1795`). An access point has to beacon and
stay receptive, so while provisioning is up the radio does not sleep at all.

`RESUME.md` records what happened the two times this radio was kept awake with
`WIFI_PS_NONE`: *"one 1 KB send took 32 seconds, 986 audio blocks were dropped,
and the device then failed to associate at all... most likely supply, since the
amplifier shares USB power."*

Two things make this survivable rather than a repeat. During provisioning the
amplifier is silent, so the load that is suspected of causing the sag is
absent; and nothing is streaming, so there is no throughput to starve — the
traffic is a few HTTP requests. **But this is a named risk with a bench check,
not an unknown:** the first thing to establish on the board is whether the
access point comes up and stays up on the intended power supply. If it does not,
that is a supply problem and no amount of code will fix it — the same
conclusion the radio investigation already reached.

**There is no font, and it has to be written.** `face.c` draws eyes;
`ssd1306.h` has no text; a search for `font|draw_text|glyph` across the firmware
returns nothing. It goes in its own file rather than into `face.c`, whose stated
discipline is eyes and nothing else — and, more usefully, because a separate
file inherits the property that made the eyes work at all: it compiles on the
laptop, so the setup screen can be looked at before it reaches glass.

## The modules

| File | Depends on | Responsibility |
|---|---|---|
| `main/provision.c`, `.h` | ESP-IDF | mode transitions, SoftAP, `esp_http_server`, the DNS stub, scanning, credential trial |
| `main/provision_form.c`, `.h` | **nothing** | form decoding and validation, as pure functions |
| `main/setup_screen.c`, `.h` | **nothing** | the 5×7 font and the setup screen, rendered into a framebuffer it is handed |
| `main/config_store.c`, `.h` | `nvs_flash` | typed get/set for `ssid`, `pass`, `server_uri`, with the `secrets.h` fallback |
| `main/face.c` | **nothing** | one addition: the reset countdown |
| `main/voice_main.c` | all of them | boot order, the long-press detector, who draws on the panel |
| `host/setup_screen_test.c` | `setup_screen.c` | layout invariants, no board |
| `host/provision_form_test.c` | `provision_form.c` | decoding and validation, no board, no network |

`setup_screen.c` and `provision_form.c` follow `face.c`'s rules — no ESP-IDF
header, no float, no allocation — for the same reason: `host/` has to build
them. That is what makes the two most bug-prone parts of this feature, the
screen layout and the form parser, testable on a laptop.

**One writer owns the framebuffer.** `face_task` stays the only thing that
draws: in provisioning it calls `setup_screen_render()` instead of the eyes,
and outside it renders the face as it does today. `setup_screen.c` never
touches the panel and holds no state about it. That is why no lock is needed,
and it is the answer to a question the first draft left open.

It learns which to draw the way it learns everything else — a `volatile` set by
the task that knows, exactly as `s_boot_stage`, `s_face_emotion` and
`s_face_startle` already work. One more of those, holding the provisioning
state and the strings to show. No new mechanism.

**`ws_start()` reads the URI from `config_store`, not from the macro.** It uses
`SERVER_URI` today, so without this one change the whole server-URI half of
this feature configures a value nothing reads.

**`httpd` is configured, not left at defaults.** The defaults are a 4096-byte
task stack and `max_open_sockets` 7, of which 3 are reserved internally
(`esp_http_server.h:55,60`). One phone needs far less: 2 sockets. That part of
the cost is a decision rather than a measurement, and the measurement is only
for what remains.

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
The bench keeps working exactly as it does today, and a device flashed with a
known-good `secrets.h` never has to be provisioned at all.

**"Configured" means a non-empty `ssid`, from either source.** This matters
more than it looks: `secrets.h.example` ships the placeholder
`"your-2.4GHz-network"`, which is non-empty and would count as configured, so a
device built from the template unchanged would try to join a network that does
not exist and wait for it forever. The template therefore changes to empty
strings, and the header gains a line saying that leaving `WIFI_SSID` empty is
how you get a device that provisions itself on first boot. Filling it in is the
bench shortcut, not the normal path.

## The state machine

```
power on
  └─ ssid configured, from NVS or secrets.h? ──no──→ PROVISIONING
        │yes
        ↓
     connect, wait indefinitely (today's behaviour, unchanged)
        │
        └─ button held + microphone quiet for HOLD_MS ──→ PROVISIONING
                                                            │
                            no HTTP request for AP_IDLE_MS  │
                                                            ↓
                                                  back to the saved network
```

Which mode to enter is a pure function of three booleans — ssid configured,
hold satisfied, idle timer expired — and is written as one so the host can test
it.

### The mode is `WIFI_MODE_APSTA`, and this is not a choice

The first draft said `WIFI_MODE_AP`. That cannot work, because the page shows a
list of networks and:

> `esp_wifi_scan_start()` API is supported only in station or station/AP mode.
> — `esp-idf/docs/en/api-guides/wifi.rst:504`

Scanning needs the station interface up. Trialling the credentials before
saving them needs it too. So provisioning runs `WIFI_MODE_APSTA` throughout,
and the station side is torn down and rebuilt as a plain `WIFI_MODE_STA` before
the websocket opens. The prohibition on coexistence is about the conversation,
not about provisioning.

Entering from a running device: `esp_wifi_stop()`, reconfigure, `esp_wifi_start()`.
Leaving does the reverse. A reboot on transition would be simpler and is
rejected because it loses the panel's continuity, and the panel is how the user
knows what happened.

### The existing event handler would sabotage the trial

`voice_main.c:412` reconnects unconditionally:

```c
} else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
    ESP_LOGW(TAG, "wifi dropped, reconnecting");
    esp_wifi_connect();
}
```

That is exactly right for normal operation — a dropped link should come back by
itself — and exactly wrong for a credential trial. A wrong password produces a
disconnect, the handler immediately retries it, and the trial never concludes:
it spins until `TRIAL_MS`, hammering the router with a password already known
to be bad, and then reports a timeout instead of "wrong password".

So the handler gains one piece of state: while a trial is in progress, a
disconnect **ends the trial** rather than retrying. The disconnect reason code
distinguishes the two answers the page needs —
`WIFI_REASON_NO_AP_FOUND` means the network is not there, an authentication
failure means the password is wrong — and reporting the difference is most of
the value of trialling at all.

Outside a trial the handler behaves exactly as it does today. This is the only
change to it, and the reconnect-forever behaviour that keeps the device alive
through a router reboot is preserved.

### Scanning during a trial

`esp_wifi_scan_start()` returns `ESP_ERR_WIFI_STATE` while the station is
connecting (`esp_wifi.h:514`). The page's rescan button is therefore refused
while a trial is running, and says so rather than failing silently.

### `wifi_start()` stops waiting forever, without waiting less

Today it blocks on `xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, ...,
portMAX_DELAY)`. If that stays, a device stuck connecting can never act on the
long press, because the task holding the boot sequence never returns. It waits
on two bits instead — connected, or provisioning-requested — still with no
timeout. The device waits for its network exactly as long as it does today; it
just becomes interruptible.

### Constants

| | | |
|---|---|---|
| `HOLD_MS` | 5000 | Long enough that no ordinary question reaches it even in silence, short enough to hold comfortably. A starting value; the bench may move it once the countdown animation exists, since the two have to read as one gesture. |
| `AP_IDLE_MS` | 300000 | Five minutes **since the last HTTP request**, not since the access point came up. A phone that joins and sits there does not hold it open; a person part-way through typing a password does. The first draft reset on neither and would have dropped an active user at 4:50. |
| `TRIAL_MS` | 20000 | How long to wait for the trial connection before calling it failed. Two DHCP attempts fit inside this on the networks measured so far. |
| `GRACE_MS` | 3000 | How long the access point stays up after a successful trial, so a page that survived can collect the result. |

## Reporting the result, and why the panel wins

This is the part the first draft got wrong, and it is worth stating the
mechanism rather than the conclusion.

When the station connects to the home network, the SoftAP is **forced onto the
home network's channel**:

> the home channel of AP and station must be the same, and if they are
> different, the station's home channel is always in priority... the AP needs to
> switch its channel from 6 to 9... Station that supports channel switching will
> transit without disconnecting
> — `esp-idf/docs/en/api-guides/wifi.rst:1660`

So at the exact moment the credentials prove correct, the phone may be dropped.
Phones that honour the Channel Switch Announcement survive; others do not. A
design whose only success signal is a page on that access point tells the user
"it worked" and "it broke" with the same silence.

Therefore:

- **The panel is authoritative.** It shows trying / connected / wrong password /
  network not found / timed out, and it is still there whatever the phone did.
  Those last three are separate on purpose: they send the user to three
  different next actions — retype the password, check the network is on, or try
  again nearer the router.
- **The page is best-effort.** It polls a status endpoint after submitting, and
  shows the result if it can still reach the device. It never has to.
- **The access point stays up for `GRACE_MS` after success**, so the poll has a
  chance to land before the interface goes away. Tearing it down the instant
  the trial succeeds would guarantee the page never gets its answer, even for a
  phone that would have survived.
- **The page says so.** One line: if this screen stops responding, look at the
  device.

With no panel attached, the USB log carries the same states. That is a
degradation, not a dead end — the common case still completes, because the user
can simply retry if nothing happens.

## The page

One screen:

- the scanned networks as a list, strongest first, with a line saying that only
  2.4 GHz networks appear because the device has no 5 GHz radio — otherwise a
  user whose phone shows a 5 GHz network hunts for it and finds nothing
- a password field
- the server URI, shown but **locked**, with the current value visible and a
  four-digit code field to unlock it
- a save button

A list rather than a text field for the SSID, because typing an SSID on a phone
is the single largest source of provisioning failures.

**Hidden networks are not supported.** The first draft contradicted itself here
— arguing against a text field in one section and referring to a "fallback text
field" in another. The decision is: no. A hidden network is served by the
`secrets.h` fallback, which is a build anyway.

On save the device trials the network, reports as described above, and writes
to NVS **only after the trial succeeds**.

The server URI is validated for shape before being accepted: scheme `ws://` or
`wss://`, a non-empty host, total length under 128. Whether the server is
actually reachable cannot be established from provisioning — the device is not
on the internet yet — so a syntactically valid but wrong URI **will** be saved.
The recovery is another long press, and the page says so next to the field.

## The countdown, and the setup screen

While the button is down and `s_audio_level` is below a threshold, a counter
advances; any speech resets it to zero. The eyes do something unmistakably
unlike listening, and releasing before the end cancels with nothing lost. The
exact animation is chosen in `host/face_preview.c`, the way every other
expression in this project was.

### Which holds arm it, because the button already means three things

A held button today means something different in each state, and the first
draft wrote "button held and the microphone quiet" as though it meant one
thing:

| State | What a hold does today |
|---|---|
| `ST_IDLE`, socket connected | starts `ST_LISTENING` and streams the utterance |
| `ST_SPEAKING` / `ST_THINKING` | interrupts the reply |
| socket down | logged and otherwise ignored (`voice_main.c:994`) |

The countdown is armed in **`ST_LISTENING` and in the socket-down case**, and
nowhere else. Arming it during a reply would make an interruption compete with
a reset, and interrupting is the more common intent by a wide margin.

`ST_LISTENING` is the case that needs care, because a silent five-second hold
there is *also* an utterance being streamed to the server. On entering
provisioning that utterance is abandoned deliberately: send `{"type":"cancel"}`
if the socket is still up, reset the microphone buffer, and go to `ST_IDLE`
before the radio is reconfigured. The server already handles `cancel` — it is
what the interrupt path uses — so this needs no server change.

The socket-down case needs no cleanup: nothing was streaming, and it is the
likeliest situation for a reset in the first place.

Once provisioning is entered the eyes are gone and the panel shows the setup
screen: the access point's name, the address `192.168.4.1`, the unlock code,
and the current status line. There is no face during provisioning, which is
itself the signal that the device is not in its normal life.

**The threshold and `HOLD_MS` are bench numbers and this spec does not invent
them.** `face.c` normalises loudness against a decaying peak, which is the
wrong tool here: silence has no recent peak to normalise against, so this needs
an absolute threshold on the raw `block_level()` output. The procedure:

1. Log `s_audio_level` once a second with the button held in a quiet room, for
   thirty seconds. Record the range.
2. Repeat while speaking normally at conversational distance.
3. The threshold goes between them, nearer the quiet figure.

If the two ranges overlap, the silence gate does not work in that room and this
spec's premise is wrong — say so rather than picking a number that splits the
difference.

**A known and accepted consequence: in a loud room the reset will not fire.**
The countdown never completes. That is the correct failure — it fails to do
something, rather than doing it by accident.

## The boot order changes

Today `app_main` creates `button_task` **after** `wifi_start()`, which blocks
forever. So while the device is stuck connecting, nothing samples the button —
the press is undetectable in precisely the situation that needs it.

`button_task` moves ahead of `wifi_start()`. It touches only GPIO and its own
debounce state, so it has no dependency on the radio. `face_task` is already
created before `wifi_start()` and stays where it is.

## Error handling

| Case | Behaviour |
|---|---|
| Wrong password | Panel and page both say so; NVS untouched; stays in provisioning |
| Network not found | Same; the list can be rescanned from the page |
| Trial takes too long | Fails at `TRIAL_MS` and is reported as a timeout, not as a wrong password — the disconnect reason is what separates those, and a timeout produced none |
| Rescan asked for during a trial | Refused with a reason; `esp_wifi_scan_start()` returns `ESP_ERR_WIFI_STATE` while connecting |
| Countdown completes mid-utterance | The utterance is abandoned: `cancel` to the server, microphone buffer reset, `ST_IDLE`, then the radio is reconfigured |
| Phone dropped by the channel switch | Panel carries the result; page is already documented as best-effort |
| Nobody uses the page | Times out `AP_IDLE_MS` after the last HTTP request, returns to the saved network |
| Wrong unlock code | The URI field stays locked; WiFi can still be set |
| Five wrong unlock codes | The field is locked for the rest of the session; only a fresh long press grants more attempts |
| Malformed server URI | Rejected on shape before the trial; the field says why |
| NVS write fails | Reported rather than swallowed; stays in provisioning |
| No panel attached | Everything works except reading the unlock code, so WiFi is settable and the server URI is not. States go to the USB log |

That last row is a constraint the project already holds itself to — `voice_main`
probes the bus and carries on if nothing answers — and provisioning must not
quietly become the first feature that requires the panel.

## What is testable without hardware

On the host, alongside `face_test.c`:

- **Setup screen layout** — text fits inside 128 px, nothing clipped, a
  maximum-length SSID does not overrun, all four lines coexist at 5×7.
- **Form parsing** — URL decoding, `+` as space, empty SSID rejected, a
  password containing `&` and `=`, an SSID at exactly 32 bytes, over-length
  input truncated rather than overflowing `wifi_config_t`.
- **URI validation** — schemes accepted and rejected, empty host, over-length.
- **Unlock code** — a wrong code leaves the URI unchanged even when the rest of
  the form is valid, and the fifth wrong code locks the field until the session
  ends. This is the security property, so it gets tests rather than a comment.
- **Mode selection** — the pure function over (ssid configured, hold satisfied,
  idle expired).

Only the bench can settle, in this order — the first one can invalidate the
rest:

1. **Does the access point come up and stay up on the intended supply?** The
   radio cannot sleep while it is beaconing, and this board has already failed
   twice in that condition. Everything else is moot if this fails.
2. The silence threshold, by the procedure above.
3. `httpd`'s heap cost beyond what is configured. Estimates on this project
   have been wrong by a factor of three, so this is measured, not predicted.
4. Whether the phone in the room survives the channel switch. It changes
   nothing about the design — the panel is authoritative either way — but it is
   worth knowing which phones get the nice path.

**The transitions themselves are not host-testable** and this spec does not
pretend otherwise. `esp_wifi_stop()`/`start()` sequencing, the DNS stub and the
grace period can only be judged on the board. What can be done is keeping the
decisions out of the transition code — which is what the pure mode selector is
for — so that what remains on the board is sequencing rather than logic.

## Deliberately out of scope

- **`DEVICE_TOKEN`.** Stays compiled in.
- **Hidden networks.** Use `secrets.h`.
- **Any change to the radio's behaviour in station mode.** `listen_interval`,
  `WIFI_PS_MIN_MODEM` and the reconnect logic are measured settings with a
  history of being made worse by well-meant changes. Provisioning must leave
  them exactly as they are.
- **Over-the-air firmware update.** A single `factory` partition cannot do OTA
  and adding one is a separate decision about the partition table.
- **Multiple remembered networks.** One is what the device needs.

## What the first draft got wrong

Recorded because two of these are easy to make twice.

1. **`WIFI_MODE_AP` cannot scan.** The draft specified AP-only mode and a page
   listing networks. `wifi.rst:504` rules that out. Both scanning and trialling
   credentials need the station interface, so provisioning is `APSTA`. The
   draft's own blanket "not APSTA" rule, written about conversations, would have
   forbidden the only workable answer.

2. **Success could not be reported reliably, and the draft did not say why.**
   `wifi.rst:1660`: connecting the station forces the access point onto the
   home network's channel, which can drop the phone at the exact moment of
   success. The draft promised a result "on the same page". The panel is now
   authoritative and the page is best-effort.

3. **WPA2 on the access point cost more than it bought.** Eight hex digits read
   off a 128×64 panel and typed on a phone, every time, to protect against a
   passerby in a five-minute window — and it made a device with no panel
   impossible to provision at all. Locking the one dangerous field behind a
   four-digit code puts the cost on the rare operation instead of the common
   one.

Also fixed: nobody owned the framebuffer; `AP_IDLE_MS` would have dropped an
active user; the hidden-network text field was referred to but never specified;
the server URI had no validation and no stated recovery path; the 5 GHz absence
was invisible; there was no face defined for provisioning itself.
