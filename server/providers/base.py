"""Provider interfaces.

Free tiers change and the mixed-language STT choice is unresolved, so every
provider sits behind a Protocol with two or three methods. Swapping one is a
new file plus a line in the factory, never a change to the session logic.
"""

from dataclasses import dataclass
from typing import AsyncIterator, Protocol


@dataclass(slots=True)
class Transcript:
    text: str
    language: str | None = None
    seconds: float = 0.0


class STTProvider(Protocol):
    async def transcribe(self, pcm: bytes, sample_rate: int) -> Transcript: ...


class LLMProvider(Protocol):
    def stream(self, messages: list[dict], max_tokens: int) -> AsyncIterator[str]:
        """Yield reply text deltas as they are generated."""
        ...

    async def complete(self, messages: list[dict], max_tokens: int) -> str:
        """Return a whole reply. Used for memory extraction, not conversation."""
        ...


class TTSProvider(Protocol):
    def synthesise(self, text: str, voice: str) -> AsyncIterator[bytes]:
        """Yield 16 kHz 16-bit mono little-endian PCM for `text`."""
        ...


class Embedder(Protocol):
    """Two methods, not one: multilingual-e5-family models are asymmetric —
    a stored fact and a live question are embedded with different prefixes,
    so mixing them up quietly makes retrieval worse rather than raising.
    """

    async def embed_documents(self, texts: list[str]) -> list[list[float]]: ...

    async def embed_query(self, text: str) -> list[float]: ...
