# Shared identity and a real web client — design

The ask that started this was "a JARVIS-like ecosystem" — a web app reachable
from phone and laptop, integrations, MCP connections, still serving the
ESP32. That is not one project. It decomposes into at least five, and they
do not share a spec: identity across surfaces, the web client itself,
tool-calling/integrations, proactivity, and device-fleet health.

This spec is the first of those, chosen because everything else is built
against it: if the web client and the ESP32 turn out to be one assistant or
two is a decision every later piece inherits, and it is cheap to get right
now and expensive to unwind once a chat UI, a settings page and an
integrations layer all assume one answer or the other.

## The decisions, and why

**One identity, not two.** Talking to it from a laptop should draw on facts
it learned from voice conversations on the device, and vice versa. The
alternative — separate memory per surface — is simpler (today's `device_id`
scoping already gives it for free) but stops being one JARVIS and becomes
two chatbots that happen to look similar. `Fact` rows stay scoped by
`device_id`, and a logged-in web session operates against the same
`device_id="default"` the ESP32 already uses. No new linking table: there is
one person and one device today, and a `User → devices` join table for a
population of one is a table with nothing to join.

**Facts are shared; the live conversation is not, yet.** What persists
(`Fact` rows, auto and user-authored) reaches every surface. The in-the-
moment back-and-forth within one connection stays exactly as scoped as it is
today — private to that WebSocket. Real-time cross-surface sync (ask
something on the device, watch it appear on the laptop mid-conversation) is
explicitly wanted later, so nothing here should make it harder — the reason
`on_text` reuses the existing pipeline instead of forking a parallel one is
exactly this: one event stream (`state`, `emotion`, `reply`) to eventually
fan out to more than one open connection, not two independent ones to
reconcile afterward.

**Persona stops being one hardcoded string.** The ESP32's rules — one or two
sentences, no markdown, no lists — exist because it is spoken aloud and
because that wording was measured (see RESUME.md). None of that applies to a
laptop chat window. `build_system_prompt` takes a small `Style` (verbosity,
whether markdown is allowed) resolved per surface, defaulting to today's
rules for `esp32` and a longer-form default for `web` — and both become
user-editable from the new Settings page, because "make it configurable"
was the explicit ask, not a guess at what the right fixed answer is.

**Login is additive, not a replacement.** `DEVICE_TOKEN` keeps authenticating
the ESP32 exactly as it does today, in production, unmodified — there is no
functional reason to touch a working, already-deployed path. The web login
is a second, independent door: a password verified against a `bcrypt` hash,
a signed session cookie on success. `/ws` accepts either credential.
Unifying them into one credential system is the more "correct" long-term
shape, but it buys nothing today and risks the one thing that must not
break.

**Password before passkey.** WebAuthn is nicer day to day but is real added
machinery — a registration ceremony, credential storage, browser quirks —
for an app with one account. A password plus a signed cookie is the whole
mechanism, well-understood, and does not block adding a passkey later; it
would sit next to the password check, not replace the session model
underneath it.

**A real client, not a throwaway.** The alternative — a minimal text box
just to prove shared memory works, with the "real" app built later — means
building the web surface twice. Since the destination is known (chat,
settings, memory, all in one place), building it once is less total work
than building it twice, even though it makes this spec larger.

**React + Vite, breaking this project's own pattern on purpose.** Every
other piece of this project avoids frontend frameworks entirely — the memory
page is hand-written HTML with zero build step, deliberately. A login flow,
a chat view with streaming replies, a settings page and a memory editor is
enough surface, with enough shared state between them, that hand-rolled
vanilla JS would mean reimplementing what a framework already does well.
This is the first build step and the first `package.json` in the repo; that
cost is accepted here rather than papered over.

**Responsive by default, not a separate mobile build.** "Use it by my phone"
was in the original ask, not an afterthought — the React app is built
mobile-first (one layout that adapts, not a phone app and a desktop app),
since a single person with one account has no reason to maintain two. An
installable PWA shell is a cheap follow-on once the responsive layout
exists, not a requirement of this spec.

**Text is the primary surface; voice is supported, not required.** Typing is
faster to produce and faster to read back than speaking and listening, on a
laptop. The mic stays available for when voice is actually wanted, using the
exact same audio framing the ESP32 already sends — the browser becomes
another device on the same protocol, not a new one.

**A typed question gets a written answer.** Synthesizing speech for
something you are already reading is wasted latency and a wasted TTS call.
A spoken question still gets a spoken answer, exactly as today. This is a
per-message property (how the question arrived), not a per-session mode.

**The old memory page retires.** Keeping it alongside the new app means two
places that can edit the same data, one of which has no login. The new
app's Settings page covers the same ground behind the same auth every other
page in the app already requires.

## Architecture

Still one Railway service, one process. It now serves three things: the WS
voice protocol (unchanged for the ESP32), the JSON API (unchanged shape,
newly reachable via cookie as well as bearer token), and a built React+Vite
static bundle, served at the root path. Same origin throughout, which is
what makes the session cookie simple — no CORS, no separate domain, no
cross-site cookie flags to fight.

`/ws` authorises against *either* `_token_ok` (existing, bearer, ESP32) *or*
a valid session cookie (browser). Both paths construct the same `Session`
with the same `device_id`; the pipeline downstream has no idea which door
the connection came through.

Text chat is not a second system bolted on next to the voice one. A new
inbound control frame, `{"type":"text","value":"..."}`, is handled by a new
`Session.on_text(text)` that skips STT and the PCM buffer entirely and
lands in the same place `_respond` already lands after transcription today
— which means `_respond` splits into "get text" (from STT, or now directly)
and "answer it" (memory retrieval, prompt assembly, LLM streaming, per-
sentence delivery), so there is exactly one implementation of "answer a
question," not two that can drift.

Per-sentence delivery already exists for TTS (`say()` in `session.py`); the
text path reuses the same per-sentence split but emits
`{"type":"reply","value":"<sentence>"}` frames instead of rendering audio,
so a typed question still streams in progressively rather than arriving as
one block once the whole reply is done. `state` and `emotion` frames go out
unchanged either way, so the web UI can show "thinking…" and reflect mood
the same way the ESP32's face already does.

## What's new

- **`server/accounts.py`** — a small `Accounts` class next to `Store`, same
  SQLite file, one `users` table. `bcrypt` is a new dependency here, and
  deliberately so: rolling password hashing by hand is a real security
  mistake, and bcrypt is small, has no heavy transitive dependencies, and is
  the standard tool for exactly this job — unlike the embedding-model
  minimalism elsewhere in this project, this is a place where reaching for
  the well-vetted library is the right call, not the lazy one.
- **Session cookies** via Starlette's `SessionMiddleware` — already inside
  FastAPI's dependency tree, so this is zero new dependencies, not one. A
  new `SESSION_SECRET_KEY` setting signs it; it needs to exist on Railway
  before this deploys, same category as `DEVICE_TOKEN` and `GROQ_API_KEY`
  today.
- **`server/main.py`** — `POST /login` (password in, cookie out, generic
  401 on failure), `POST /logout`, a `require_login` dependency mirroring
  the existing `require_token`, and a static mount serving the built
  frontend. The standalone `GET /memory` HTML route is removed once the new
  app's Settings page covers it.
- **`server/session.py`** — `on_text(text)`; `_respond` split as described
  above.
- **`server/persona.py`** — `build_system_prompt` takes a `Style`; a small
  per-surface default (`esp32` = today's rules, unchanged; `web` = longer
  form, markdown allowed), overridable from Settings.
- **`web/`** (new directory) — the React+Vite project: a login page, a chat
  page (text input, mic button, streaming replies, state/emotion display),
  and a Settings page (persona/style controls plus the memory list/add/
  delete that `GET /memory` used to own alone).

## Error handling

Wrong password: 401, one generic message — no username enumeration, even
for an account population of one. No valid cookie and no valid bearer on
`/ws`: the existing 4401 close, unchanged. A cookie expiring mid-use: the
next API call returns 401, the app sends the user back to login. A typed
message arriving while a reply is still in progress: treated as an
interrupt through the same path a device button-press already uses — one
interrupt implementation, not one per surface. A browser tab closing
mid-conversation: the same `finally: await session.finish()` that already
runs for the ESP32 fires here too, so facts still get extracted from an
abandoned browser session exactly as they would from a dropped device
connection.

## Testing, and what can't be tested here

Backend testing follows this project's existing shape: `tests/test_
accounts.py` for user creation, password verification, and rejection of a
wrong one; `tests/test_main.py` additions for `/login`, `/logout`,
cookie-gated `/ws` and `/memory` access, and the text-frame path, structured
the same way the existing PCM-path tests are.

What is not covered by anything that runs in this environment: the actual
React app in an actual browser. There is no browser tool available here —
same limitation the memory page had, with more surface this time (a login
flow, streaming chat rendering, a mic permission prompt). The person who
builds this needs to be the one who drives it in a real browser before
trusting it, the same way `voice_main.c` changes need the bench, not just a
green test suite.

## Deliberately out of scope

Tool-calling, MCP, integrations — the LLM pipeline stays a pure
STT/text → LLM → TTS/text chain here; giving it the ability to call
anything is its own spec, independent of this one, and arguably the largest
piece of the original "JARVIS" ask. Proactivity — the assistant reaching out
on its own — needs background jobs and a way to reach a person that does
not yet exist; not here. Device-fleet health (battery, RSSI, uptime as a
dashboard) — a monitoring concern, not an identity concern; not here.
Real-time cross-surface conversation sync — explicitly wanted later,
explicitly not built now; the architecture above is chosen so it is an
addition later, not a rewrite. Multi-device or multi-user modeling — there
is one person and one ESP32; a `User → devices` table is a real thing to
build the day a second device or a second person shows up, not before.

## Order of work

Roughly: the `users` table and password/session plumbing first, since
nothing else can be tested end-to-end without a way to log in. Then the
`on_text`/`_respond` split and persona `Style`, which are pure backend and
testable without any frontend at all. Then the React app, built against a
backend that already has everything it needs. A full implementation plan is
the next step, not this document.
