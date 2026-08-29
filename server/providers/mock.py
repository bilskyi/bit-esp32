"""Offline stand-ins for the Groq providers.

Enabled with PROVIDER_MODE=mock. TTS stays real, so the audio path — edge-tts
MP3, decode, resample, framing over the socket — is exercised end to end
without an API key. Never enable this in production; it does not transcribe.
"""

import asyncio
import hashlib
import json
from typing import AsyncIterator

from server.providers.base import Transcript

CANNED_REPLY = "Все добре, дякую. А в тебе як справи?"


class MockSTT:
    def __init__(self, text: str = "Привіт, як справи?") -> None:
        self._text = text

    async def transcribe(self, pcm: bytes, sample_rate: int) -> Transcript:
        return Transcript(text=self._text, language="uk", seconds=len(pcm) / 2 / sample_rate)


class MockLLM:
    def __init__(self, reply: str = CANNED_REPLY, echo: bool = False, delay: float = 0.02) -> None:
        self._reply = reply
        self._echo = echo
        self._delay = delay

    def _text_for(self, messages: list[dict]) -> str:
        if not self._echo:
            return self._reply
        last = next((m["content"] for m in reversed(messages) if m["role"] == "user"), "")
        return f"Ти сказав: {last}. {self._reply}"

    async def stream(self, messages: list[dict], max_tokens: int) -> AsyncIterator[str]:
        for word in self._text_for(messages).split(" "):
            await asyncio.sleep(self._delay)  # imitate generation pacing
            yield word + " "

    async def complete(self, messages: list[dict], max_tokens: int) -> str:
        # The only caller is fact extraction, which wants a JSON array.
        return json.dumps(["Uses a mock LLM for local testing"], ensure_ascii=False)


class MockEmbedder:
    """No model, no download, no network - a hash is not a real embedding,
    but PROVIDER_MODE=mock only has to exercise the plumbing.

    hash() is randomised per process (PYTHONHASHSEED), so it is unusable
    here; md5 is not.
    """

    _DIMS = 8

    async def embed_documents(self, texts: list[str]) -> list[list[float]]:
        return [self._vec(t) for t in texts]

    async def embed_query(self, text: str) -> list[float]:
        return self._vec(text)

    @classmethod
    def _vec(cls, text: str) -> list[float]:
        digest = hashlib.md5(text.encode("utf-8")).digest()
        return [b / 255.0 for b in digest[: cls._DIMS]]
