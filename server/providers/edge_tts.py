"""edge-tts synthesis, converted to device-ready PCM on the fly.

edge-tts always returns 24 kHz mono MP3 (the output format is a literal in its
request payload, not a parameter), so every chunk is decoded and resampled to
16 kHz here. Doing it incrementally is what keeps first-audio inside the
latency budget.
"""

import logging
from typing import AsyncIterator

import edge_tts

from server.audio import Mp3ToPcm

log = logging.getLogger(__name__)


# edge-tts returns no audio at all for text containing certain typographic
# characters, and gives no indication why. Observed with a narrow no-break
# space (U+202F) and a non-breaking hyphen (U+2011), both of which the LLM
# produces routinely: "Турбулентность\u202f—\u202fэто..." rendered as silence
# on every voice tried. Normalising them away is cheaper than discovering the
# next one in production.
_TTS_SUBSTITUTIONS = {
    "\u00a0": " ",  # no-break space
    "\u202f": " ",  # narrow no-break space
    "\u2009": " ",  # thin space
    "\u200b": "",   # zero-width space
    "\u200c": "",   # zero-width non-joiner
    "\u200d": "",   # zero-width joiner
    "\ufeff": "",   # byte order mark
    "\u2011": "-",  # non-breaking hyphen
    "\u2010": "-",  # hyphen
    "\u2012": "-",  # figure dash
    "\u2015": "-",  # horizontal bar
    "\u2043": "-",  # hyphen bullet
}


def normalise(text: str) -> str:
    """Strip characters that make the service return silence."""
    for bad, good in _TTS_SUBSTITUTIONS.items():
        text = text.replace(bad, good)
    return " ".join(text.split())


# Keep a little room after the last audible sample so speech is never clipped
# and consecutive sentences do not run together.
_TAIL_MARGIN_S = 0.08
_SILENCE_LEVEL = 200  # int16; the pad is true digital silence, not room tone


def _trim_trailing_silence(pcm: bytes, rate: int) -> bytes:
    """Drop the silent padding at the end of a sentence."""
    if not pcm:
        return pcm
    import array

    samples = array.array("h")
    samples.frombytes(pcm[: len(pcm) // 2 * 2])
    last = -1
    for i in range(len(samples) - 1, -1, -1):
        if abs(samples[i]) > _SILENCE_LEVEL:
            last = i
            break
    if last < 0:
        return b""  # the whole holdback was padding
    keep = min(len(samples), last + 1 + int(rate * _TAIL_MARGIN_S))
    return samples[:keep].tobytes()


class EdgeTTS:
    def __init__(
        self,
        communicate=edge_tts.Communicate,
        rate: int = 16000,
        speed: str = "+25%",
    ) -> None:
        self._communicate = communicate
        self._rate = rate
        # These voices speak slowly by default: "Привет! Всё хорошо, спасибо.
        # Чем могу помочь?" - 45 characters - takes 6.3 seconds, about half
        # conversational pace. That is not a decoding artefact; the MP3 really
        # is that long. Speeding it up shortens every reply, which matters
        # twice over on a congested link, where a 13-second answer is 443 KB
        # that has to survive stalls of a second or more.
        self._speed = speed

    async def synthesise(self, text: str, voice: str) -> AsyncIterator[bytes]:
        decoder = Mp3ToPcm(self._rate)
        stream = self._communicate(normalise(text), voice, rate=self._speed).stream()

        # The service pads roughly 0.7 s of silence onto the end of every
        # sentence. Sent verbatim that is ~22 KB of nothing per sentence, and
        # on a congested link nothing still has to survive the stalls. Holding
        # the last half second back lets the tail be trimmed without giving up
        # streaming: everything before the holdback goes out immediately.
        hold = bytearray()
        holdback = int(self._rate * 0.5) * 2

        async for event in stream:
            if event.get("type") != "audio":
                continue  # WordBoundary and friends carry no audio
            pcm = decoder.feed(event["data"])
            if not pcm:
                continue
            hold += pcm
            if len(hold) > holdback:
                out = bytes(hold[:-holdback])
                del hold[:-holdback]
                yield out

        hold += decoder.flush()
        trimmed = _trim_trailing_silence(bytes(hold), self._rate)
        if trimmed:
            yield trimmed
