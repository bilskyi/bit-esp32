"""Language detection for picking a TTS voice.

The user code-switches between Ukrainian, Russian and English. The LLM is told
to answer in one language, so this only has to classify a finished reply, not
handle mid-sentence switching. A heuristic is deliberate: a model dependency
would cost RAM the Railway container does not have.
"""

DEFAULT = "uk"

# Letters that exist in only one of the two Cyrillic alphabets.
_UA_LETTERS = set("іїєґ")
_RU_LETTERS = set("ыэъё")

# Frequent function words with distinct forms in each language. Words spelled
# identically in both (не, для, при, там, тут) are excluded as pure noise.
_UA_WORDS = frozenset("""
як що це вона вони ви добре привіт справи дякую ласка зараз потім треба
можна дуже тільки який коли чому мене тебе його її від або чи вже ще
робити зробити хочу знаю думаю так
""".split())
_RU_WORDS = frozenset("""
как что это она они вы хорошо привет дела спасибо пожалуйста сейчас потом
нужно можно очень только который когда почему меня тебя его ее от или ли
делать сделать хочу знаю думаю да
""".split())

# ShortNames verified against edge_tts.list_voices().
VOICES = {
    "uk": "uk-UA-OstapNeural",
    "ru": "ru-RU-DmitryNeural",
    "en": "en-US-AndrewNeural",
}

_PUNCT = str.maketrans({c: " " for c in ".,!?;:()[]«»\"'—–-…\n\t"})


def _is_cyrillic(ch: str) -> bool:
    return "Ѐ" <= ch <= "ӿ"


def detect_language(text: str, default: str = DEFAULT) -> str:
    """Return 'uk', 'ru' or 'en' for a block of text."""
    if not text or not text.strip():
        return default

    cyrillic = sum(1 for c in text if _is_cyrillic(c))
    latin = sum(1 for c in text if c.isascii() and c.isalpha())
    if cyrillic == 0 and latin == 0:
        return default
    if cyrillic <= latin:
        # Technical terms stay Latin inside Cyrillic replies, so this only
        # tips to English when English actually dominates.
        return "en"

    lowered = text.lower()
    words = set(lowered.translate(_PUNCT).split())
    ua = sum(1 for c in lowered if c in _UA_LETTERS) + 2 * len(words & _UA_WORDS)
    ru = sum(1 for c in lowered if c in _RU_LETTERS) + 2 * len(words & _RU_WORDS)
    if ua == ru:
        return default
    return "uk" if ua > ru else "ru"


def voice_for(language: str, voices: dict[str, str] | None = None) -> str:
    """Map a detected language to an edge-tts voice ShortName."""
    table = voices or VOICES
    return table.get(language, table[DEFAULT])
