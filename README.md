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

247 tests, no network, no hardware, under ten seconds.

## Protocol

One persistent WebSocket. Binary frames are audio in both directions; text
frames are JSON control messages.

| Direction | Frame | Meaning |
|---|---|---|
| device → server | `{"type":"start"}` | button pressed, audio follows |
| device → server | binary | PCM chunks, sent *while* the button is held |
| device → server | `{"type":"end"}` | button released, utterance complete |
| server → device | `{"type":"state","value":"listening\|thinking\|speaking\|idle"}` | drives the LED |
| server → device | binary | PCM chunks of the reply |
| server → device | `{"type":"done"}` | playback finished, re-arm mute |

Audio is 16 kHz, 16-bit signed, mono, little-endian, raw PCM — in both
directions, so the firmware never converts anything.

Authentication is a shared secret in the handshake:

```
Authorization: Bearer <DEVICE_TOKEN>
```

Leaving `DEVICE_TOKEN` empty disables the check. Set it before the URL is
public: an open socket lets anyone drain the Groq free tier.

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
`/ws` — one shared secret, not per-device, same as the WebSocket:

| Method | Path | Does |
|---|---|---|
| `GET` | `/memory/{device_id}` | list everything remembered, with its source |
| `POST` | `/memory/{device_id}` | add a standing instruction (`{"text": "..."}`) |
| `DELETE` | `/memory/{device_id}/{fact_id}` | remove one entry |
| `DELETE` | `/memory/{device_id}` | wipe a device's memory entirely |

`RELEVANT_FACTS_LIMIT` (default 6) caps how many auto facts reach the prompt
per turn. `EMBEDDING_CACHE_DIR` should point at the same Railway volume the
database uses, or every redeploy re-downloads the model.

## Layout

```
server/
  main.py              FastAPI app, /ws endpoint, auth
  session.py           per-connection state machine and pipeline
  context.py           prompt assembly under the token budget
  sentences.py         incremental sentence segmentation
  lang.py              uk/ru/en detection, voice selection
  audio.py             streaming MP3 -> 16 kHz PCM, WAV wrapper
  persona.py           system prompt
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
    store.py           SQLAlchemy 2.0 async, SQLite, cosine similarity in Python
    summarise.py       end-of-session fact extraction
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

Set `GROQ_API_KEY` and `DEVICE_TOKEN` as variables. Two things to get right:

- **Attach a volume** at `/data` and set `DB_PATH=/data/voice.db`. SQLite is a
  file; without a volume every redeploy forgets everything.
- Memory sits far below the 512 MB ceiling — nothing is loaded locally, and the
  MP3 decoder works on a few KB at a time.

## Deliberately out of scope

Wake-word, barge-in / AEC, Opus, local models. Each needs either an ESP32-S3 or
a GPU. Nothing above has to be rewritten to add them — only a firmware swap or
a config change.
