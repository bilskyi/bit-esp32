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


class EdgeTTS:
    def __init__(self, communicate=edge_tts.Communicate, rate: int = 16000) -> None:
        self._communicate = communicate
        self._rate = rate

    async def synthesise(self, text: str, voice: str) -> AsyncIterator[bytes]:
        decoder = Mp3ToPcm(self._rate)
        stream = self._communicate(normalise(text), voice).stream()
        async for event in stream:
            if event.get("type") != "audio":
                continue  # WordBoundary and friends carry no audio
            pcm = decoder.feed(event["data"])
            if pcm:
                yield pcm
        tail = decoder.flush()
        if tail:
            yield tail
