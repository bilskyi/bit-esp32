# Where this stopped

The device works end to end: button, microphone, WiFi, Whisper, the LLM,
speech synthesis, speaker. Replies have been heard out loud. What is left is
that it only works about half the time, and stutters when it does.

## Measured, not assumed

| | |
|---|---|
| Speech recognition | correct every single time - `Что такое GitHub?`, `Сколько будет 2 плюс 3?`, 300-500 ms |
| Upload | median send **3 ms** per KB, 0 failures |
| Reply delivery | `dropped 0 B` - the whole reply reaches the device |
| Playback | **starved 906 times** during a 3 s reply, which took 20.8 s to play |
| Sends | normally 3 ms, but regularly stall **200-1600 ms** |

## The remaining problem, and the prime suspect

Playback stutters because the buffer keeps running dry, and it runs dry
because the link stalls for hundreds of milliseconds at a time.

The boot log says why, most likely:

    wifi: dp: 1, bi: 102400, li: 3, scale listen interval ... 307200 us

WiFi power save parks the radio for **307 ms** between beacons. That is the
same magnitude as the stalls. `WIFI_PS_NONE` was tried once and appeared to
destabilise things, but that was before the keepalive bug was found - and the
keepalive was what actually killed those connections. It deserves a second try
now that the real cause is fixed.

**Start here:**

1. `esp_wifi_set_ps(WIFI_PS_NONE)` in `wifi_start()`, flash, press a few times.
   Watch whether `send of 1024 B took ...` warnings disappear and `starved`
   drops toward zero.
2. If the stalls persist, buffer around them instead: `PLAY_BUFFER_BYTES` to
   49152 (1.5 s), `PREBUFFER_BYTES` to 24000 (0.75 s), and raise
   `playback_lead_s` to match. This costs RAM - the spec budgets ~40 KB for
   buffers and this would take it to ~64 KB, against ~110 KB of free heap.
3. Both, if neither alone is enough.

## Fixed today, with the evidence

- **A bounded send timeout was tearing down connections.** The value goes
  straight to the transport's poll_write, and an expired poll makes
  `esp_transport_write()` return 0, which the client treats as fatal. Sampling
  the button in its own task removed the need for the bound.
- **The keepalive was killing healthy connections.** `Could not lock ws-client
  within 2000 timeout for PONG` - a stalled send held the lock past the PONG
  budget. Lock budget raised to 15 s, missed PONGs no longer disconnect.
- **edge-tts returns silence for typographic characters.** U+202F around an em
  dash, a non-breaking hyphen in "из-за". The model emits them constantly.
  Yesterday's per-voice fallback appeared to help and did not: every voice
  failed on the same text. Normalising is the real fix.
- **The stuck-state timer cut off replies that were playing.** It counted from
  the press, and a long reply legitimately takes twenty seconds. It now
  measures silence from the server.
- **Debug logging broke what it measured.** Transport logging at DEBUG blocked
  the socket task through the synchronous USB console.

## Four wrong theories, for the record

WiFi power save, the shared send/receive lock, unpaced audio, buffer overflow.
Each was reasoned from how the system ought to behave, each produced a change
that helped slightly, none was the cause. What actually found it was printing
the client's own error text and timing every send - two small measurements
that should have gone in two days earlier.

## Resuming

```bash
uv run uvicorn server.main:app --host 0.0.0.0 --port 8000   # DEBUG_DUMP_PCM=1 optional

deactivate 2>/dev/null; unset VIRTUAL_ENV      # ESP-IDF refuses to share a venv
. ~/esp/esp-idf/export.sh                      # source it, never pipe it
cd firmware && idf.py -DSKETCH=voice -p /dev/cu.usbmodem1101 flash monitor
```

Check `ipconfig getifaddr en0` first: `SERVER_URI` in `firmware/main/secrets.h`
is pinned to a literal address and will not follow the laptop onto a new
network.

`idf.py -DSKETCH=mute` silences the amplifier if it is ever left making noise.

## Traps already paid for

- **A press with no socket is silently ignored**, so after a disconnect the
  button looks broken for the couple of seconds it takes to reconnect. Still
  worth surfacing on the device somehow.
- **Only one process may hold the serial port.** Two monitors produce garbled
  output and a flash will fail with `exit=2`.
- **ESP-IDF and this project's `uv` venv collide**; if `idf.py` reports
  *Cannot import module esp_idf_monitor*, run
  `unset VIRTUAL_ENV && python3 $IDF_PATH/tools/idf_tools.py install-python-env`
- **`idf.py ... | tail` hides failures** - the exit status is `tail`'s.

## Then

- Latency to first audio is 1.7-2.0 s against the 1.5 s target, and untuned.
- The binary is 933 KB of a 1 MB partition; `wss://` for Railway will need a
  custom partition table. The chip has 2 MB.
- `capture` uses mono slot mode while `voice` uses stereo. Both work, but they
  should be unified.
- Instrumentation (`send of ... took`, `starved`, reply logging) should come
  out once this is stable.
