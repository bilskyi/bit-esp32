# Voice companion — server

The ESP32-C3 is a thin audio terminal. Everything that thinks lives here.

Push-to-talk in, spoken reply out, over one WebSocket. This half of the project
is deliberately buildable and testable before any firmware exists.

## Quick start

```bash
uv sync
cp .env.example .env
```

Run it with no API key at all — the audio path is real, only STT and the LLM
are stubbed:

```bash
PROVIDER_MODE=mock uv run uvicorn server.main:app --port 8000
```

Then, in another terminal, pretend to be the device:

```bash
uv run python scripts/fake_device.py --say "Привіт, як твої справи?"
```

It synthesises the question, streams it as microphone audio, releases the
button, collects the reply into `reply.wav`, and prints where the time went:

```
upload took 4 ms, button released
  state listening  +      0 ms
  state thinking   +      0 ms
  state speaking   +     68 ms
  first audio      +    560 ms  <-- the number that matters
  done             +   1506 ms
button release -> first audible sound: 560 ms (within the 1500 ms target)
```

For the real pipeline, put a Groq key in `.env` and drop `PROVIDER_MODE`.

## Tests

```bash
uv run pytest
```

398 tests, no network, no hardware, under ten seconds.

The web app has its own, deliberately narrow suite — 31 tests for logic a
browser check cannot falsify (the socket hook's cancel accounting, the audio
resampler, the role patch payload, the face's bit unpacking):

```bash
npm --prefix web test
```

Everything visual is checked by a person in a browser. That is thinner than
the Python side and the frontend plan says so.

## Protocol

One persistent WebSocket. Binary frames are audio in both directions; text
frames are JSON control messages.

| Direction | Frame | Meaning |
|---|---|---|
| device → server | `{"type":"start"}` | button pressed, audio follows |
| device → server | binary | PCM chunks, sent *while* the button is held |
| device → server | `{"type":"end"}` | button released, utterance complete |
| device → server | `{"type":"text","value":"..."}` | typed question, no audio to follow |
| server → device | `{"type":"state","value":"listening\|thinking\|speaking\|idle"}` | drives the LED |
| server → device | binary | PCM chunks of the reply |
| server → device | `{"type":"done"}` | playback finished, re-arm mute |
| server → device | `{"type":"emotion","value":"curious"}` | drives the face, ahead of the first word |
| server → browser | `{"type":"reply","value":"<sentence>"}` | a typed question streams back as text, one sentence at a time — no TTS spent on something already being read |
| server → browser | `{"type":"trace","value":{…}}` | what the turn actually did: active role, retrieved facts with their similarity scores, the assembled prompt, token counts, per-stage timings |

The `trace` frame goes **only** to a connection authorised by a session cookie.
The device never receives it: it has about 55 KB of free heap mid-conversation
and no use for the payload. On an open dev server with no login, a browser is
labelled `esp32` and gets no `trace` frames either — it fails closed, so log in
if the inspector looks empty.

Audio is 16 kHz, 16-bit signed, mono, little-endian, raw PCM — in both
directions, so the firmware never converts anything.

Authentication is a shared secret in the handshake:

```
Authorization: Bearer <DEVICE_TOKEN>
```

Leaving `DEVICE_TOKEN` empty disables the check. Set it before the URL is
public: an open socket lets anyone drain the Groq free tier.

An unauthorised connection is closed with code 4401 — but the close happens
*before* the handshake is accepted, which Starlette answers as an HTTP 403, so
a browser's `onclose` sees a generic abnormal closure and never that code. The
device reads the close; a browser has to ask `GET /me` to tell "my cookie is
gone" from "the server is down". `/ws` also
accepts a logged-in session cookie as an alternative credential, so a browser
that has called `POST /login` can open the socket with no bearer token at
all.

A typed question (`"text"`) gets a written `{"type":"reply","value":"..."}`
reply, streamed sentence by sentence like TTS is - no audio is synthesised
for it. A spoken question still gets a spoken reply, exactly as before.

Multiple devices are separated by `?device=<id>`, which scopes their memory.

## How a turn works

On `end` the buffered audio goes to STT, the transcript is appended to history,
the LLM response is streamed, and **each sentence is sent to TTS the moment it
is complete** rather than waiting for the full reply. That pipelining is where
the latency budget is won.

The voice is chosen from the *first* sentence and held for the rest of the
reply — detecting per sentence would switch voice mid-answer, which is audible.

## Memory and customization

Two kinds of memory, told apart by who put them there:

- **Auto facts** — extracted by the LLM at the end of every session (`memory/summarise.py`), embedded, and ranked against the live question each turn (`Store.relevant_facts`). Real cosine-similarity retrieval, not just "the last 20" — but done in pure Python over a `BLOB` column, not a vector database, because a personal device's memory never reaches the row count where that would pay for itself.
- **Standing instructions** — typed by the user, e.g. "always answer informally". Never ranked: they apply every turn, in full.

Embeddings are local: `fastembed` (ONNX Runtime, no PyTorch) with
`paraphrase-multilingual-MiniLM-L12-v2` (~0.22 GB, covers uk/ru/en). No paid
API, no external account, no per-request cost or rate limit.

An HTTP API manages both, behind the same `DEVICE_TOKEN` bearer scheme as
`/ws` — one shared secret, not per-device, same as the WebSocket. A logged-in
web session's cookie works here too, so the same routes are reachable from
either door:

| Method | Path | Does |
|---|---|---|
| `GET` | `/memory/{device_id}` | list everything remembered, with its source |
| `POST` | `/memory/{device_id}` | add a standing instruction (`{"text": "..."}`) |
| `DELETE` | `/memory/{device_id}/{fact_id}` | remove one entry |
| `DELETE` | `/memory/{device_id}` | drop this device's facts **and** its recorded conversations |

`RELEVANT_FACTS_LIMIT` (default 6) caps how many auto facts reach the prompt
per turn. `EMBEDDING_CACHE_DIR` should point at the same Railway volume the
database uses, or every redeploy re-downloads the model.

## Running the web app

Two processes in development, so the frontend hot-reloads:

```bash
PROVIDER_MODE=mock SESSION_COOKIE_SECURE=False uv run uvicorn server.main:app --port 8000
npm --prefix web dev          # Vite, proxying the API and /ws to :8000
```

`SESSION_COOKIE_SECURE=False` is not optional over plain `http://` — the
browser silently discards a Secure cookie, so `/login` returns 200 and every
call after it returns 401. Same when testing from a phone on your LAN. Note
browsers also require HTTPS or `localhost` for microphone access, so voice
needs `localhost` or a tunnel.

Create the login before you try to sign in:

```bash
uv run python scripts/create_account.py --username you --password <something>
```

To serve the built app from FastAPI itself, as production does:

```bash
npm --prefix web run build     # writes web/dist, which FastAPI mounts at /
```

The device's eyes in the browser are **generated from the firmware**, not
reimplemented — `firmware/host/face_preview.c` records why: a JavaScript copy
of `face.c` drifts from the original within a day. Regenerate them only when
`face.c` changes:

```bash
make -C firmware/host face-data   # writes web/public/face-frames.json
```

Deployment is a two-stage `Dockerfile` (Node builds the bundle, Python runs
the server) rather than nixpacks, which has to be coaxed into a dual-runtime
build.

## Roles: the personality, as rows

The prompt is not one hardcoded string any more. A **role** is a persona
section plus four knobs — how many sentences, whether markdown is allowed,
which of the three available languages, and an optional pinned mood — and each
surface (`esp32`, `web`) points at one. The device can run "Terse" while the
laptop runs "Coach", off the same memory.

Two roles are seeded and marked `built_in`: they cannot be deleted or renamed,
because `ensure_defaults()` re-seeds by that flag on every boot and a renamed
built-in would come back as a duplicate.

`prompt: null` is a real value, not an omission — it is how a customised role
reverts to the built-in wording. That matters most for the device: **the
measured prompt is the default and stays byte-identical** (`persona.BASE`, with
a golden test asserting the assembly reproduces it exactly — it was measured,
and rewording it made the emotion spread worse across three runs of
`scripts/emotion_survey.py`). You *can* override it now, unlike before, but the
revert is one field away.

Two invariants are appended to every prompt regardless of what a role says: the
emotion-tag rule, because it drives the device's face, and the language
restriction, because `lang.VOICES` has voices for uk/ru/en and edge-tts emits
silence for anything else. A role may narrow those three, never extend them.
Markdown is force-disabled whenever a reply will be spoken, whatever the role
asked for, because TTS reads asterisks aloud.

A role change and a pinned mood take effect on the **next turn** of an
already-open socket, not the next connection — the device holds one socket for
days, so per-connection resolution would have meant rebooting it to hear a
change.

## Conversations are recorded

Every answered turn is stored as two rows (question, reply) under a
conversation per connection. This is what the planned dashboard reads; there is
no UI for it yet.

It is personal data, including other people's — strangers found the deployed
server before the token was set — so: recording defaults **on** and is
switchable at runtime, `retention_days` defaults to **90**, and expired
messages are swept both at startup and after each session ends. `retention_days
= 0` means keep forever. Expiry is keyed on each message's own timestamp, not
the conversation's, because one conversation row can span days on an
always-connected device.

**One gap to know about:** facts are extracted *from* these transcripts at
session end, and facts are not covered by `retention_days`. Deleting or expiring
a conversation does not remove the summary derived from it. `DELETE
/memory/{device_id}` drops both.

## Endpoints behind the login

These are for the person, not the device: session cookie only, never the
device token.

| Method | Path | Does |
|---|---|---|
| `POST` | `/login` | verify `{"username", "password"}`, set the session cookie |
| `POST` | `/logout` | clear the session |
| `GET` | `/roles` | list every role, with `built_in` |
| `POST` | `/roles` | create one (409 on a duplicate name, 422 on a language with no voice, a mood that is not one of the nine faces, or a prompt over 2000 chars) |
| `PUT` | `/roles/{role_id}` | change only the fields sent; `prompt: null` reverts to built-in wording |
| `DELETE` | `/roles/{role_id}` | remove one (409 for a built-in; any surface pointing at it falls back to its default) |
| `GET` | `/settings/surfaces` | which role `esp32` and `web` are each using |
| `PUT` | `/settings/surfaces/{surface}` | switch one (404 for an unknown surface or role) |
| `GET` | `/settings/app` | `{"store_conversations", "retention_days"}` |
| `PUT` | `/settings/app` | change either |
| `DELETE` | `/conversations/{device_id}` | drop recorded transcripts, keeping facts |

Create the account with `uv run python scripts/create_account.py`, and set
`SESSION_SECRET_KEY` before deploying — left empty, the app signs cookies with
a random key generated per process, so every restart logs you out.
`SESSION_COOKIE_SECURE` defaults to `True`, which is right for Railway and
wrong for testing from a phone over plain `http://` on your LAN: the browser
discards the cookie silently, `/login` returns 200 and everything after it 401s.

## Layout

```
server/
  main.py              FastAPI app, /ws endpoint, auth
  session.py           per-connection state machine and pipeline
  context.py           prompt assembly under the token budget
  sentences.py         incremental sentence segmentation
  lang.py              uk/ru/en detection, voice selection
  audio.py             streaming MP3 -> 16 kHz PCM, WAV wrapper
  persona.py           system prompt, assembled from a role
  roles.py             roles and which one each surface uses; rows, not wording
  accounts.py          password accounts, separate from the facts/usage store
  costs.py             per-session usage accounting
  config.py            pydantic-settings
  providers/
    base.py            STTProvider, TTSProvider, LLMProvider, Embedder protocols
    groq_stt.py        whisper-large-v3-turbo
    groq_llm.py        chat completion, streaming
    edge_tts.py        edge-tts + inline decode to PCM
    embeddings.py      fastembed, local, no torch
    mock.py            offline stand-ins
    _retry.py          shared 429/5xx backoff
  memory/
    store.py           facts, usage, conversations, app settings; cosine similarity in Python
    summarise.py       end-of-session fact extraction
web/src/
  api.ts               fetch wrapper, session state, 401 handling
  useTurn.ts           the WebSocket, one turn at a time
  Chat.tsx             transcript and composer
  Inspector.tsx        what the turn actually did
  Settings.tsx         roles, surfaces, recording
  RoleEditor.tsx       one role's persona and knobs
  Memory.tsx           remembered facts, and deleting them
  Face.tsx             the device's eyes, played from face.c's own frames
  audio.ts             16 kHz capture, resampling, PCM playback
  traceStatus.ts       why a turn has no trace
firmware/host/
  face_export.c        writes web/public/face-frames.json from face.c
```

`context.py`, `sentences.py`, `lang.py`, `audio.py` and `_retry.py` are not in
the original spec's tree. Each was split out because it is pure, has one job,
and is far easier to test alone than through a WebSocket.

## Guardrails

- `max_tokens=150` on every LLM call.
- Prompt trimmed to `MAX_CONTEXT_TOKENS` before every request, oldest turns
  dropped first. History never starts with a dangling assistant reply.
- `SESSION_TIMEOUT_S` both ends a stuck button and caps one utterance at
  `timeout x 16000 x 2` bytes, so a jammed switch cannot grow the buffer.
- Exponential backoff with jitter on 408/429/5xx, honouring `Retry-After`.
- Tokens, audio seconds and TTS characters logged per session into SQLite.

## Two things the spec did not cover

**edge-tts does not emit PCM.** Its output format is the string literal
`audio-24khz-48kbitrate-mono-mp3` in the request payload — a constant, not a
parameter. So every chunk is decoded and resampled 24 kHz → 16 kHz on the way
through, incrementally, as bytes arrive. Buffering a whole sentence first would
spend the entire 300 ms TTS budget.

This uses **PyAV**, not an `ffmpeg` subprocess: it ships as a wheel with the
codecs bundled, so Railway needs no system packages and there is no child
process to supervise.

**The WebSocket had no auth.** Added as a bearer token, above.

## Deploy to Railway

```bash
railway up
```

Set `GROQ_API_KEY`, `DEVICE_TOKEN` and `SESSION_SECRET_KEY` as variables. Two
things to get right:

- **Attach a volume** at `/data` and set `DB_PATH=/data/voice.db`. SQLite is a
  file; without a volume every redeploy forgets everything.
- Memory sits far below the 512 MB ceiling — nothing is loaded locally, and the
  MP3 decoder works on a few KB at a time.

Before the web login can be used, set the one account's password once:
`uv run python scripts/create_account.py --username <you> --password <...>`.
Against the production volume, run it via
`railway run python scripts/create_account.py --username <you> --password <...>`.

## Deliberately out of scope

Wake-word, barge-in / AEC, Opus, local models. Each needs either an ESP32-S3 or
a GPU. Nothing above has to be rewritten to add them — only a firmware swap or
a config change.
