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

## The playground — REBUILT AND DEPLOYED 3 Sep

The UI was rebuilt from scratch in a design the client approved after six
competing directions. The short history, so nobody re-opens it: three
device-centric directions were rejected because they made the ESP32 the
subject rather than the assistant - my brief's own first line caused that.
Three ecosystem directions followed; the client chose one frame, then said the
style was still wrong and named Linear/Raycast as the reference. Rebuilt in
that language, then subtracted after "слишком нагружено": Головна went from 8
bordered regions to 3 and 90 text elements to 46, with the type scale
untouched. `docs/design-brief.md` carries the reframed brief and both pinned
decisions.

**Seven sections**: Головна, Розмова, Знання, Зʼєднання, Пристрої, Характер,
Журнал. The landing screen is a home that greets you - the client's own
decision over opening into a transcript or a command bar.

**Two endpoints were added** to make Журнал real: `GET /conversations/{id}`
and `GET /usage/{id}`, both login-gated. `conversation_rows` and `usage_rows`
had been recording in production since 2 Sep and were never exposed.

**What is deliberately absent.** Документи, Зʼєднання and a device's health
have no backend, so nothing about them is rendered - no file list, no fake
calendar, no invented heap or uptime. Each says so in her voice. The rule that
outranks fidelity to the design: a figure appears only if the server returns
it.

**The one piece of real product logic**: Головна's waiting card counts
auto-extracted facts older than the earliest conversation on record, and
disappears when none qualify. It was first written against the earliest
conversation *or usage* row, which could never fire - usage is logged for every
session including the strangers', so the baseline predated every fact. On the
real volume it now finds 9. Note it over-counts slightly by intent: the two
2 Sep facts predate that day's first *recorded* conversation, so they are
flagged too. The copy says only what it measured - `device_id` is "default"
for everyone, so the app cannot know whose a fact is.

Verified on production 3 Sep: the device's socket answered with **no trace
frame** (`state, emotion, state, reply, done`), the layout harness is clean
across twenty combinations, and Головна shows real figures - 19 questions,
8,407 tokens, 9 facts and 2 instructions.

Counts: **404 pytest, 35 vitest.** Image built and smoke-tested before deploy.
Rollback point: deployment `58b8567f`.

## The playground — earlier record, 2 Sep

Live at https://voice-server-production-e023.up.railway.app — sign in as
`bilskyi`. Deployed with `railway up` (there is no git remote, so uploads are
the deploy path) from the two-stage Dockerfile; `builder = "nixpacks"` and the
`startCommand` are both gone.

Verified against production, not inferred:
- **The device still works.** Its bearer socket answered a real Groq question
  and got `state, emotion, state, reply, done` with **no trace frame**. That
  was the rollback question and the answer was no.
- The browser socket asked `Як мене звати?` and got `Laaa Тебе звати Саша.` -
  correct, with the standing instruction applied - plus a trace: role
  `Web default`, emotion `curious`, 355 prompt tokens, 325 ms.
- `/` serves the app; `face-frames.json` arrives **gzipped at 38,063 bytes**.
- The account was created inside the container with
  `railway ssh --service voice-server "python scripts/create_account.py ..."`,
  against the volume, then login → `/me` → `/roles` → `/settings/app` all work.
- `SESSION_SECRET_KEY` set (64 hex chars). `DB_PATH=/data/voice.db` and
  `EMBEDDING_CACHE_DIR=/data/fastembed_cache` were already correct.
  `SESSION_COOKIE_SECURE` deliberately unset, so it defaults True on HTTPS.
- Rollback point if ever needed: deployment `76d4f57d`.

**What the inspector showed on its first real question, worth knowing:** the
six auto facts it retrieved scored 0.24 down to 0.10 and none of them was the
name. The answer came from the standing instructions, which are injected in
full every turn rather than ranked. Ranked retrieval contributed nothing to
that answer - which is exactly the kind of thing this panel exists to reveal.

### Three layout bugs, and the reason they shipped (2 Sep)

All three passed a green suite, a clean typecheck and a lint pass, and all
three were obvious the moment anyone looked:

- The composer put its input on one row with a mic button pinned at 11rem and
  a Send that refuses to shrink, so below roughly 600px the input collapsed to
  a single character wide - placeholder wrapping one letter per line - and Send
  was pushed off the edge. The 420px breakpoint never touched the composer and
  the width where it broke was far above 420. The input owns its own row now.
- The 900px grid assigned columns by DOM order, and the face comes first in the
  markup so it can sit above the transcript on a phone - so the eyes took the
  1fr column and the conversation was squeezed into 15rem. The eyes were the
  hero and the chat a sidebar. Both are placed explicitly now.
- One message left ~600px of nothing under it. The transcript anchors to the
  bottom now.

**The fix for the cause:** `npm --prefix web run shots` drives chromium at five
widths in both colour schemes, screenshots each, and *fails* on the shapes
these bugs had - a collapsed input, a Send past the edge, a face wider than
the conversation beside it. Verified against production: ten combinations, no
problems. It reproduces all three before the fix.

One thing that tool taught immediately: it first screenshotted before the
WebSocket connected, so every image showed "Connecting..." with the face
marked OFFLINE - indistinguishable from a broken deploy. It waits for the
socket now. The socket itself connects in 431 ms on production and a real
conversation works: "Скажи одне слово." came back "Привет" with 6 facts
retrieved and the inspector reading 335 ms.

### The volume's 10 facts, and an open question about them

    2026-08-27  auto   User prefers communicating in Russian
    2026-08-27  auto   User also understands and uses Ukrainian
    2026-08-28  auto   User communicates in Ukrainian and Russian
    2026-08-28  auto   User appreciates humor and jokes
    2026-08-28  auto   User speaks Ukrainian and Russian
    2026-08-28  auto   User is interested in learning programming
    2026-08-28  auto   User asks for facts about space, elephants, machines, and jungles
    2026-09-01  user   меня зовут саша
    2026-09-01  user   user's name is "Sasha"
    2026-09-01  user   always start speaking with "Laaa"

An earlier note in this file says the seven 27-28 Aug facts arrived from real
traffic that was "none of them mine". Read now, they do not obviously look
like a stranger's - they describe someone who speaks Ukrainian and Russian,
likes jokes and is learning programming. **Nothing has been deleted.** Whether
they are yours from your own bench testing on those two days, or somebody
else's, is a question only you can answer, and the memory editor in Settings
is where to act on it.

## The playground — how it was built (1 Sep)

**There is an app now.** Sign in, ask a question, watch the reply arrive a
sentence at a time, open an inspector under any answer to see which remembered
facts were retrieved and at what cosine score, edit roles and pin a mood, hold
a button and speak. The device's eyes are in the browser too, played from
frames generated by `firmware/main/face.c` — the same C that drives the panel,
because `face_preview.c`'s header records what happens to a JavaScript copy of
them: it drifts within a day. Regenerate with `make -C firmware/host face-data`.

Counts at handoff: **400 Python tests, 35 frontend tests.** The frontend suite
is deliberately narrow — it covers only logic a browser cannot falsify (the
socket's cancel accounting, the audio resampler, the role patch payload, the
face's bit unpacking, `traceStatus`). Everything visual rests on a per-task
browser checklist in the plan, **and nobody has run any of it yet.**

### Before you deploy — do these in order

1. **The image is built and smoke-tested — 2 Sep.** `docker build` succeeded
   first try, and the container was then driven end to end on a mounted
   volume. Proven in the real image, not by reading:
   - it binds `$PORT` (told 9137, listened on 9137) — which is what the
     deleted `startCommand` would have broken;
   - `/` serves the app, `/favicon.svg` serves as SVG, `/roles` is 401 without
     a cookie;
   - `face-frames.json` comes over the wire **gzipped at 38,063 bytes** rather
     than 1,234,649;
   - `railway ssh -- python scripts/create_account.py` works inside the
     container against the volume, then login → `/me` → `/roles` → `/settings/app`;
   - **the device's socket gets no trace frame** (`state, emotion, state,
     reply, reply, done`), a browser's gets exactly one, and a spoken question
     returns 29 binary PCM frames plus its trace.
   Rebuild before deploying if anything changed since: `docker build -t vc .`
2. `SESSION_SECRET_KEY` set in Railway variables. Empty means a random key per
   process and everyone signed out on every restart.
3. `DB_PATH=/data/voice.db`. It defaults to a relative path that lands inside
   the container and **boots perfectly healthy while showing none of the real
   facts**, wiping the account on every redeploy.
4. `EMBEDDING_CACHE_DIR` still on the volume, or every boot re-downloads
   0.22 GB.
5. Decide the recording default **before the first boot** — the table starts
   filling on turn one. Currently on, 90-day retention.
6. Deploy, then check **the device first**, before the app: press its button
   and get a spoken reply. It was working before this.
7. Create the login **inside the container**:
   `railway ssh -- python scripts/create_account.py --username you --password ...`
   Not `railway run`, which executes on your laptop with Railway's variables
   injected and writes to a local file while telling you it succeeded.

### The browser checks that matter most

The final review named six, in priority order. The first two are the only ones
that can catch a Critical class of bug that automated tests cannot reach:

1. Hold the mic on a **fresh browser profile** and answer the permission
   prompt. This is the one check for the tap-leaves-the-mic-open bug.
2. Ask a typed question, open the inspector, break the connection, reconnect,
   ask again. This is the one check for the cancel-counter bug — whose symptom
   is that the inspector silently never renders again.
3. Sign in, ask a question, confirm the inspector shows facts, scores and the
   assembled prompt.
4. Create a role, point the **device** at it, ask the device a question.
5. The app on a real phone over the LAN at 320px with
   `SESSION_COOKIE_SECURE=False` — this settles whether the composer sits below
   the fold (`100vh` vs `100dvh` on iOS Safari, an open Important).
6. Read the memory list against the production volume **before** deleting
   anything.

Can wait: dark mode, reduced motion, the face's grid alignment, keyboard-only
traversal of the role editor.

### Known-open, from the final review

- **`100vh` on iOS Safari** (`app.css`) may put the composer below the fold on
  first paint. `100dvh` is the fix; needs a phone to confirm it matters.
- **No `Cache-Control` on any asset**, so hashed bundles revalidate every load.
- **`DEVICE_TOKEN` empty makes `_token_ok` return True**, so the `/memory`
  routes need no credentials at all. On a public domain that is a
  world-writable memory. Set the token.
- **`.railwayignore` never learned about `web/`**, so `railway up` would upload
  ~115 MB of `node_modules`. `.dockerignore` has it; the two drifted apart.
- **Switching tabs discards an unsaved role draft** silently — and "edit the
  prompt, then go test it" is the playground's core loop.
- **Recording off does not stop the assistant remembering**: `finish()` runs
  `extract_facts` ungated. The copy explains retention but not this.
- **`/docs`, `/redoc`, `/openapi.json` are public.**
- The cancel-counting mechanism in `useTurn.ts` is on its fourth patch. Sounder
  now — one server-state-derived rule replaced several special cases — but
  still a hand-maintained invariant. The structural fix is a turn id echoed in
  the `done` frame, matched rather than counted. That is a protocol change
  touching the device, so it belongs to the next slice, when the bench is out.

### Not done, deliberately

Document RAG (uploads) is slice 2, the dashboard reading these conversations is
slice 3, MCP tool-calling is slice 4. `Store.conversation_rows()` exists and is
unused until slice 3.

**The twenty-second stalls are still the open bug.** Nine of sixteen remain
unexplained (section 0a). Nothing in the playground work touched the radio
link, and the uncommitted firmware changes in the working tree belong to that
investigation, not this one.

---

## The playground backend — the server half, for reference

The web playground was specced into four slices; **slice 1's backend is done
and on main, and none of it has been deployed.** There is no UI: the frontend
is a separate plan that has not been written. So the only way to touch any of
this today is curl plus a session cookie.

What landed, in one line each:

- The shared-identity branch that had been sitting unmerged in a locked
  worktree for three days: password accounts, a session cookie additive to
  `DEVICE_TOKEN`, and a `text` control frame so a typed question runs the same
  pipeline as a spoken one. Its own whole-branch review never ran last session
  (spend limit); it ran this time, as part of the final review.
- **Roles.** The personality is rows now, not a constant. Per-surface active
  role, so the device and the browser can differ. Two seeded roles are
  `built_in` and cannot be renamed or deleted.
- **`persona.BASE` is still byte-identical**, and now protected by a golden test
  rather than by being impossible to change. Verified on the real path — a role
  resolved from a live database — not just against the in-memory constant.
- **Mood can be pinned**, overriding the tag the model chose, and reaches the
  device's face on the next turn of an already-open socket.
- **Retrieval returns its cosine scores**, and a web-only `trace` frame carries
  them, the assembled prompt, token counts and per-stage timings.
- **Conversations are recorded.** Nothing stored a transcript before this.

### Before this deploys — in this order

1. Set `SESSION_SECRET_KEY` in Railway vars. Empty means a random key per
   process, so every restart invalidates logins.
2. Decide the recording default **before first boot**, because the table starts
   filling on turn one. It currently defaults to on with 90-day retention.
3. `uv run python scripts/create_account.py` against the volume, or you cannot
   log in and none of the new endpoints are reachable.
4. Then the cleanup the spec asks for: log in and delete the facts belonging to
   other people. `DELETE /memory/{device_id}` now drops that device's
   conversations too, which it did not when the spec was written.

`SESSION_COOKIE_SECURE` defaults to True. Right for Railway, wrong for testing
from a phone over plain http on the LAN — the browser drops the cookie and
every call after a 200 login returns 401.

### Things the final review found that are worth carrying forward

- **Facts are not covered by retention.** `extract_facts` derives facts *from*
  the transcripts at session end. Expiring or deleting a conversation does not
  remove the summary made from it. `DELETE /memory/{id}` removes both; the
  retention sweep removes only messages and the conversations they emptied.
- **SQLite now has three engines on one file** (Store, Accounts, Roles) and
  writes moved from once per session to two message rows plus two small reads
  per turn. WAL, `synchronous=NORMAL` and a 5 s busy timeout are set on all
  three. `synchronous=NORMAL` is the documented WAL pairing and trades a
  fsync-per-commit for the possibility of losing the last transactions on a
  hard power loss — not corruption. It was added because WAL alone produced
  3-72 s non-deterministic stalls in the sandbox this was built in.
- **`/login` has no rate limit.** bcrypt runs on its own single-thread executor
  now, so it can no longer contend with the per-turn embedding calls, but an
  unauthenticated endpoint that deliberately does expensive work is still an
  open door on a small container. Rate-limit it if the URL gets shared.
- **The startup purge had no test at all** until the last pass: deleting the
  call from the lifespan left all 380 tests green. It has one now.
- One subagent observed an intermittent native `libc++abi recursive_mutex`
  abort at interpreter shutdown, always after the test count printed, roughly
  1 in 10 runs. Five consecutive controller runs did not reproduce it. Likely
  an aiosqlite/anyio teardown flake, not this work — but if it turns up in CI,
  this is where it was first seen.

### Not done, deliberately

Document RAG (uploads) is slice 2, the dashboard that reads these conversations
is slice 3, MCP tool-calling is slice 4. `Store.conversation_rows()` exists and
is unused until slice 3.

**The twenty-second stalls are still the open bug.** Nine of sixteen remain
unexplained (see section 0a). None of the playground work touched the radio
link, and the uncommitted firmware changes in the working tree are from that
investigation, not this one.

---

## Where things run

| | |
|---|---|
| Server, cloud | `wss://voice-server-production-e023.up.railway.app/ws` |
| Railway | project `voice-companion`, service `voice-server` |
| Auth | `DEVICE_TOKEN`, 43 chars, in Railway vars and `firmware/main/secrets.h` |
| Database | SQLite on a Railway volume at `/data/voice.db` |
| Server, local | `uv run uvicorn server.main:app --host 0.0.0.0 --port 8000` |
| Device | serial port name is **not stable** - `ls /dev/cu.usbmodem*` before flashing; seen as both `usbmodem1101` and `usbmodem101`. WiFi `Xiaomi_CFB8`, gets `192.168.31.109` |

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
| Button B to GND | 20 | settings: hold 2 s |
| Status LED | 8 | onboard, **lights on LOW** |
| I2C SDA | 0 | SSD1306 `SDA` |
| I2C SCL | 1 | SSD1306 `SCL` |

Mic on **3V3**, amp on **5V**. The OLED takes 3V3 too. Do not use GPIO 9
(BOOT), 18/19 (USB), 2 (strapping).

GPIO 20 is U0RXD and is free only because the console is USB-Serial-JTAG. A
board with a CP2102 or CH340 bridge cannot use it. GPIO 21 is the last free
pin after it.

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
| Emotion tag rate | 29/30 tagged on the recorded run; brackets never reached spoken text |
| Provisioning, flash | **55.4 KB**, and the app partition still has 48% free |
| Provisioning, heap | **unknown** — not logged; measure before trusting it |
| Provisioning, end to end | works: phone → NVS → boots on the saved network. 28 Aug |
| Emotion spread | 6-7 of 9 emotions across three 30-question runs; `neutral` share 33-40% |
| Reply length | median 52-72 chars across three runs |
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

> **It did not reproduce on 28 Aug, and nothing was done to fix it.** Same
> setup as when the 67% was measured - board in the laptop's USB port - and:
>
> ```
> 6 packets transmitted, 6 packets received, 0.0% packet loss
> min/avg/max = 45 / 102 / 193 ms
> ```
>
> 102 ms is the power-save listen interval and is expected. RSSI held -59 to
> -71 dBm across four captures, and the slowest upload of the day was 217 ms
> with everything else at 4-17 ms. **Why it is fine now is unexplained.** Do
> not assume it is cured; do not assume it is broken. Run the ping below first
> and believe that, not this section.
>
> What *did* show up twice in four captures is a different fault wearing the
> same coat: `no data from server for 20 s in state 3, forcing idle` - with the
> link healthy. **It is not the radio and it is not the server either**: the
> device was dropping the question on the floor, and the server was waiting for
> an `end` that was never going to come. Chased and fixed on 28 Aug from
> `railway logs` alone - see item 0 of "Agreed next" for the evidence and for
> the one part of it that is still unaccounted for.


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

The value holds between stages but **eases across every one of them, on one
curve**. The first version eased only the last transition and let the others
step: the WiFi milestone moved the face 932 lit pixels in a single 40 ms frame
while every other frame moved by fewer than ten, and it read as a cut because
it was one. It is now spread over 400 ms, worst single frame 174 px.

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

- Host checks, 0 failures: **500** on the face, **50** on the provisioning
  logic, **14872** on the setup screen. `cd firmware/host && make test`.
  Everything runs twice — plain, and under `-fsanitize=address,undefined`.
  `make test ASAN=0` skips the second for a toolchain without the runtime.
  The sanitizer exists because a one-byte out-of-bounds read shipped past the
  plain build and its own wrong-length test, which passed a string literal —
  and a literal has enough bytes after it that the read lands in the same page.
- All four sketches build with **zero warnings**.
- 394 server tests. One reads the emotion names straight out of `face.c`, because
  the device matches them by substring and a rename would not raise anywhere —
  the face would just quietly stop changing.
- The text fallback reaches seven of the nine emotions by decision, not
  omission: `annoyed` cannot be read off a reply that is polite by
  construction, and `sleepy` must not be read off text at all — the firmware
  already falls asleep on its own 90 s timer.
- End to end against the real pipeline: `emotion happy` arrives at the same
  instant as `state speaking`, 370 ms before the first audio.
- gpt-oss tags 29/30 replies across three languages, with nothing leaking into
  the spoken text. The one miss was `Повтори ще раз, я не розчув.`, which came
  back as zero characters — the model returned nothing, so there was nothing to
  tag. No reply that had a tag lost it.

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

## WiFi from a phone

**Works.** A network picked on a phone, stored in NVS, and the device coming up
on it with `WIFI_SSID` empty in `secrets.h` — so the network came entirely from
provisioning. Design in
`docs/superpowers/specs/2026-08-27-wifi-provisioning-design.md`.

Measured on the board, 28 Aug:

| | |
|---|---|
| Access point | comes up in APSTA, DHCP on 192.168.4.1, DNS stub bound, **holds** — 40 s idle on laptop USB with no reboot or brownout |
| Captive portal | **pops up by itself on iOS** — `captive.apple.com` redirected to the page. Not relied on, and it happened anyway |
| Panel | the 5×7 font is legible on real glass; the status line reads |
| Boot on a saved network | IP in 2.0 s, TLS validated, socket connected in 10 s, STA only with no access point left up |
| Flash | 55.4 KB for the feature; app partition 48% free |
| Heap during provisioning | **still unknown** — not logged, and worth a number before trusting it |

**Three bugs the bench found that no amount of reading had.** The wiring called
`provision_start()` before `esp_wifi_init()`, so it could not start at all. The
page's network list was a static "No networks found yet." with a `/scan`
endpoint nothing ever called. And a successful trial did not end provisioning —
`PROV_GRACE_MS` was a constant nothing consumed, so a correctly configured
device sat in its own access point for five more minutes. All three are fixed.

**Not yet tried on the board:** the hold-to-reset gesture, because
`RESET_SILENCE_LEVEL` is still the unmeasured placeholder. See "Agreed next".

The network used to be compiled into `secrets.h`. Now NVS is the source and
`secrets.h` is the fallback for whatever NVS lacks, so a board flashed with a
filled-in header still joins immediately and is never asked anything. **The
template ships empty on purpose** — it used to say `"your-2.4GHz-network"`,
which is a non-empty string and would have counted as configured, so a board
built from it unchanged would have waited forever for a network that does not
exist instead of asking.

| File | Depends on | Does |
|---|---|---|
| `main/provision_logic.c` | **nothing** | form decoding, URI shape, the unlock counter, the mode choice |
| `main/setup_screen.c` | **nothing** | a 5×7 font and the four-line setup screen |
| `main/config_store.c` | `nvs_flash` | NVS first, `secrets.h` behind it |
| `main/provision.c` | ESP-IDF | the access point, `httpd`, a DNS stub, scanning, the trial |

The first two follow `face.c`'s rules — no ESP-IDF header, no float, no
allocation — so `host/` builds them. That is deliberate: form parsing and
screen layout are where the bugs are, and both are now testable on a laptop.

**How it behaves.** Nothing configured, or the hold gesture asked: raise an
**open** access point `Voice-XXXX`, show its name, `192.168.4.1` and a
four-digit code on the panel, serve one page. Pick a network, submit, and the
device trials it before anything is saved. Configured and no request: connect
and **wait forever**, exactly as before — a router rebooting is worth waiting
out, and an AP that appeared on its own would turn a two-minute outage into a
device that had stopped being a voice companion.

**Why the access point is open.** WPA2 would mean reading eight hex digits off
a 0.96" panel and typing them on a phone every single time, to stop a passerby
inside a five-minute window — and it would make a device with no display
impossible to provision at all. The lock sits on the one field that warrants
it: the server URI, behind the code, five wrong tries and the attempt is over.
Four digits are scriptable in under a minute; the attempt limit is what
protects it, and standing next to the device is what gets you more attempts.

**Why the panel is the source of truth.** Connecting the station forces the
access point onto the home network's channel (`wifi.rst:1660`), so the phone
can be dropped at the exact instant the password proves correct. A design whose
only success signal was a web page would report "it worked" and "it broke" as
the same silence. The page polls and is best-effort; the panel carries
trying / connected / wrong password / not found / timed out, and says so.

**The hold gesture reboots, and the spec said not to.** Hold the button and
stay quiet for five seconds — `s_audio_level` already knows whether anyone is
talking, so a long question can never trigger it — and the device records the
request in NVS and restarts into provisioning. The spec wanted an in-place
transition to keep the face's continuity. There is no safe one to write:
`wifi_start()` does one-time initialisation (`esp_netif_init`, the default
event loop, `esp_wifi_init`), so there is no second call to make, and inventing
a teardown that could be neither run nor reviewed here would have been worse
than losing an animation. This firmware already reboots for a clean slate.

**A stuck button cannot be distinguished from a deliberate silent hold** — the
signals are identical. It is handled by cost instead: nothing is erased on the
way in, and the access point returns to the saved network five minutes after
the last HTTP request.

---

## Memory retrieval and customization

Two gaps closed: facts were unranked (`recent_facts`, newest-first, no
notion of relevance to the current question) and there was no way to see or
add to what the assistant remembers short of editing `persona.py` and
redeploying. Both closed in the same schema change: `Fact` gained a
`source` column. `"auto"` facts (today's end-of-session extraction) are now
ranked by embedding similarity to the live question, every turn. `"user"`
facts are standing instructions typed through a new HTTP API and are never
ranked — a rule like "always answer informally" is not a fact to judge for
relevance.

**Embeddings are local:** `fastembed` (ONNX Runtime, no PyTorch) — free
forever, no external account, no per-request cost. `server/memory/store.py`
still imports nothing from it; embedding happens in `session.py`, Store
only stores and ranks vectors it is handed, in pure Python (`struct.pack`
into a `BLOB`, cosine similarity by hand) — a vector database would be
solving a problem this device's memory will not reach the size of.

**The model name was guessed wrong once, and checked before it shipped.**
`intfloat/multilingual-e5-small`, the obvious pick from general knowledge of
what covers uk/ru/en cheaply, is not in this fastembed version's registry —
`TextEmbedding(...)` raises `ValueError` naming the supported list.
`multilingual-e5-large` is there but 2.24 GB, too big for Railway's memory
ceiling. `sentence-transformers/paraphrase-multilingual-MiniLM-L12-v2` is
what actually shipped: 0.22 GB quantized, and — per fastembed's own
registry entry — does not need the query/passage prefix e5 models do, so
`FastEmbedEmbedder` applies none. `Embedder` still has two methods
(`embed_documents`/`embed_query`) rather than one, because that split is
what makes a future asymmetric model a one-file change instead of a
call-site hunt.

**Verified, real model, not mocked:** loaded once outside the test suite
and asked to rank two facts against two questions in uk/ru mixed with the
literal Ukrainian words used elsewhere in this project -
`Розкажи про мого кота` against "Has a cat named Musya" scored 0.75,
against "Lives in Kyiv" scored 0.145; `Де я живу?` scored 0.303 against
the Kyiv fact and 0.057 against the cat fact. Ordering is right both ways
and the margins are not close.

**Verified since, inside the real app, real Groq keys, two real turns on a
fresh scratch database.** Turn one, real STT: `Мене звати Соломія, я живу у
Чернівцях.` Extraction stored three auto facts, each embedded for real -
confirmed in the database directly, not inferred: `length(embedding)` is
1536 bytes for all three, exactly 384 floats. A **second, independent**
connection then asked `У якому месте я живу?` (Whisper's own transcription,
typo and all) and the real model answered `Ти живеш у Чернівцях.` - correct,
and only reachable through retrieval, since nothing in that second
session's history mentioned the city. `EMBEDDING_CACHE_DIR` also checked
for real: pointed at a scratch directory, the model landed there (240 MB
including HF's own metadata, not just the 0.22 GB model file), so a second
boot would not redownload it.

**Deployed and verified against the real Railway volume, 29 Aug.** By the
time this shipped the volume held seven pre-existing facts, not one - real
traffic had accumulated overnight (see the live conversations in that day's
`railway logs`, none of them mine). `backfill_embeddings` ran once at boot
and gave all seven real embeddings with no error. Asked the live device's
own endpoint `Чим я цікавлюсь і якою мовою мені відповідати?`, the real
reply was `Ви цікавитеся програмуванням і любите жарти. Відповідати можна
українською...` - both halves correct, and both came from facts that
existed **before this feature did**: `backfill_embeddings` is what made
them reachable at all.

`healthcheckTimeout` had to move from 30 to 120 first: the model loads
before `/healthz` can answer, and a cold download measured up to 36 s
locally. Turned out not to matter on Railway's own network - the deployed
container fetched all 5 files in 2 s - but 30 s was one slow network away
from failing the health check and rolling the deploy back, so the bump
stays. `EMBEDDING_CACHE_DIR=/data/fastembed_cache` is set on Railway now,
not just proven locally.

## Agreed next, in order

### 1. Build-time switch between LAN and cloud — **answered, not dropped**

This asked for `idf.py -DSERVER=lan` and `-DSERVER=cloud`, because `SERVER_URI`
was compiled in and switching meant a reflash. WiFi provisioning made it moot:
the URI is settable from the phone, behind the unlock code, so there is nothing
left to switch at build time. Left here rather than deleted so nobody
rediscovers it as an open item.

The complaint underneath it stands and is not addressed: local felt instant and
always worked, Railway is ~300 ms slower and drops far more often, because TLS
needs several round trips in succession and falls apart on a lossy link where
plain TCP scraped through. What changed is that switching between them now
costs a long press instead of a toolchain.

### 0. The twenty-second stalls — **half of it found and fixed; see 0a**

The lead was `no data from server for 20 s in state 3, forcing idle`, twice in
four captures on 28 Aug with the radio measurably healthy. `railway logs
--json` answered it without the board, and the answer was not the server going
quiet. **The device was throwing away the question.**

**What the server's own log says.** Timestamps on 28 Aug:

```
11:15:32.913  reply cancelled by the device
11:16:33.103  utterance timed out after 60.0s
11:16:33.103  utterance of 0.22 s is too short to be speech, not transcribing
```

The 60 s watchdog is armed by `on_start`, so it fired 60.00 s after a `start`
the session accepted at 11:15:33.10 — **0.19 s after the cancel**. So the
device did begin the next question. It then sent **0.22 s of audio and no
`end` at all**, and the user heard nothing for a full minute. Three questions
later they asked the device `почему-то только что не отвечал`.

**Why.** `done` was not scoped to a reply. The server sends one for *every*
reply task including a cancelled one, and that `done` necessarily lands after
the device has already started recording again — measured at 0.3 s twice and
7 s twice, which the interrupt work above had already established. The device
applied it to whatever it was doing now: `audio_out_task`'s "reply produced no
audio" rescue fired unconditionally and forced `ST_IDLE` **out from under
`ST_LISTENING`**. `end` is only ever sent from `ST_LISTENING`, so the release
sent nothing, and the question was recorded into a state that could not deliver
it. It is the same late `done` the discard-window fix already proved dangerous;
only the discard window was moved off it, not this.

The server then made it worse by being quiet about it: `on_start` returned
early for any state that was not `IDLE`, at `log.debug`, so a device pressing
again was ignored without a line in the log — and the abandoned fragment stayed
in the buffer, ready to be prepended to the next question.

**Fixed.**

- `voice_main.c` — the rescue only fires in `ST_THINKING` or `ST_SPEAKING`,
  which are the only states a reply lives in. The flag is cleared either way,
  because leaving it raised just moves the damage to the next `THINKING`.
  Anything else logs `stale done ignored in state N`.
- `session.py` — a second `start` with no `end` between them starts a clean
  utterance: buffer cleared, watchdog re-armed, and one `INFO` line saying how
  many bytes were dropped.
- Three tests, all of which fail without the fix: two in `test_session.py`, and
  `test_an_utterance_the_device_abandons_does_not_deafen_the_session` in
  `test_main.py`, which replays the whole cancel → start → go-deaf → start
  sequence over a real socket.

**What is verified and what is not.** The server half is covered by tests
(223 pass) and is **deployed**. The firmware half builds with zero warnings,
48% of the app partition free, and is **flashed to the board** — but
`voice_main.c` is not host-testable and **the confirming log line has never
been read**. On the bench, one interrupted reply is enough: **`stale done
ignored in state 1` is the fix working.** Seeing `idle (reply produced no
audio)` while the button is down is the bug still there.

### 0a. …and nine of the sixteen stalls are still unexplained — **the real next move**

The full 400-line window holds **16** twenty-second stalls, not the eight the
first pass saw. They span 19.75–20.10 s against 98 ordinary gaps whose median
is 3.48 s and whose maximum is 9.41 s — no overlap, so every one of them is the
device's watchdog and not a person. But they split:

| Preceded by | n | |
|---|---|---|
| `reply cancelled by the device` | 7 | the fault above, fixed |
| a **completed** reply | 9 | **not explained, not fixed** |

The nine follow a reply that finished normally — no cancel, no stale `done`.
And the gap is anchored to `done`, not to when the speaker went quiet: playback
end varies over 2.57 s across the nine while the gap varies over 0.12 s. So the
device sat in a non-idle state for twenty seconds *after the reply was fully
delivered*. That is `ST_SPEAKING` that never ended — **exactly the `state 3` in
the original report.**

So the fix above closes the "it stopped answering" complaint and the proven
no-`end` case. It does **not** close `state 3`, and it was wrong to imply it
did. The server cannot see any further: it has sent everything it owes. The
next move is a serial capture across an ordinary conversation with **no
interruptions**, watching whether `idle: played … B` appears after each reply.

Full evidence, the ruled-out theories and the bench recipe:
`docs/superpowers/measurements/2026-08-28-twenty-second-stalls.md`.

### 1a. Finish taking provisioning to the bench

The path a user actually walks — raise the access point, pick a network on a
phone, come up on it after a reboot — is done and recorded above. What is left:

1. **Measure `RESET_SILENCE_LEVEL`.** It is 400 in `voice_main.c` and that is a
   placeholder, said so in the comment, and it is the only thing standing
   between the hold-to-reset gesture and a first try. Log `s_audio_level` for
   thirty seconds with the button held in a quiet room, then again while
   speaking at conversational distance, and put the threshold between the two
   ranges nearer the quiet one. **If the ranges overlap, the silence gate does
   not work in that room** and the gesture needs rethinking rather than a
   number splitting the difference.
2. Then hold silently — the countdown should appear and complete, and the
   device should restart into provisioning. Hold and talk — it should never
   complete.
3. Enter provisioning and walk away; it should return to the saved network five
   minutes after the last HTTP request.
4. **Log the heap while the access point is up.** Nothing measures it, and this
   device runs at 55 KB free during a conversation. `httpd` with two sockets, a
   DNS task and the AP are not free.
5. **Does the access point still hold with a phone attached and traffic
   flowing, on the phone charger rather than laptop USB?** It held idle for
   40 s on USB, which is the encouraging half. An access point cannot use modem
   sleep, and this board has died twice from an awake radio.

**One known residual, and only a board can size it.** When a trial times out,
the code cancels the attempt and waits up to 300 ms for the cancellation's own
disconnect event before clearing the trial flag. If that event takes longer, it
lands on `wifi_event`'s ordinary branch, which reconnects forever — with the
credentials the user was just told had failed. The window is narrowed, not
closed, and the code says so where it happens.

### 2. Cleanup, once the link is sound

- Remove instrumentation: `send of ... took`, `starved`, `rssi`, reply logging.
- `capture` uses mono slot mode while `voice` uses stereo. Both work, but they
  should agree before someone reads the wrong one and believes it.
- Facts on Railway start empty — the volume is new, so the assistant has to
  learn the user's name (Катерина) again. Nothing to fix, just surprising.

### 3. What the emotion work left open

- **Reaching `excited` and `surprised` at all.** They appeared in none of the
  six runs. A glossary of the nine was the spec's recorded next move and is
  untried; it needs the 400-token cap in `test_persona.py` raised on purpose.
  Anything tried here must be measured with `scripts/emotion_survey.py`
  against the spread and length figures recorded above, and three runs, not
  one — a single draw moves by ±1 emotion on its own.
- **Emotion during `listening` and `thinking`.** The firmware accepts an
  emotion frame at any moment — `s_face_emotion_seq` re-triggers even on a
  repeat — and the server uses that exactly once per reply, just before it
  starts speaking.
- **Mood across turns.** The emotion is written neither to `history` nor to
  the store, so nothing carries between replies and no distribution
  accumulates from real traffic.

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

## Interrupting: four bugs, all found by the log

Pressing the button during a reply should silence it at once and start
recording. It did neither reliably, and reading the code found none of it -
every one came out of `idle.py`-style serial capture with timestamps.

**Measured after the fixes:** press to silence is **14-31 ms**, plus 25-30 ms
of button debounce ahead of it. No playback restarts after an abort.

1. **The tail was still on the wire.** Resetting the play buffer discards what
   arrived; the server runs `playback_lead_s` - four seconds - ahead of real
   time, so the rest of the reply lands a moment later and the amp comes back
   on. Incoming audio is now dropped between the cancel and the next reply's
   `speaking`.
2. **The discard window was closed by the wrong event.** It ended on the
   cancelled reply's `done`, which arrived 0.3 s after the interrupt twice and
   **7 s** twice - and in the slow cases the answer to the *next* question was
   thrown away as though it were the old tail. `speaking` of the next reply is
   the correct boundary; the server sends it immediately before the first audio
   of every reply.
3. **The prebuffer wait un-muted what the abort had muted.** `audio_out_task`
   waits for enough audio before opening the amp, and that wait did not look at
   the abort flag - so an interrupt arriving during it was honoured and then
   reversed by the `amp_enable(true)` on the next line.
4. **A blocked write refilled the buffer that had just been cleared.** The
   receive handler refuses stale audio, but its write blocks when the buffer is
   full and a send already waiting inside `xStreamBufferSend` has passed that
   check. The abort frees the room, the send wakes, and four seconds of the
   abandoned reply play. `playback aborted` and `playing` in the same
   millisecond. **This is why it was intermittent** - it needed a full buffer.
   The reader now checks the same flag as the writer.

**And the state machine can be wrong.** The stuck-state timer forces `ST_IDLE`
after 20 s of server silence; the log caught audio starting three milliseconds
later. A press then took the "new utterance" path and the abandoned reply
talked over the question being recorded. The interrupt now also fires on
`s_playing`, which `audio_out_task` owns: whether sound is coming out is a
fact, what the state machine believes is an opinion.

**Still not perfect**, by the user's judgement. The remaining budget is the
25 ms debounce plus 14-31 ms to mute plus the amplifier's own shutdown. The
cheap next move is a shorter debounce on the press edge than on the release -
worth ~20 ms, at the cost of making a spurious press cheaper to trigger, which
now matters because five of them enter provisioning.

---

## Traps already paid for

- **`esp_wifi_set_config()` writes to flash unless you tell it not to.** The
  default is `WIFI_STORAGE_FLASH` (`esp_wifi.h:1038`) and nothing in this
  project had ever called `esp_wifi_set_storage()`. So provisioning's "try the
  password before saving it" was writing the unverified password to the
  driver's own NVS the instant it configured the station — everything the
  design was built to prevent, through a second NVS nobody had thought about.
  `provision_start()` now sets `WIFI_STORAGE_RAM`, which costs nothing here
  because `wifi_start()` reapplies the configuration explicitly on every boot.
- **An access point cannot use modem sleep.** Sleep is a station feature
  (`wifi.rst:1773`, `:1795`), and an AP has to beacon. So provisioning holds
  the radio in exactly the state that produced a 32-second 1 KB send the two
  times `WIFI_PS_NONE` was tried here. It should survive — the amplifier is
  silent and nothing is streaming — but that is reasoning, not a measurement,
  and it is the first thing to check on the bench.
- **Source order is not synchronisation.** A fix that called
  `esp_wifi_disconnect()` before clearing a flag looked correct and was not:
  the disconnect event is dispatched on the event-loop task while the flag
  clears on the HTTP task, so the flag almost certainly won. Ordering two
  statements in one task says nothing about a third.
- **Substring matching on short Cyrillic words is a trap.** `"ого"` as a
  surprise marker hides inside `нічого`, `нікого` and `когось`, so
  `"Дякую, нічого не потрібно."` came back `surprised` despite containing
  `дякую`. Word boundaries fix that class; they do not fix `надо же` or
  `no way`, whose boundaries are intact. See `_SURPRISED` in `server/emotion.py`.
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
- **Substring matching on short Cyrillic words is a trap.** `"ого"` as a
  surprise marker hides inside `нічого`, `нікого` and `когось`, three of the
  commonest words in an ordinary Ukrainian reply — so `"Дякую, нічого не
  потрібно."` returned `surprised` despite containing `дякую`, because the
  surprise list is checked before the greeting list. The same trap was also
  live in `_CONFUSED`: `"уточни"` is six characters and sits inside
  `уточнити` and `уточнив`, so `"Хочу уточнити деталі замовлення."` returned
  `confused` for a sentence with no confusion in it. Both lists are now
  matched on word boundaries; `_SORRY` and `_HAPPY` stay plain substrings
  because their entries are genuinely long enough to be safe — that claim now
  covers two lists, not four, because it was never true of `_CONFUSED`.
  Python's `\b` is Unicode-aware, which is most of why the fix is small.
  `\b` alone was not quite enough for `ого`, though: a hyphenated ordinal
  like `"21-ого"` puts a hyphen directly in front of it, and a hyphen reads
  as a word boundary too, so a lookbehind excludes that one shape. Two
  entries in `_SURPRISED` — `надо же`, `no way` — have no substring problem
  at all; their false positive is meaning, not spelling (`"Надо же ещё раз
  перевірити документи"` reads as "also need to", not as the interjection),
  and `\b` cannot fix meaning. Anchoring both to a sentence start closes the
  mid-sentence case but not a sentence-initial one, which stays a known,
  documented residual — see the comment above `_SURPRISED` in
  `server/emotion.py`. `"що саме"` in `_CONFUSED` is left alone for the same
  reason: it is a semantic ambiguity, not a matching bug, and no amount of
  `\b` closes it.

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

The tag rule earned a place here too: the theory was that `Start every reply
with how you feel about it` was collapsing the model onto `[neutral]`, because
an assistant answering a weather question feels nothing about it. Measured
over thirty questions, three runs each, the reframe made the `neutral` share
worse on every paired draw — the worst old run beat the best rewrite by
thirteen points. The old wording is what ships.

The rewrite also made replies shorter, the other way round: median 52, 66, 72
characters for the old wording against 40, 48, 51 for the rewrite, paired
reductions of 23%, 27% and 29% — call it about a quarter shorter, not a
third. Brevity does outrank variety in this project, so that is a real gain
— but it was not what the change was for, and it was not worth a worse face.
If shorter replies are wanted, the brevity rule is the honest place to ask
for them.

The premise behind the rewrite was also wrong: it assumed the model had
collapsed onto two or three emotions. The kept wording reached six or seven
of nine on every run (7, 7, 6); the reverted wording dropped to five twice
(5, 5, 6). `excited` and `surprised` never appeared in any of the six runs —
that is the real gap, not `neutral`-collapse.
