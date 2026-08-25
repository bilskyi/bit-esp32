"""Incremental sentence segmentation for streamed LLM output.

The LLM arrives token by token. TTS wants whole sentences. This splitter sits
between them and emits a sentence the moment it is provably complete, so the
first sentence can be spoken while the rest is still being generated.
"""

TERMINATORS = ".!?…"
# Characters that may trail a terminator and still belong to the same sentence.
CLOSERS = "\"')]»”’"


class SentenceSplitter:
    """Accumulates streamed text and emits complete sentences."""

    def __init__(self) -> None:
        self._buf = ""

    def feed(self, text: str) -> list[str]:
        """Add streamed text; return any sentences that are now complete."""
        self._buf += text
        out: list[str] = []
        while (cut := self._boundary()) is not None:
            out.append(self._buf[:cut].strip())
            self._buf = self._buf[cut:].lstrip()
        return [s for s in out if s]

    def flush(self) -> list[str]:
        """Return whatever is left, terminated or not. Call at end of stream."""
        rest = self._buf.strip()
        self._buf = ""
        return [rest] if rest else []

    def _boundary(self) -> int | None:
        """Index just past the end of the first complete sentence, if any.

        A boundary requires whitespace after the terminator. That single rule
        also keeps decimals ("3.5") and most abbreviations intact, and it means
        a trailing terminator waits for more input rather than guessing.
        """
        i = 0
        while i < len(self._buf):
            if self._buf[i] not in TERMINATORS:
                i += 1
                continue
            j = i
            while j < len(self._buf) and self._buf[j] in TERMINATORS:
                j += 1
            while j < len(self._buf) and self._buf[j] in CLOSERS:
                j += 1
            if j >= len(self._buf):
                return None  # need more input to tell
            if self._buf[j].isspace():
                return j
            i = j
        return None
