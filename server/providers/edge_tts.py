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


class EdgeTTS:
    def __init__(self, communicate=edge_tts.Communicate, rate: int = 16000) -> None:
        self._communicate = communicate
        self._rate = rate

    async def synthesise(self, text: str, voice: str) -> AsyncIterator[bytes]:
        decoder = Mp3ToPcm(self._rate)
        stream = self._communicate(text, voice).stream()
        async for event in stream:
            if event.get("type") != "audio":
                continue  # WordBoundary and friends carry no audio
            pcm = decoder.feed(event["data"])
            if pcm:
                yield pcm
        tail = decoder.flush()
        if tail:
            yield tail
