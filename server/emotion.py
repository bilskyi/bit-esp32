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
   a heuristic rather than leaving the face on whatever it showed last. That
   heuristic reaches seven of the nine; see GUESSABLE for the two it will not
   guess and why.
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

# The reply did not understand the *question*, which is a different face from
# not knowing the answer: confusion asks for the question again, sadness
# apologises for the answer. Checked after _SORRY, because "Вибач, не
# зрозумів" is both and the apology is the more specific fact.
#
# Matched on word boundaries, for the same reason as _SURPRISED below:
# "уточни" is a bare six characters and hides inside "уточнити" and
# "уточнив", two ordinary conjugations of the same verb - "Хочу уточнити
# деталі замовлення" (I'd like to clarify the order's details) contains no
# confusion, and used to report some anyway because the substring matched
# regardless of what came after it. The rest of the entries here are full
# phrases or long enough on their own that this was never a risk for them.
#
# "що саме" is left as-is: not a matching bug but a semantic one. "Ось що
# саме сталося вчора" (here is what exactly happened yesterday) contains the
# phrase in a plain declarative with nothing confused about it, and no \b or
# anchoring closes that gap - the words are the marker and the words are
# also ordinary. Fixing it needs the sentence's meaning, which this module
# does not have.
_CONFUSED = re.compile(
    r"\b(не зрозумів|не зрозуміла|що саме|уточни|уточніть|не понял вопрос|"
    r"что именно|what do you mean|not sure what you mean|could you clarify)\b",
    re.IGNORECASE,
)

# Checked before the exclamation mark, because "Ого!" would otherwise be read
# as excitement.
#
# Two unrelated fixes live in this one pattern, because they close two
# different kinds of bug and neither generalises to the other.
#
# "ого", "нічого собі", "оце так" and "ничего себе" are matched on \b word
# boundaries: the interjection is short enough - "ого" alone is a bare three
# characters - to hide inside "нічого", "нікого" and "когось", three of the
# commonest words in an ordinary Ukrainian reply. \b alone is not quite
# enough, though: a hyphenated ordinal like "21-ого" ("the 21st") puts a
# hyphen - a non-word character - directly in front of "ого", and a hyphen
# reads as a word boundary exactly as well as a space does. The lookbehind
# excludes that one shape.
#
# "надо же" and "no way" cannot be fixed the same way: their word boundaries
# are already intact - "There is no way to tell from here" bounds "no way"
# cleanly on both sides - so \b has nothing to close. The ambiguity there is
# meaning, not spelling: "надо же" is the interjection ("well I never") in
# one reading and, just as literal a reading, "[we] also need to" ("Надо же
# ещё раз перевірити документи"). Anchoring the two of them to a sentence
# start (`(?:^|[.!?]\s)`) rules out the mid-sentence literal reading and
# leaves every existing test passing, but it is not a full fix: an
# occurrence that opens the sentence outright, like the "документи" example
# above, still reads as surprise. That is accepted as a residual rather than
# hidden - shape alone cannot tell a sentence-initial idiom from a
# sentence-initial literal use, only meaning can, and this module has none.
_SURPRISED = re.compile(
    r"\b((?<![0-9]-)ого|нічого собі|оце так|ничего себе|wow)\b"
    r"|(?:^|[.!?]\s)(?:надо же|no way)\b",
    re.IGNORECASE,
)

# Greeting, thanks, warmth. Checked before the exclamation mark for the same
# reason surprise is: "Привіт!" is warmth before it is excitement.
#
# Deliberately narrow. Substring matching on short Cyrillic words is a trap -
# "рад " matches inside "парад" - so every entry here is long enough not to
# hide inside an ordinary word.
_HAPPY = (
    "привіт",
    "дякую",
    "радий",
    "чудово",
    "нема за що",
    "привет",
    "спасибо",
    "отлично",
    "не за что",
    "hello",
    "thanks",
    "thank you",
    "you're welcome",
    "glad",
)

# What from_text can actually produce. Two of the nine are outside it on
# purpose:
#
# `annoyed` cannot be read off the assistant's own reply. The reply is polite
# by construction, so a rule inferring irritation from it either never fires
# or fires in the wrong place.
#
# `sleepy` must not be read off it at all. The firmware falls asleep on its
# own timer after 90 s of idle, and a server guessing sleepiness from words
# would fight that timer rather than help it.
#
# Both stay reachable the way they were always meant to be: the model tags
# them. A test asserts this set, so a tenth emotion forces a decision.
GUESSABLE = EMOTIONS - {"annoyed", "sleepy"}


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
    """Guess an emotion from a reply, for when the model gave no usable tag.

    The order of the rules is the whole design. Each one overlaps with at
    least one below it, so each sits above the more general signal that would
    otherwise swallow it. The return is always a member of GUESSABLE.
    """
    clean = strip_tags(text).strip()
    if not clean:
        return DEFAULT

    low = clean.lower()
    if any(phrase in low for phrase in _SORRY):
        return "sad"
    if _CONFUSED.search(clean):
        return "confused"
    if _SURPRISED.search(clean):
        return "surprised"
    if any(phrase in low for phrase in _HAPPY):
        return "happy"
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
