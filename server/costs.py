"""Per-session usage accounting.

Nothing here bills anything; it records what a session consumed so the free
tiers can be watched from SQLite instead of guessed at.
"""

from dataclasses import dataclass


@dataclass
class Usage:
    turns: int = 0
    audio_seconds: float = 0.0
    prompt_tokens: int = 0
    completion_tokens: int = 0
    tts_chars: int = 0

    def add_turn(
        self,
        audio_seconds: float,
        prompt_tokens: int,
        completion_tokens: int,
        tts_chars: int,
    ) -> None:
        self.turns += 1
        self.audio_seconds += audio_seconds
        self.prompt_tokens += prompt_tokens
        self.completion_tokens += completion_tokens
        self.tts_chars += tts_chars

    @property
    def total_tokens(self) -> int:
        return self.prompt_tokens + self.completion_tokens

    @property
    def is_empty(self) -> bool:
        return self.turns == 0
