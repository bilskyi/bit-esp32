"""What the assistant feels, and how it says so.

The device shows a pair of eyes. Four states - idle, listening, thinking,
speaking - make that an indicator lamp with eyelids; what makes it a face is
knowing whether the answer it is giving is a happy one. So the model tags each
reply, and this module pulls the tag off before anything can read it aloud.

Three things have to be true, in this order of importance:

1. A tag must never reach TTS. edge-tts will pronounce "curious" without
   hesitation, and the handoff already records it emitting silence for
   characters nobody expected.
2. There must always be an emotion, tag or no tag. A missing tag falls back to
   a heuristic rather than leaving the face on whatever it showed last.
3. Sniffing must not delay the first sentence, which is the whole latency
   budget. The head resolves within a couple of dozen characters and then gets
   out of the way.
"""

import re

# Exactly the nine names face.h enumerates. Anything else the firmware logs and
# ignores, so adding one here without adding it there is a silent no-op.
EMOTIONS = frozenset(
    {
        "neutral",
        "happy",
        "excited",
        "curious",
        "confused",
        "surprised",
        "sad",
        "annoyed",
        "sleepy",
    }
)

DEFAULT = "neutral"

# How much of the reply to hold while looking for the tag. Long enough for
# "[surprised] " with room to spare, short enough that a model which ignores
# the instruction costs a couple of dozen characters of latency and no more.
HEAD_LIMIT = 24

# Letters only, so "[1]" and "[2024]" - which could plausibly be meant - stay.
_TAG = re.compile(r"\[[A-Za-z][A-Za-z _-]{0,19}\]")
_LEADING_TAG = re.compile(r"\s*\[([A-Za-z][A-Za-z _-]{0,19})\]\s*")

# Phrases that mean the assistant could not help. Checked before the
# punctuation rules, because "Вибач, я не розчув. Повтори?" is an apology that
# happens to end in a question mark, and the apology is the more specific fact.
_SORRY = (
    "вибач",
    "перепрошую",
    "не розчув",
    "не знаю",
    "извини",
    "прости",
    "не расслышал",
    "sorry",
    "i don't know",
    "i do not know",
    "didn't catch",
    "did not catch",
)


def split_tag(text: str) -> tuple[str | None, str]:
    """Pull a leading ``[tag]`` off a reply.

    Returns the emotion and the remaining text. An unrecognised tag yields no
    emotion but is still removed: the point of removing it is that it must not
    be spoken, and that is true whether or not we know the word.
    """
    match = _LEADING_TAG.match(text)
    if not match:
        return None, text
    name = match.group(1).strip().lower()
    rest = text[match.end() :]
    return (name if name in EMOTIONS else None), rest


def strip_tags(text: str) -> str:
    """Remove bracketed tags anywhere in the text, and tidy the gap."""
    if "[" not in text:
        return text
    return re.sub(r"\s{2,}", " ", _TAG.sub("", text)).strip()


def from_text(text: str) -> str:
    """Guess an emotion from a reply, for when the model gave no usable tag."""
    clean = strip_tags(text).strip()
    if not clean:
        return DEFAULT

    low = clean.lower()
    if any(phrase in low for phrase in _SORRY):
        return "sad"
    if clean.endswith("?"):
        return "curious"
    if "!" in clean:
        return "excited"
    return DEFAULT


class LeadingTag:
    """Sniffs the head of a token stream for a tag and passes the rest on.

    The tag arrives token by token, so ``[``, ``hap``, ``py``, ``]`` is the
    normal case rather than the edge case. Text is buffered only until the
    question is settled, and after that ``feed`` is a pass-through.
    """

    def __init__(self) -> None:
        self._head = ""
        self._resolved = False
        self._emotion: str | None = None

    @property
    def emotion(self) -> str | None:
        return self._emotion

    @property
    def resolved(self) -> bool:
        return self._resolved

    def feed(self, delta: str) -> str:
        """Take a delta; return the text that is safe to pass downstream."""
        if self._resolved:
            return delta

        self._head += delta
        stripped = self._head.lstrip()

        if not stripped:
            return ""  # nothing but whitespace so far, keep waiting

        if not stripped.startswith("["):
            return self._release()  # no tag is coming

        if "]" in stripped:
            self._emotion, rest = split_tag(self._head)
            self._resolved = True
            self._head = ""
            return rest

        if len(stripped) >= HEAD_LIMIT:
            # A bracket that has not closed by now is not a tag, and the reply
            # must not be held hostage waiting for one.
            return self._release()

        return ""

    def flush(self) -> str:
        """Release anything still held. Call at the end of the stream."""
        if self._resolved:
            return ""
        return self._release()

    def _release(self) -> str:
        self._resolved = True
        out, self._head = self._head, ""
        return out
