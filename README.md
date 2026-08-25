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

118 tests, no network, no hardware, under a second.

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
    base.py            STTProvider, TTSProvider, LLMProvider protocols
    groq_stt.py        whisper-large-v3-turbo
    groq_llm.py        chat completion, streaming
    edge_tts.py        edge-tts + inline decode to PCM
    mock.py            offline stand-ins
    _retry.py          shared 429/5xx backoff
  memory/
    store.py           SQLAlchemy 2.0 async, SQLite
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
