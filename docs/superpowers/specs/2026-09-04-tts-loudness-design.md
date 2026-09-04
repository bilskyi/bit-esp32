# Making the reply as loud as the speaker can be

The volume page's top step is unity - `VOLUME_GAIN[5] = 0x7fff`, and
`audio_out_task` copies the samples through untouched at that setting - so
"maximum volume is very quiet" is not a bug in the menu. There is nothing left
in the firmware to turn up. The quiet is in what arrives.

Measured on `reply.wav`, one of edge-tts's own utterances at 16 kHz:

| | |
|---|---|
| Peak | −4.3 dBFS |
| RMS, whole phrase | −21.9 dBFS |
| RMS, loudest tenth of the blocks | −17.8 dBFS |

Four decibels of headroom nobody is using, over speech whose loud parts sit
18 dB below full scale. `server/audio.py` decodes the MP3, resamples it and
hands it on; no stage between the voice and the amplifier ever looks at the
level. A 3 W class-D amp into a speaker this small has no reserve to spare
for that.

So the level is fixed where the samples are made, once, for every device and
every client - not per device, and not by adding steps above unity to the
volume table, which would only clip the same signal on the board instead.

## What it does

A make-up gain, with a limiter to catch what the gain would have pushed past
full scale.

`tts_gain_db` (default **8.0**) multiplies everything. `tts_ceiling_dbfs`
(default **−1.0**) is the level no sample may exceed afterwards. Both are
`Settings` fields, so a device that turns out to distort - or a room that
wants more - is an `.env` change and a restart, not a reflash.

Eight decibels is chosen against the numbers above: it puts the loudest tenth
of a phrase near −10 dBFS and asks the limiter for about 4.7 dB of reduction
at the peaks, which speech carries without audible pumping. It is the setting
to revisit first if a bench session says the speaker distorts before it is
loud enough.

## How the limiter works

Audio is not available all at once - `EdgeTTS.synthesise()` yields while the
voice is still speaking, and the reply is paced against playback - so peak
normalising a whole phrase is not on the table. The limiter is streaming, and
looks ahead by a fixed amount instead.

The stream is cut into **windows of 160 samples**, 10 ms at 16 kHz. Each
window has a target multiplier: `min(makeup, ceiling / peak_of_window)` - the
most gain that window can take without a sample crossing the ceiling.

The multiplier actually applied to window *n* is

```
g[n] = min(target[n], target[n+1], g[n-1] * release_step)
```

and within the window it interpolates linearly from `g[n-1]` to `g[n]`.

That gives three properties, and each one is why a term is there:

- **The ceiling holds.** Both ends of the interpolation across window *n* are
  at most `target[n]`, so every sample in it is at most `target[n]` - and
  `target[n]` is by definition the gain at which that window's peak lands
  exactly on the ceiling. No clipping, and no `min`-with-32767 hiding a
  clipped sample as a flat top.
- **The gain comes down before the loud part, not on it.** `target[n+1]` is
  in the expression for `g[n]`, so a peak one window away is already pulling
  the gain down. Stepping the coefficient at the sample where the peak starts
  is a discontinuity in the waveform, and a discontinuity is a click - the
  same reason `play_beep()` ramps its ends and `audio_out_task` ramps a volume
  change across a block.
- **The gain goes back up slowly.** `release_step` is 1.0 dB per window, so
  recovering the full 8 dB takes 80 ms. Instant release would ride the gain up
  and down inside a syllable, which is the pumping that makes a limiter
  audible.

Two edges fall out of the formula rather than needing rules of their own. A
silent window has no peak to divide by and takes the full make-up gain, which
is what it would have taken anyway - silence times anything is silence. And
the first window of a phrase is applied flat, at `g[0]`, with no interpolation
into it: interpolating from the make-up gain would put a value above
`target[0]` at the very front of the phrase, which is the one sample the
ceiling argument above would not cover.

Looking one window ahead means holding one window back, so the limiter runs
**two windows, 20 ms, behind its input** - one being measured, one being
emitted. `flush()` drains both at the end of a phrase. Twenty milliseconds
against a first-chunk budget of seven seconds is not a latency question, but
losing the last 20 ms of every phrase to a missing `flush()` would clip the
final consonant of every reply, which is.

## Where it lives

`server/loudness.py`, one class, `feed(pcm) -> pcm` and `flush() -> pcm` -
the shape `Mp3ToPcm` and `AdpcmEncoder` already have, for the same reason:
chunk boundaries must not be audible, so the state between them belongs to an
object and not to a caller.

It is wired into `Session._speak`'s `render()`, per phrase, in one specific
place in that loop:

```python
sent_bytes += len(pcm_chunk)          # unchanged: pacing counts audio
pcm_chunk = limiter.feed(pcm_chunk)   # new
if encoder is not None: ...           # unchanged: ADPCM after the limiter
```

**Before the ADPCM encoder**, because clipped samples compressed into 4-bit
nibbles are clipped for good, and because the encoder's predictor would chase
the step. **After `sent_bytes`**, because that counter paces the reply against
what the device can have played by now; feeding it the limiter's output would
make the pacing lag by the look-ahead and stall on the empty return the
limiter gives for a chunk shorter than two windows. **After the first-chunk
timeout is rescheduled**, for that same empty return: `started` has to be set
by audio arriving from the voice, not by audio leaving the limiter.

Every client gets it, not only the board. A browser has its own volume
control, but a reply normalised to −1 dBFS is the right thing to hand it too,
and one path is one thing to reason about.

## What is testable without hardware

`server/loudness.py` is a pure function of its input with carried state, so
all of it:

- no sample in the output exceeds the ceiling, on a signal built to have
  peaks the make-up gain would push past it;
- a signal quiet enough that the limiter never engages comes out multiplied
  by exactly `tts_gain_db`, within a count of rounding;
- the same input split into arbitrary chunk sizes gives byte-identical
  output - the streaming property, and the one a window-based design can
  plausibly get wrong;
- `feed()` plus `flush()` returns exactly as many samples as went in;
- the applied gain moves no faster between adjacent samples than the window
  interpolation allows - the click test, stated as a bound rather than by ear.

Session-level: a reply through a fake TTS comes out louder than it went in,
which is what proves the limiter is actually in the path and on the right
side of the encoder.

**What only hardware can answer:** whether 8 dB is where the speaker starts
to distort, and whether the top volume step now feels like a maximum. One
bench session, the same one the volume table was always going to need.

## Deliberately out of scope

- **Steps above unity in the firmware volume table.** The signal is the same
  signal; amplifying it on the board clips it there instead, without the
  look-ahead to stop it. Rejected in favour of fixing the level once, at the
  source.
- **The amplifier's own gain pin.** MAX98357A runs at its default 9 dB with
  `GAIN` left floating; 100 kΩ to Vdd would make it 15 dB. Six clean decibels,
  and no code can reach it - a soldering iron and a wiring change, so it is a
  bench decision, not this change.
- **Per-device or per-conversation loudness.** The knob on the board is the
  volume page. This sets what "loud" means for all of them.
- **Loudness normalisation to a standard (LUFS).** Peak plus a fixed make-up
  gain answers the question asked here. Matching a broadcast target needs a
  measurement window longer than the phrases being spoken.
