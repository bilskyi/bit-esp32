# The twenty-second stalls — what the server log proved, and what it did not

`RESUME.md` item 0 carries the summary. This is the evidence behind it, kept
separately because the evidence expires: it all comes from `railway logs`,
which rolls off, and none of it can be re-gathered once it has.

**Status: half closed.** One fault is proven, fixed, deployed and flashed. It
accounts for **7 of the 16** twenty-second stalls in the window below. The
other **9 remain unexplained**, and they are the ones that match the original
`state 3` report. Do not read this document as a closed ticket.

---

## Provenance

| | |
|---|---|
| Source | `railway logs --lines 400 --json`, fetched 28 Aug ~15:20 local |
| Window | `2026-08-28 09:37:54.966` → `11:20:18.788`, 400 lines |
| Raw file | scratchpad `rlogs.json` — session-specific, **will expire**: `/private/tmp/claude-501/-Users-bilskyi-Documents-BIT/aca008c7-50d5-4583-8260-03ac5f2d2e4b/scratchpad/rlogs.json` |
| Device log | **not captured.** Nobody was at the board while this window was recorded, and that is the single biggest hole in what follows |

Railway's JSON carries per-line timestamps; the plain `railway logs` output
does not. Everything below is arithmetic on those timestamps. Nothing here is
reconstructed or inferred from memory — but note that `on_start` logs nothing
at INFO, so a `start` is only ever visible *indirectly*, through the watchdog
it arms.

---

## 1. The stalls are a timer, not a person

Gaps between consecutive server log lines, whole window:

| Class | n | Range |
|---|---|---|
| Ordinary (0.5–15 s) | 98 | median **3.48 s**, max **9.41 s** |
| The stalls (15–25 s) | **16** | **19.75 – 20.10 s** |

Zero overlap, and the 16 sit inside a 0.35 s band. A person waiting to press a
button does not do that. `STUCK_TIMEOUT_MS` is the only twenty-second constant
in the system, so the device's stuck-state watchdog is what ended each of these
— which means for twenty seconds the device was in a non-`ST_IDLE` state that
nothing was feeding.

### The sixteen, by what preceded them

| Preceded by | n |
|---|---|
| `reply cancelled by the device` | **7** — explained below, fixed |
| a completed `reply … B audio in … s` | **9** — **not explained** |

---

## 2. The proven fault: `done` is not scoped to a reply

### The evidence

```
11:15:30.476  stt 217 ms: Что такое звезда?
11:15:30.476  emotion curious (tagged)
11:15:32.913  reply cancelled by the device        <- the user interrupted
11:16:33.103  utterance timed out after 60.0s
11:16:33.103  utterance of 0.22 s is too short to be speech, not transcribing
```

The 60 s watchdog is armed by `on_start` and by nothing else. It fired at
11:16:33.103, so it was armed at **11:15:33.10 — 0.19 s after the cancel.**
The device therefore *did* start the next question. It then sent **0.22 s of
audio and no `end` at all**, and the user heard nothing for a full minute.

Three questions later, in the same session, the user asked the device:
`почему-то только что не отвечал`.

### The mechanism

`done` was never scoped to a reply. The server sends one from `_run_reply`'s
`finally` for **every** reply task — including a cancelled one, and including
an utterance it declined to answer. That `done` necessarily arrives *after* the
device has started recording again: measured at 0.3 s twice and 7 s twice
during the earlier interrupt work.

The device applied it to whatever it was doing at the time. `audio_out_task`'s
"reply produced no audio" rescue fired unconditionally and forced `ST_IDLE`
**out from under `ST_LISTENING`**. `end` is only ever sent from `ST_LISTENING`
(`voice_main.c`, the `!held && s_state == ST_LISTENING` branch), so the release
sent nothing and the question was recorded into a state that could not deliver
it.

0.22 s of audio is the sliver between the `start` and the stale `done` landing
one round trip later — the right order of magnitude for a link whose
power-save listen interval is ~100 ms.

This is the *same* late `done` the earlier interrupt work already proved
dangerous. That work moved the discard window off it and onto `speaking`; it
did not move `s_reply_finished`.

### The server made it invisible

`on_start` returned early for any state that was not `IDLE`, at `log.debug`.
So a device pressing again was ignored **without a line in the log**, the
abandoned fragment stayed in `_buf` ready to be prepended to the next question,
and the 60 s watchdog was never re-armed.

### What was changed

| File | Change |
|---|---|
| `firmware/main/voice_main.c` | the rescue only fires in `ST_THINKING`/`ST_SPEAKING`. The flag is cleared either way — leaving it raised just moves the damage to the next `THINKING`. Anything else logs `stale done ignored in state N` |
| `server/session.py` | a second `start` with no `end` starts a clean utterance: buffer cleared, watchdog re-armed, one INFO line with the bytes dropped |

Three tests, **all of which fail without the fix**:

- `test_session.py::test_a_second_start_while_listening_begins_a_fresh_utterance`
- `test_session.py::test_a_second_start_rearms_the_utterance_watchdog`
- `test_main.py::test_an_utterance_the_device_abandons_does_not_deafen_the_session`
  — replays cancel → start → go-deaf → start over a real socket

Verified: 223 server tests pass; host checks 500 / 50 / 14880, 0 failures;
`voice` builds with zero warnings, 48% of the app partition free. Server
deployed to Railway and confirmed serving; firmware flashed to the board.

---

## 3. What is NOT explained: nine stalls after a *completed* reply

This is the open half, and it is the half that matches the original report of
`no data from server for 20 s in state 3`.

Nine of the sixteen stalls follow a reply that finished normally. No cancel, no
stale `done` — the reply ran to completion and the server sent its ordinary
closing `done`. The device then sat for twenty seconds anyway.

The decisive number is *what the gap is anchored to*. The server logs `reply …`
when it has finished **sending**; the device finishes **playing** later, by
however much the audio outran the send:

| Gap (s) | Audio (s) | Sent in (s) | Playback ends, relative to the log |
|---|---|---|---|
| 19.99 | 3.41 | 2.0 | +1.41 |
| 20.00 | 8.09 | 4.2 | +3.89 |
| 20.10 | 10.23 | 6.3 | +3.93 |
| 20.00 | 10.11 | 6.2 | +3.91 |
| 19.98 | 5.80 | 3.6 | +2.20 |
| 20.00 | 7.27 | 3.3 | +3.97 |
| 20.00 | 9.55 | 8.1 | +1.45 |
| 20.00 | 6.88 | 2.9 | +3.98 |
| 20.00 | 9.83 | 5.9 | +3.93 |

**Playback end varies over 2.57 s. The gap varies over 0.12 s.** So the timer
is anchored to `done` — to the last byte the server sent — and not to when the
speaker went quiet. The device was still in a non-idle state twenty seconds
after the reply was fully delivered, and the watchdog is what released it.

That is `ST_SPEAKING` that never ended: exactly `state 3`.

### What was ruled out, on paper

Reasoning only — no device log, so none of this is measured:

- **The user simply waiting.** Ruled out by the 0.12 s band against a 2.57 s
  spread in when the device actually went quiet.
- **The stale `done` of a previous reply arriving mid-playback.** It would be
  consumed by the `playing && s_reply_finished` branch, which ends the reply
  *early* rather than never — the wrong symptom.
- **`s_reply_finished` consumed before playback starts.** Needs the play buffer
  empty with `playing` false, which the `got > 0` branch prevents unless
  everything is being discarded — and discarding only happens after a cancel,
  which these nine are not.

No path was found from the fixed fault to this one. Either there is a second
way to wedge `ST_SPEAKING`, or one of the three above is wrong.

### Where to look next

The server cannot see this. It has sent everything it owes and is idle; the
whole remaining question is what `audio_out_task` and the stuck timer are doing
on the device between `done` and the watchdog. **The next move is a serial
capture across a completed reply**, not more log arithmetic:

1. Flash `voice`, hold the monitor, have an ordinary conversation with **no
   interruptions at all** — the nine happen without one.
2. After each reply, watch for `idle: played … B` (healthy — the reply ended)
   versus its absence (the reply never ended, and the next line will be
   `no data from server for 20 s in state 3, forcing idle`).
3. If `starved` climbs after `done`, the buffer is being fed silence forever
   and `s_reply_finished` was consumed by someone — that identifies the branch.

The new `stale done ignored in state N` line discriminates the fixed fault from
this one: if the stalls persist *and* that line never appears, they are
entirely this second cause.

---

## What this document deliberately does not contain

The user's spoken questions appear in the raw log as STT transcripts. A handful
are quoted above only where they carry evidence — `почему-то только что не
отвечал` is the user reporting the bug in band, which is the point. The rest
are not reproduced: they date badly and none of them bear on the fault.
