# Firmware over the air, because the cable is going away — design

The enclosure is being sealed. After that there is no USB port, and
`idf.py flash` stops being an answer to anything. Everything below exists to
replace that cable before it disappears.

Two paths, because they fail for different reasons and a sealed box needs both:

- **Through the server.** The device already holds an authenticated `wss://`
  to the server it talks to. Firmware goes down that same socket. Remote, and
  the only path that works when the device is not in the room.
- **Through the access point.** The provisioning AP grows a second job: a form
  that takes a `.bin`. No server, no internet, no account. The only path that
  works when the server is dead.

Neither path has the device fetching from GitHub. That is deliberate and it is
the whole shape of this design — see "Where the binary comes from" below.

> **`partitions.csv` cannot be changed over the air.** It is the reason this
> has a deadline. One more flash over the cable, bootloader included, and
> then the box can close. Doing it in the other order costs an enclosure.

## Where the binary comes from

This project is going open source, and the point of that is that nobody has to
depend on the author's server. So:

**GitHub holds the releases. The browser downloads them. The device never
talks to GitHub.**

The web app reads `api.github.com` (which sends `Access-Control-Allow-Origin:
*`) to show what the latest release is, and offers its download link — an
ordinary download, which CORS does not touch. The person then drops that file
into the page, and the server streams it to the device. Someone who built their
own firmware drops their own build in with the identical gesture; there is no
second code path for "my own version".

What this buys, and it is the whole reason for the arrangement:

- **The server stores nothing.** No volume, no bucket, no object storage. The
  file passes through. Nobody's Railway instance becomes anybody's CDN.
- **Railway or `localhost`, no difference.** Someone running the server on
  their laptop gets the same path with the same code.
- **One trust anchor on the device.** The device talks to exactly the host it
  already talks to, with the token it already has, over TLS it already does.
  Fetching from GitHub would mean redirects through
  `objects.githubusercontent.com` and a second TLS handshake — paid for out of
  the ~30 KB of free heap that `wss://` left behind (`voice_main.c:93`), which
  is the scarcest resource on the board.

The one cost is a manual step: download, then drag in. A server-side proxy
could remove it later. It is not worth the redirect handling today.

## The partition table

```
# Name,     Type, SubType,  Offset,   Size
nvs,        data, nvs,      0x9000,   0x6000
otadata,    data, ota,      0xf000,   0x2000
phy_init,   data, phy,      0x11000,  0x1000
ota_0,      app,  ota_0,    0x20000,  0x1F0000
ota_1,      app,  ota_1,    0x210000, 0x1F0000
```

Two slots of 1.94 MB, ending exactly at the end of the 4 MB flash. The app is
1,099,920 bytes today — 54% of a slot, 931 KB of headroom in each.

**`nvs` does not move, by a single byte.** Same offset, same size, so the WiFi
credentials and the server URI survive the reflash and the device does not have
to be provisioned again to be updated.

The 56 KB gap between `phy_init` and `ota_0` is not waste worth arguing about.
App partitions must begin on a 64 KB boundary — `CONFIG_MMU_PAGE_SIZE=0x10000`,
confirmed in the current `sdkconfig` — and `nvs` cannot be moved to close it.
Record it as reserved for growing `nvs`, and stop: there is 931 KB of slack in
each slot.

`sdkconfig.defaults` gains two lines, and loses one:

```
CONFIG_PARTITION_TABLE_CUSTOM=y            # already there, unchanged
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y    # new
```

The bootloader binary changes as a result, which is one more reason the cable
flash is not optional.

## Version identity

Nothing to configure. ESP-IDF 5.3.2 resolves `PROJECT_VER` in this order
(`tools/cmake/project.cmake:661`): `version.txt`, then a `VERSION` argument to
`project()`, then `git describe`, then the literal `1`. `git describe` is
already what happens — the `esp_app_desc` in the current build reads
`version='372f5d5-dirty'`, `idf_ver='v5.3.2'`.

What is missing is tags: the repository has none, so `--always` falls back to a
bare commit hash. `git tag v0.1.0` is the whole fix, and the GitHub release is
built from the same tag, so the string on the panel and the name of the release
are the same string.

A dirty tree gives `v0.3.0-2-g372f5d5-dirty`, which is a feature: it says out
loud that the board is not running a release.

**Versions are never compared.** `git describe` output is not a total order, and
any comparison would lie about dirty builds. The UI shows the two strings side
by side and leaves the button always enabled. This project does not render
numbers nobody sent it, and it should not start by inventing an ordering.

## Rollback: what "good" means

With `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, the bootloader marks a freshly
booted OTA image `ESP_OTA_IMG_PENDING_VERIFY`
(`bootloader_support/src/bootloader_utility.c:392`). If it reboots without
calling `esp_ota_mark_app_valid_cancel_rollback()`, the bootloader reverts to
the other slot.

**An image is good when a round trip to the server has happened** — the socket
opened *and* a frame came back down it. That single fact proves the radio, TLS,
the token and the protocol at once. "WiFi connected" proves none of those and
does not count.

**Entering provisioning does not make an image good.** It is tempting, and it
is a hole: firmware that breaks NVS reads would land in provisioning, declare
itself good, and take the working firmware with it. It is also unnecessary — a
provisioning session that succeeds ends in a reboot and a connection to the
server, which is the rule above. So provisioning **suspends the rollback
deadline** while someone is working with it, and nothing more. If it expires on
`PROV_AP_IDLE_MS` without success, the deadline resumes and the image is rolled
back. That is the right answer: firmware that can neither reach the network nor
be configured is worse than what it replaced.

**One deadline, not two racing rules.** `link_task` reboots the board after 90
seconds offline (`voice_main.c:1060`), and while in `PENDING_VERIFY` that
reboot **is** a rollback. Leaving it in place would mean a router that takes
longer than 90 seconds to come back rolls back perfectly healthy firmware. So
while `PENDING_VERIFY` is in force, `link_task`'s reboot is suppressed and a
single rollback deadline of 10 minutes governs.

**The deadline needs its own timer, because this project's watchdog does not
reboot.** `CONFIG_ESP_TASK_WDT_PANIC is not set` in the current `sdkconfig`:
the task watchdog prints and carries on. The usual reasoning — "a hang trips
the watchdog, which panics, which reboots, which rolls back" — is not connected
here. Firmware that quietly does nothing would sit in `PENDING_VERIFY` forever.
So a one-shot `esp_timer` calls
`esp_ota_mark_app_invalid_rollback_and_reboot()` after 10 minutes.

**That timer is armed on the first line of `app_main`, before
`nvs_flash_init()`.** This is a constraint, not a preference: everything after
it is something that can hang, and a deadline armed after the hang is not a
deadline. It works there — `esp_timer` is initialised by the startup code
before `app_main` runs, and `esp_ota_get_state_partition()` reads `otadata`
through `esp_partition`, which does not need NVS.

Honest about the false positive: update the device while the router happens to
be down, and healthy firmware is rolled back for the router's sin. The price is
updating again. The alternative — not rolling back on network failure — leaves
a sealed box with nothing to save it.

## Path 1: through the server

### The protocol

Binary frames are already spoken for: they carry reply audio into the play
buffer (`voice_main.c:743`). During a transfer they route to the OTA writer
instead — the same shape as the existing `s_discard_audio` flag
(`voice_main.c:737`), not a new mechanism.

Server to device:

| Frame | What the device does |
|---|---|
| `{"type":"ota_begin","size":N,"version":"…"}` | checks `N` against the inactive partition, `esp_ota_begin(N)`, switches binary routing |
| binary frames | `esp_ota_write()` in arrival order |
| `{"type":"ota_end"}` | checks the byte count, `esp_ota_end`, `esp_ota_set_boot_partition`, sends `ota_ready`, reboots |
| `{"type":"ota_abort"}` | `esp_ota_abort`, routing back |

Device to server: `{"type":"ota_ready"}` once the image is committed and
the board is about to reboot, `{"type":"ota_failed","reason":"…"}` when it is
not, and `{"type":"hello","version":"…"}` on every connect — which is what finally makes
the empty "Здоровʼя пристрою" block in `Devices.tsx:196` tell the truth.

`size` is mandatory, and not for validation: `esp_ota_begin(OTA_SIZE_UNKNOWN)`
erases the whole 2 MB partition up front, which is seconds of blocking. With
the size known, erasure is lazy, a sector at a time.

**There is no `sha256` in the protocol, on purpose.** `esp_ota_end()` runs
`esp_image_verify(ESP_IMAGE_VERIFY, …)`, and `esp_ota_set_boot_partition()`
verifies the image a second time (`esp_ota_ops.c:443`). The image carries
`hash_appended == 1` — a SHA-256 of itself in its last 32 bytes — and both
calls check it. So corruption in flight is caught by the appended hash, a
truncated transfer by the byte count, and a foreign binary by the header check
below. A protocol-level hash would add an mbedtls context and a megabyte of
hashing on a 160 MHz core, racing the flash writes, to re-answer a question
already answered twice.

**No application-level flow control.** `esp_ota_write()` of 4 KB is 10–20 ms;
TCP backpressure is the right mechanism. This is an assumption to measure, not
a settled question — this firmware has a documented history of the WebSocket
client tearing down healthy connections when its own task cannot get the lock
in time (see the `CONFIG_ESP_WS_CLIENT_TX_LOCK_TIMEOUT_MS` note in
`sdkconfig.defaults`). If it does not hold, the answer is a window of 8 frames
acknowledged by the device, and nothing else in the design changes.

### Rejecting a foreign file before writing a byte

A valid image for this board, verified against the actual build:

| Offset | Value | Meaning |
|---|---|---|
| `0x00` | `0xE9` | ESP image magic |
| `0x0C` | `5` | `chip_id`, ESP32-C3 |
| `0x20` | `0xABCD5432` | `esp_app_desc` magic |
| `0x50` | `"voice_capture"` | `project_name` |

144 bytes decide whether a file is a firmware image for *this* project on *this*
chip. A ZIP, a build for another chip, or another project's binary is refused
before the partition is touched. It is a pure function of a fixed-size buffer.
The device's copy lives in `ota_logic.c`; the server and the browser each
express the same rule in their own language, over one shared set of test
vectors taken from a real build, so the three cannot drift. The browser's copy
exists only to fail fast before uploading a megabyte - the server's is the one
that actually guards the device.

### Progress, and the two things it can mean

The server counts the bytes it has pushed into the socket and streams that
count back in the body of its own response, which `fetch()` reads as it
arrives. No second channel to the browser, and no per-transfer queue between
the server's socket task and the HTTP request — an earlier draft had the device
report progress and needed exactly that queue for nothing.

But "the bytes were sent" is not "the firmware works", and the UI must not
conflate them. Two phases, shown separately:

1. **Передано** — the POST completed. Cheap, immediate, and proves very little.
2. **Запустилось** — the device rebooted, reconnected, and sent `hello`
   carrying the *new* version string. This is the only phase that proves
   anything, and it is simultaneously the proof that rollback did not fire.

### On the server

`server/firmware.py`, and a registry of live device sockets by surface. There
is no such registry today, and `Devices.tsx:52` says so outright; this is the
minimum that makes the feature possible, and it is also what makes the device
health panel honest.

`POST /firmware/push` — login-authenticated, multipart, streamed. It checks the
header, streams 4 KB frames into the esp32 socket, and returns a stream of
progress. It never writes the file to disk.

`main.py`'s WebSocket loop needs one change: `ota_*` and `hello` frames are
intercepted before `session.on_*` dispatch, which today would drop them into
`log.debug("ignoring control message")`.

### Parsing control frames

The existing text-frame handling matches substrings with `memmem` — `emotion`,
`done`, `speaking`, `thinking` (`voice_main.c:759`). **That code is not
touched.** It works, and rewriting working code to accommodate a new feature is
how working code stops working.

But `size` cannot be pulled out with a substring match, so the `ota_*` frames
get precise field extraction — `ol_frame_type()`, `ol_field_u32()` — checked
before the existing branches.

## Path 2: through the access point

No new gesture, no new access point, no second HTTP server. Provisioning mode
takes a second job: the same AP, the same `httpd`, one more handler,
`POST /firmware`, streaming the request body into the OTA writer 4 KB at a
time. This is honest rather than merely cheap — both states mean "the device is
out of service and you are configuring it".

Entering provisioning to update firmware is safe and reversible: nothing about
entering it erases a working network (`config_store.h`), so a device that comes
out of it goes straight back to the server it knew.

**The upload sits behind the same four-digit code that already guards the
server URI** (`provision.c:878` — fresh from `esp_random()` each session, shown
on the panel). Uploading firmware is more dangerous than retargeting the URI,
not less.

**The idle timeout has to be refreshed from inside the upload, and this is a
real bug avoided rather than a precaution.** `PROV_AP_IDLE_MS` is five minutes
since the last HTTP *request* (`provision.h:19`). A single long POST produces
no requests while it runs. A megabyte over a weak AP link can exceed five
minutes, and the access point would be torn down in the middle of writing to
flash. The handler stamps the idle marker on **every chunk it receives**, not
once on entry.

Two more numbers to check on the board rather than assume:
`cfg.recv_wait_timeout`, five seconds by default, which every `httpd_req_recv()`
must meet; and `max_uri_handlers`, which goes from five handlers to six against
a default of eight.

## Interaction with the two-button settings design

`docs/superpowers/specs/2026-09-03-two-button-settings-design.md` is in flight
at the same time, and it rewrites how provisioning is entered: the five-tap
gesture disappears entirely, along with `tap_count()`, `tap_counter_t`,
`RESET_TAPS` and `boot_gesture_task`, replaced by a two-second hold on a second
button.

**Nothing here depends on how provisioning is entered.** Path 2 lives *inside*
provisioning mode; whether the way in is five taps or a hold on button B, the
upload handler is reached the same way and this design does not care.

What that costs is a rule about footprint, and it is worth stating because it
is the only reason the two changes can land in either order: **all new logic
goes in new files.** `voice_main.c` gains routing and three small edits, and
`provision.c` gains one handler and one form field. Neither goes near the
button apparatus the other design is removing.

## Module boundaries

The split mirrors the existing `provision_logic.c` / `provision.c` pair,
because that is this repository's established shape for "pure decisions here,
hardware there".

| Unit | Owns | Depends on |
|---|---|---|
| `firmware/main/ota_logic.{c,h}` | the 144-byte header check, frame type and field extraction, the transfer state machine, size against partition | nothing — C stdlib only |
| `firmware/main/ota.{c,h}` | `esp_ota_*`, the rollback deadline, marking valid | ESP-IDF, `ota_logic` |
| `firmware/main/voice_main.c` | routing `ota_*` and binary-during-transfer, suppressing `link_task`'s reboot in `PENDING_VERIFY`, marking valid on the first server frame, arming the deadline, sending `hello` | everything |
| `firmware/main/provision.c` | one URI handler, one form field, refreshing the idle marker | `ota`, `ota_logic` |
| `server/firmware.py` | the live-socket registry, the relay, the header check | FastAPI |
| `web/src/firmwareApi.ts` | the GitHub release query, the upload call, progress parsing | fetch |
| `web/src/Devices.tsx` | the panel, both phases, the two version strings | `firmwareApi` |

`ota_logic.c` is where anything that can be decided without hardware goes,
because everything in `ota.c` can only be judged on the board.

## What is tested where

**On the laptop, with no board:**

- `firmware/host/ota_logic_test.c`, alongside `provision_logic_test` in
  `firmware/host/Makefile`. The header check parses bytes supplied by whoever
  uploaded the file, which is exactly the shape of bug the ASan build in that
  Makefile already caught once in `pl_unlock_check` — so it runs sanitized by
  default, like the rest.
- The whole server path, end to end. `scripts/fake_device.py` already speaks
  this protocol; it learns to accept `ota_begin`, collect the bytes, and verify
  them. Browser to server to device gets covered without hardware, which leaves
  only what genuinely needs the board.
- `web/src/firmwareApi.test.ts`, in the shape of the existing
  `settingsApi.test.tsx`.

**Only on the board, and this spec does not pretend otherwise:**

- Whether the WebSocket survives a megabyte with flash writes in the receive
  path, or whether the 8-frame window is needed.
- `esp_ota_write()` timing, and total transfer time over real WiFi.
- Free heap during a transfer, with the socket open. Estimates on this project
  have been wrong by a factor of three before.
- `httpd` receiving a megabyte: `recv_wait_timeout`, handler stack, and the
  idle-marker refresh actually holding the AP up.
- That rollback fires. Deliberately flash an image that connects to nothing,
  and watch the previous one come back.

## The order of operations, which is not negotiable

1. New partition table, `ota.c`, `ota_logic.c`, both paths, tests green.
2. Flash over the cable — bootloader, partition table, app.
3. Confirm the device still talks.
4. Confirm an update actually lands, **on the open board**, both paths.
5. Confirm rollback fires, with a deliberately broken image, **on the open
   board**.
6. Only now, seal the enclosure.

Steps 4 and 5 on a sealed box are not tests, they are the thing the tests were
supposed to prevent.

## Deliberately out of scope

- **Signed images and secure boot.** The trust boundary today is TLS, the
  user's own server, and a login. The `secure_version` field exists in the
  image header, so anti-rollback can be turned on later without touching the
  partition table.
- **Automatic updates and staged rollout.** Updates happen on an explicit yes.
- **A device registry beyond live sockets.** Heartbeat, uptime, history: a
  separate decision, and `Devices.tsx` currently declines to show them for
  exactly the right reason.
- **Delta updates and compression.** A megabyte over WiFi is tens of seconds.
- **OTA of the bootloader or the partition table.** Not possible, and the
  reason for the single cable flash.
- **A server-side GitHub proxy.** Removes one manual step, costs redirect
  handling. Later, if the step turns out to be annoying.

## What the first draft got wrong

Recorded because four of these were confidently stated before being checked,
which is the failure mode this section exists to catch.

1. **`CONFIG_APP_PROJECT_VER_FROM_GIT` does not exist.** Specified as the way
   to get versions from git. In ESP-IDF 5.3.2, `esp_app_format/Kconfig.projbuild`
   only knows `APP_PROJECT_VER_FROM_CONFIG`; `git describe` is already the
   default fallback and needs no configuration at all. The real gap was tags,
   which the draft never mentioned.

2. **The watchdog was assumed to reboot on a hang.** It does not here —
   `CONFIG_ESP_TASK_WDT_PANIC is not set`. The draft's rollback story had no
   coverage for firmware that hangs without crashing, which is the failure a
   rollback deadline exists for.

3. **`link_task`'s 90-second reboot was called a free rollback trigger.** It is
   one, and it is too aggressive to be the only one: a router slower than 90
   seconds would roll back healthy firmware. Two mechanisms racing was the
   actual design; one deadline, with `link_task` suppressed, is the fix.

4. **Provisioning was going to mark the image valid.** That hands a
   NVS-breaking build a way to declare itself good and delete the firmware that
   worked. It also was not needed: a provisioning session that succeeds reaches
   the server anyway. Suspending the deadline is what was meant.

5. **A protocol-level `sha256` re-answered a question answered twice.**
   `esp_ota_end` and `esp_ota_set_boot_partition` both verify the image's own
   appended hash. Removed.

6. **Device-reported progress needed a queue between two server tasks.** The
   server already knows how many bytes it has sent. Removed, and with it the
   queue.

7. **Version strings were going to be compared.** `git describe` output has no
   total order. Two strings side by side, and a button that is always enabled.

8. **The release row had no repository to read.** This design says the web app
   shows the latest GitHub release beside the running version. There is no
   GitHub remote: `git remote -v` is empty. Building a row that reports on a
   repository which does not exist is exactly the thing this project refuses
   to do everywhere else, so `GITHUB_REPO` in `web/src/firmwareApi.ts` is
   empty and the panel says "не налаштовано" until it is filled in. The
   function that reads a release is written and tested; only the constant is
   missing, and that is the honest state rather than a gap.

Also caught: the AP idle timeout would have torn the access point down in the
middle of a slow upload, because a single long POST is indistinguishable from
five minutes of nobody doing anything.

## Two things that changed during implementation

Recorded because the spec above still describes them the old way in places.

- **The panel is `web/src/Firmware.tsx`, not a block inside `Devices.tsx`.**
  That file was already carrying persona, the live screen and the joining
  instructions; this adds a file picker, two kinds of progress and a poll
  loop. `Devices.tsx` renders `<Firmware />` and keeps its own size.

- **`ota_note_error()` exists.** Refusing an update because the device is
  mid-conversation is a decision `voice_main.c` makes, but the reason has to
  reach `ota_last_error()` so the frame sent back carries one. Rather than
  give `voice_main.c` an error string of its own - the footprint rule above -
  the reason is recorded through the module that owns all the others.
