"""Groq Whisper transcription.

No `language` parameter is ever sent: the user code-switches between Ukrainian,
Russian and English, and push-to-talk gives clean utterance boundaries, which is
the case Whisper's auto-detection handles best.
"""

import asyncio
import logging
from typing import Callable

import httpx

from server.audio import pcm_to_wav
from server.providers._retry import with_retries
from server.providers.base import Transcript

log = logging.getLogger(__name__)

BASE_URL = "https://api.groq.com/openai/v1/audio/transcriptions"


class GroqSTT:
    def __init__(
        self,
        api_key: str,
        model: str,
        client: httpx.AsyncClient | None = None,
        max_retries: int = 4,
        sleep: Callable = asyncio.sleep,
        timeout: float = 30.0,
    ) -> None:
        self._key = api_key
        self._model = model
        self._client = client or httpx.AsyncClient(timeout=timeout)
        self._max_retries = max_retries
        self._sleep = sleep

    async def transcribe(self, pcm: bytes, sample_rate: int) -> Transcript:
        wav = pcm_to_wav(pcm, sample_rate)

        async def send():
            return await self._client.post(
                BASE_URL,
                headers={"Authorization": f"Bearer {self._key}"},
                files={"file": ("utterance.wav", wav, "audio/wav")},
                data={"model": self._model, "response_format": "json"},
            )

        response = await with_retries(send, self._max_retries, self._sleep)
        body = response.json()
        return Transcript(
            text=(body.get("text") or "").strip(),
            language=body.get("language"),
            seconds=len(pcm) / 2 / sample_rate,
        )
