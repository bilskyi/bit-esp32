"""Make-up gain with a look-ahead limiter, applied to reply audio.

The firmware's top volume step is unity - `audio_out_task` copies the samples
through untouched at that setting - so there is nothing left in the device to
turn up. What arrives is simply quiet: measured on one of edge-tts's own
utterances, peaks at -4.3 dBFS over speech whose loud parts sit 18 dB below
full scale. This is where that is fixed, once, for every client.

The design and the reasoning behind every constant here:
docs/superpowers/specs/2026-09-04-tts-loudness-design.md
"""

import struct

FULL_SCALE = 32767

# The window the limiter measures and interpolates over, as a fraction of a
# second: 10 ms, so 160 samples at 16 kHz. Short enough that the gain reaches
# a peak before the peak reaches the speaker, long enough that measuring it
# costs one pass over 160 samples rather than a decision per sample.
WINDOWS_PER_SECOND = 100

# How fast the gain may climb back after a loud passage has pushed it down,
# in decibels per window - so the full 8 dB of make-up gain returns over
# 80 ms. Instant release would ride the gain up and down inside a syllable,
# which is the pumping that makes a limiter audible.
RELEASE_DB_PER_WINDOW = 1.0


class Loudness:
    """Streaming make-up gain and peak limiter over 16-bit mono PCM.

    Carries its window, its odd trailing byte and its gain between calls:
    chunk boundaries must not be audible, and a phrase arrives in whatever
    pieces the voice happens to yield.

    Runs two windows - 20 ms - behind its input, one being measured for the
    look-ahead and one being emitted. `flush()` drains both, and a phrase that
    never calls it loses its last 20 ms.
    """

    def __init__(
        self, gain_db: float = 8.0, ceiling_dbfs: float = -1.0, rate: int = 16000
    ) -> None:
        self._makeup = 10 ** (gain_db / 20)
        self._ceiling = int(FULL_SCALE * 10 ** (ceiling_dbfs / 20))
        self._window = max(1, rate // WINDOWS_PER_SECOND)
        self._release = 10 ** (RELEASE_DB_PER_WINDOW / 20)

        self._odd = b""             # half a sample, split across two chunks
        self._part: list[int] = []  # samples of a window that is not full yet
        self._queue: list[tuple[list[int], float]] = []  # windows and their targets
        # The gain in force at the last sample emitted. A phrase starts at the
        # make-up gain so that a quiet opening is not faded in; a loud one is
        # covered by the flat first window in _emit().
        self._gain = self._makeup
        self._started = False

    def feed(self, pcm: bytes) -> bytes:
        data = self._odd + pcm
        usable = len(data) // 2 * 2
        self._odd = data[usable:]
        if usable:
            self._part.extend(struct.unpack(f"<{usable // 2}h", data[:usable]))

        while len(self._part) >= self._window:
            window = self._part[: self._window]
            del self._part[: self._window]
            self._queue.append((window, self._target(window)))

        out = bytearray()
        # One window stays behind: emitting it needs the target of the window
        # after it, which is the whole point of the look-ahead.
        while len(self._queue) >= 2:
            out += self._emit(*self._queue.pop(0), self._queue[0][1])
        return bytes(out)

    def flush(self) -> bytes:
        """Drain the look-ahead at the end of a phrase."""
        if self._part:
            self._queue.append((self._part, self._target(self._part)))
            self._part = []
        out = bytearray()
        while self._queue:
            window, target = self._queue.pop(0)
            # The last window has nothing after it to look ahead to, so it
            # looks at itself and the expression below is unchanged.
            ahead = self._queue[0][1] if self._queue else target
            out += self._emit(window, target, ahead)
        return bytes(out)

    def _target(self, window: list[int]) -> float:
        """The most gain this window can take without crossing the ceiling."""
        peak = max(abs(v) for v in window)
        if peak == 0:
            return self._makeup  # silence times anything is silence
        return min(self._makeup, self._ceiling / peak)

    def _emit(self, window: list[int], target: float, ahead: float) -> bytes:
        # Both ends of the interpolation are at most `target`, so every sample
        # in this window is scaled by at most `target` - and `target` is by
        # definition the gain at which this window's peak lands exactly on the
        # ceiling. That is the whole ceiling argument; there is no clamp below
        # doing the work, and a clipped sample cannot reach the output.
        #
        # `ahead` is in the expression because a peak one window away has to
        # start pulling the gain down before it arrives: stepping the
        # coefficient at the sample where the peak starts is a discontinuity in
        # the waveform, and a discontinuity is a click.
        end = min(target, ahead, self._gain * self._release)
        # The first window of a phrase is applied flat. Interpolating into it
        # from the make-up gain would put a value above its own target at the
        # very front of the phrase - the one sample the argument above would
        # not cover.
        start = self._gain if self._started else end
        self._started = True

        n = len(window)
        last = n - 1 if n > 1 else 1
        out = [0] * n
        for i, v in enumerate(window):
            gain = start + (end - start) * i / last
            out[i] = int(v * gain)
        self._gain = end
        return struct.pack(f"<{n}h", *out)
