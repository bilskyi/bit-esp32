# Where this stands

The device works. Button, microphone, WiFi, Whisper, the LLM, speech synthesis,
speaker - a real conversation, switching between Russian, Ukrainian and English
mid-session, with memory of the user's name carrying across days.

## Measured on hardware, most recent session

Nine consecutive turns:

    played 3.9 s in 4.4 s, starved 0
    played 6.6 s in 6.9 s, starved 0
    played 5.5 s in 5.7 s, starved 0
    ... nine of nine, zero underruns, zero drops

| | |
|---|---|
| Speech recognition | correct essentially every time, 200-570 ms |
| Upload | 3-8 ms per KB typical, 0 dropped |
| Playback | real time, 0 underruns across nine turns |
| First audio | median 1.5 s against the spec's 1.5 s target |
| Memory | 6 facts stored, injected across sessions |
| Cost logging | 41 sessions, 237-488 prompt tokens against a 2000 budget |

For contrast, earlier the same day: one reply played 7.4 s of audio over 17.4 s
with 308 underruns.

## The blocking issue is radio, and it is physical

Measured with everything else working:

    laptop  -> router:   0% loss,  3.6 ms
    laptop  -> Railway:  0% loss,   47 ms, healthz in 300 ms
    laptop  -> device:  67% loss, 2200 ms

The laptop is fine on the same network and Railway is fine from it. Only the
device's own link is broken - and the device reports -49 to -63 dBm while it
happens, so it hears the access point perfectly well. Strong signal with heavy
loss is interference, not distance.

The board sits in the laptop's USB port. USB 3.0 is a well documented broadband
noise source at exactly 2.4 GHz, centimetres from a PCB antenna. Worth trying,
in order: a USB extension cable to move the board away, a USB 2.0 port or a
separate supply, keeping it off metal, and changing the router from channel 2
to 11.

This matters more since the move to Railway. A TLS handshake needs several
round trips in succession; on a link losing two thirds of its packets it falls
apart where plain TCP still scraped through. The same board managed nine clean
turns with 3-8 ms sends earlier the same day, so the hardware is capable and
the environment changed.

`firmware/main/secrets.h` can be pointed back at the LAN server for a session
that has to work today.

## What is left

- **A socket drop roughly once per ten turns.** A send stalls past the timeout
  and the client tears the connection down; it recovers in about three seconds,
  but the press during that window is lost. The timeout was just raised from
  5 s to 12 s because the stall that triggered the last one measured 5006 ms.
  Unverified.
- **No feedback on the device.** Pressing while it is still speaking logs
  `press ignored: still in state 3` and does nothing visible, which reads as
  "it broke". Most boards have an LED on GPIO 8; the spec always intended state
  to drive one.
- **Railway.** The server still runs on the laptop. Deploying needs `wss://`,
  which needs a custom partition table - the binary is 933 KB of a 1 MB
  partition and the chip has 2 MB - plus DEVICE_TOKEN and a volume for SQLite.
- **Instrumentation should come out** once this is stable: `send of ... took`,
  `starved`, `rssi`, the reply logging.
- **`capture` uses mono slot mode, `voice` uses stereo.** Both work; they
  should agree.

## What actually caused the trouble, in order of how long it hid

1. **The model was thinking instead of answering.** gpt-oss reasons before it
   replies and the reasoning is billed against the same budget. At
   max_tokens=150 an open question produced 511 characters of reasoning and 12
   of answer. `reasoning_effort=low` fixed every "long questions are ignored"
   symptom. Days were spent looking at the radio for this.
2. **Power save parked the radio for 307 ms at a time.** Pings lost nothing but
   ranged 3 ms to 2075 ms. Waking every beacon took the average to 108 ms.
   Turning power save *off* is worse and was tried twice - a 1 KB send took
   32 seconds and the radio then failed to associate at all.
3. **Raw PCM did not fit the link.** ADPCM at four bits per sample took the
   uplink from 32 KB/s to 8 KB/s, and the reply from 340 KB to 86 KB.
4. **Pacing starved the buffer it was meant to protect.** Holding the server
   1.2 s ahead left five of the device's six buffered seconds unused.
5. **A failing voice was indistinguishable from a slow one.** edge-tts returns
   nothing when it fails, and the full budget was spent waiting before trying
   the alternate. Split into a first-byte budget and an overall one.
6. **Debug logging broke what it measured.** Transport logging at DEBUG blocked
   the socket task through the synchronous USB console.

## Resuming

```bash
uv run uvicorn server.main:app --host 0.0.0.0 --port 8000   # DEBUG_DUMP_PCM=1 optional

deactivate 2>/dev/null; unset VIRTUAL_ENV      # ESP-IDF refuses to share a venv
. ~/esp/esp-idf/export.sh                      # source it, never pipe it
cd firmware && idf.py -DSKETCH=voice -p /dev/cu.usbmodem1101 flash monitor
```

Check `ipconfig getifaddr en0` first: `SERVER_URI` in `firmware/main/secrets.h`
is a literal address and will not follow the laptop onto a new network.

`idf.py -DSKETCH=mute` silences the amplifier if it is left making noise.

## Traps already paid for

- **Restarting the server strands the device.** It reconnects on its own, but
  the keepalive can take most of a minute to notice, and every press until then
  is swallowed. Reset the board after a server restart rather than wondering.
- **Only one process may hold the serial port.** Two monitors produce garbled
  output and a flash fails with `exit=2`.
- **A backgrounded monitor started with `&` inside a foreground command dies
  with its parent.** Several test windows produced empty logs this way and were
  misread as "no presses".
- **ESP-IDF and this project's `uv` venv collide.** If `idf.py` reports
  *Cannot import module esp_idf_monitor*:
  `unset VIRTUAL_ENV && python3 $IDF_PATH/tools/idf_tools.py install-python-env`
- **`idf.py ... | tail` hides failures** - the exit status is `tail`'s.
- **Watch the disk.** A build failed on `No space left on device` with 120 MB
  free on the volume.

## Wrong theories, for the record

WiFi power save (twice), the shared send/receive lock, unpaced audio, buffer
overflow, a per-voice TTS fault, heat damage to the microphone. Each was
reasoned from how the system ought to behave and each produced a change that
helped a little or not at all. What found the real causes every time was
printing a number: the client's own error text, the duration of each send, the
size of the reply, the distribution of first-chunk latency.
