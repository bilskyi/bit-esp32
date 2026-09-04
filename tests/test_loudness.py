"""The make-up gain and limiter that decide how loud a reply arrives.

The firmware's top volume step is unity - there is nothing left to turn up on
the board - so the level is set here, once, for every device and every client.
See docs/superpowers/specs/2026-09-04-tts-loudness-design.md.
"""

import math
import struct

from server.loudness import Loudness

RATE = 16000
CEILING_DBFS = -1.0
GAIN_DB = 8.0

# What -1 dBFS is in counts, computed here rather than imported: a ceiling the
# test takes from the code it is testing is not a ceiling.
CEILING = int(32767 * 10 ** (CEILING_DBFS / 20))
MAKEUP = 10 ** (GAIN_DB / 20)


def pcm_of(values: list[int]) -> bytes:
    return struct.pack(f"<{len(values)}h", *values)


def samples(pcm: bytes) -> list[int]:
    return list(struct.unpack(f"<{len(pcm) // 2}h", pcm))


def sine(n: int, hz: float = 220.0, amp: int = 1000) -> list[int]:
    return [int(amp * math.sin(2 * math.pi * hz * i / RATE)) for i in range(n)]


def through(limiter: Loudness, pcm: bytes, chunk: int | None = None) -> bytes:
    """Everything the limiter emits for `pcm`, in chunks of `chunk` bytes."""
    if chunk is None:
        return limiter.feed(pcm) + limiter.flush()
    out = bytearray()
    for at in range(0, len(pcm), chunk):
        out += limiter.feed(pcm[at : at + chunk])
    out += limiter.flush()
    return bytes(out)


def test_a_signal_the_limiter_never_engages_on_is_multiplied_by_the_make_up_gain():
    # 1000 counts times 2.51 is nowhere near the ceiling, so this is the
    # make-up gain on its own, with nothing taken back off it.
    values = sine(2000, amp=1000)
    out = samples(through(Loudness(GAIN_DB, CEILING_DBFS, RATE), pcm_of(values)))
    assert len(out) == len(values)
    worst = max(abs(out[i] - int(values[i] * MAKEUP)) for i in range(len(values)))
    assert worst <= 1, f"off by {worst} counts at the loudest divergence"


def test_no_sample_crosses_the_ceiling():
    # Times 2.51 this asks for 50238, well past full scale. Nothing may come
    # out above the ceiling, and nothing may come out flat-topped either -
    # which is what a clip would look like and what the look-ahead exists to
    # avoid.
    out = samples(through(Loudness(GAIN_DB, CEILING_DBFS, RATE), pcm_of(sine(4000, amp=20000))))
    assert max(abs(v) for v in out) <= CEILING


def test_a_full_scale_input_is_turned_down_rather_than_clipped():
    values = [32767 if i % 2 else -32768 for i in range(2000)]
    out = samples(through(Loudness(GAIN_DB, CEILING_DBFS, RATE), pcm_of(values)))
    assert max(abs(v) for v in out) <= CEILING


def test_a_sudden_loud_passage_does_not_step_the_gain():
    # A step in the coefficient is a step in the waveform, and a step is a
    # click. DC either side of the jump makes the applied gain readable per
    # sample: it is the output over the input.
    quiet, loud = 8000, 30000
    values = [quiet] * 640 + [loud] * 640
    out = samples(through(Loudness(GAIN_DB, CEILING_DBFS, RATE), pcm_of(values)))
    gains = [out[i] / values[i] for i in range(len(values))]

    # The gain has a whole window - 160 samples - to travel the distance
    # between the make-up gain and what the loud passage can take, so no two
    # neighbouring samples may differ by more than that share of it.
    span = MAKEUP - CEILING / loud
    allowed = span / (RATE // 100) * 1.05 + 1e-3
    worst = max(abs(gains[i + 1] - gains[i]) for i in range(len(gains) - 1))
    assert worst <= allowed, f"the gain jumped {worst:.4f} between samples, allowed {allowed:.4f}"

    # And it does get there: the loud passage is held at the ceiling, not
    # merely approached.
    assert max(abs(v) for v in out) <= CEILING
    assert max(abs(v) for v in out[-160:]) >= CEILING - 2


def test_chunk_boundaries_do_not_change_the_output():
    # The window is 160 samples and chunk sizes are whatever edge-tts happens
    # to yield, so none of these line up with it - including an odd byte
    # count, which splits a sample down the middle.
    pcm = pcm_of(sine(3000, amp=12000))
    whole = through(Loudness(GAIN_DB, CEILING_DBFS, RATE), pcm)
    for chunk in (1, 7, 320, 641, 4096):
        piecewise = through(Loudness(GAIN_DB, CEILING_DBFS, RATE), pcm, chunk=chunk)
        assert piecewise == whole, f"{chunk}-byte chunks gave a different result"


def test_flush_returns_what_the_look_ahead_was_holding():
    pcm = pcm_of(sine(1000, amp=5000))
    limiter = Loudness(GAIN_DB, CEILING_DBFS, RATE)
    fed = limiter.feed(pcm)
    assert len(fed) < len(pcm), "nothing was held back, so there is no look-ahead"
    assert len(fed) + len(limiter.flush()) == len(pcm)


def test_a_phrase_shorter_than_the_look_ahead_survives_it():
    # The last 20 ms of every phrase live in the look-ahead until flush(), and
    # a phrase can be shorter than that - "so?" through a fast voice.
    pcm = pcm_of(sine(64, amp=4000))
    limiter = Loudness(GAIN_DB, CEILING_DBFS, RATE)
    assert limiter.feed(pcm) == b""
    assert len(limiter.flush()) == len(pcm)


def test_silence_comes_out_silent():
    pcm = b"\x00\x00" * 1000
    assert through(Loudness(GAIN_DB, CEILING_DBFS, RATE), pcm) == pcm


def test_no_make_up_gain_leaves_a_quiet_signal_exactly_as_it_was():
    # tts_gain_db = 0 is how the whole stage is turned off, so it has to be a
    # true passthrough for anything already under the ceiling.
    pcm = pcm_of(sine(2000, amp=6000))
    assert through(Loudness(0.0, CEILING_DBFS, RATE), pcm) == pcm
