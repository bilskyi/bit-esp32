# Where this stopped

Last session ended mid-way through step 4. Everything below was measured on the
real board, not assumed.

## Working, verified on hardware

| Link | Evidence |
|---|---|
| Microphone → int16 | −27.9 dBFS, 0% clipping, 39% of energy in the speech band |
| Upload to server | 193536 B for 6.0 s held, **0 dropped, 0 failures** |
| Speech recognition | `Привет, как твои дела?`, `сколько будет 5 плюс 8` — four correct in a row, 314–569 ms |
| LLM | `openai/gpt-oss-120b`, answers |
| TTS → PCM | works; emulator gets a full spoken reply |
| Amplifier + mute pin | step 3 tone: clean, real silence in the gap |

## The one thing still open

**How the reply sounds.** The transport underneath it is now clean — a press
captured after the last fix logged:

    thinking: sent 84992 B (2.7 s), 0 dropped, 0 failures
    idle (play dropped 0 B)

Zero dropped bytes, so the whole reply reached I2S, and the server's fallback
voice kicked in on that same turn (`ru-RU-DmitryNeural` refused, the alternate
rendered it). What nobody has heard yet is the *result*: the last time this was
listened to, it was the old build that discarded most of the audio and came out
in fragments.

So: one button press, and listen. If it is still choppy the problem is no
longer the buffer, and the next suspects are the 32-bit slot format on TX and
the `frame[i*2 + 1] = 0` right-slot fill in `audio_out_task`.

## Also unexplained

The socket sometimes dies a fraction of a second into an upload:

    socket error: esp_tls=97 sock_errno=1070325472
    socket disconnected: sent 12288 B, 0 send failures, 0 dropped blocks

Not throughput (no failures, no drops), not memory (110+ KB free). It recovers
in about two seconds and the next press works. It has happened perhaps one turn
in three, and it is the last thing standing between this and something usable.
`sock_errno` is a garbage value, so the transport is not filling it in — worth
enabling verbose logging on `transport_ws` and catching one.

## Resuming

```bash
# 1. server (needs GROQ_API_KEY in .env)
uv run uvicorn server.main:app --host 0.0.0.0 --port 8000
#    add DEBUG_DUMP_PCM=1 to write each received utterance to /tmp

# 2. firmware
deactivate 2>/dev/null; unset VIRTUAL_ENV      # ESP-IDF refuses to share a venv
. ~/esp/esp-idf/export.sh                      # source it, never pipe it
cd firmware && idf.py -DSKETCH=voice -p /dev/cu.usbmodem1101 flash monitor
```

`SERVER_URI` in `firmware/main/secrets.h` is pinned to `192.168.31.214:8000`.
**That address will change** when the laptop rejoins the network — check with
`ipconfig getifaddr en0` and edit `secrets.h` before wondering why the socket
will not open.

## Traps already paid for

- **A press with no socket is silently ignored.** `net_task` requires
  `esp_websocket_client_is_connected()`, so after a server restart the button
  looks broken until the device reconnects. Worth logging; not yet done.
- **Restarting the server drops the device.** It does reconnect on its own,
  within a couple of seconds — earlier sessions blamed the client for this
  wrongly, the server had simply been killed.
- **ESP-IDF and this project's `uv` venv collide.** The installer skips its own
  Python environment if another venv is active, then every `idf.py` fails with
  *Cannot import module esp_idf_monitor*. Fix:
  `unset VIRTUAL_ENV && python3 $IDF_PATH/tools/idf_tools.py install-python-env`
- **`idf.py … | tail` hides failures** — the pipeline exit status is `tail`'s.
  A failed build once looked like a clean pass this way.
- **Individual edge-tts voices fail on individual phrases.** Not the language,
  not the code: `ru-RU-DmitryNeural` refused "Привет, Катерин!" while
  `ru-RU-SvetlanaNeural` said it happily. Hence the fallback voice.

## Then

- Latency is **1.7–2.0 s** to first audio against the 1.5 s target, measured
  with the emulator. Nothing has been optimised yet.
- Binary is 933 KB of a 1 MB partition. `wss://` for Railway will need a
  custom partition table; the chip has 2 MB.
- `capture` still uses mono slot mode while `voice` uses stereo. Both work;
  they should be unified before this is called finished.
- The verbose transport logging in `voice_main.c` should come out once
  playback is confirmed.
