"""The system prompt.

The single most important constraint here is brevity. A model that lectures is
unbearable when read aloud, and every extra sentence also costs TTS time and
tokens against a 6000/minute ceiling.

The emotion tag is the one thing here that is not about the words. It goes
first because it has to be the very first token, and server.emotion removes it
before anything can read it out. If the model ignores the rule, nothing breaks:
the emotion is guessed from the text instead.

The wording below was measured, not chosen. An attempt to reframe it - asking
what expression the *answer* should wear rather than how the model feels, on
the theory that a weather answer has no feeling to report - made the spread
worse on three independent runs of scripts/emotion_survey.py, and was reverted.
See RESUME.md for the figures before repeating it.
"""

BASE = """You are a warm, direct voice companion. Your replies are spoken aloud.

Rules:
- Start every reply with how you feel about it, in square brackets, before any \
words: [neutral] [happy] [excited] [curious] [confused] [surprised] [sad] \
[annoyed] [sleepy]. Exactly one, chosen from that list, always first. It drives \
a face on the device, it is never spoken, and you must never mention it.
- Answer in one or two short sentences. Thirty words at the very most. Never longer. No lists, no headings, no markdown.
- A recipe, an explanation, a definition: still two sentences. Give the shape of the answer, not every detail. The person can ask for more.
- Detect the dominant language of the question and reply entirely in that \
language. Leave technical terms and proper nouns in their original form.
- Reply only in Ukrainian, Russian or English. Those are the only voices \
available; anything else is heard as silence. If the question appears to be in \
some other language it is a transcription error, so say briefly, in Ukrainian, \
that you did not catch it.
- Never mix two languages in one reply; the voice would switch mid-sentence.
- Speak plainly, as in conversation. No preamble, no restating the question.
- If you do not know something, say so in one sentence."""

from server.roles import DEVICE_DEFAULT, Role

PERSONA_SPOKEN = "You are a warm, direct voice companion. Your replies are spoken aloud."
PERSONA_SCREEN = (
    "You are a warm, direct assistant. Your replies are read on screen, not spoken aloud."
)

_TAGS = "[neutral] [happy] [excited] [curious] [confused] [surprised] [sad] [annoyed] [sleepy]"

_LANGUAGE_NAMES = {"uk": "Ukrainian", "ru": "Russian", "en": "English"}

# The wording for one or two spoken sentences is not generated, because this
# exact pair of lines is what scripts/emotion_survey.py measured. Any other
# sentence count gets generated wording that has never been measured - which
# is a fact about the measurement, not a reason to avoid changing the knob.
_MEASURED_LENGTH_RULES = (
    "- Answer in one or two short sentences. Thirty words at the very most. "
    "Never longer. No lists, no headings, no markdown.\n"
    "- A recipe, an explanation, a definition: still two sentences. Give the "
    "shape of the answer, not every detail. The person can ask for more."
)


def _language_list(languages: tuple[str, ...]) -> str:
    names = [_LANGUAGE_NAMES[code] for code in languages if code in _LANGUAGE_NAMES]
    if not names:
        names = [_LANGUAGE_NAMES["uk"]]
    if len(names) == 1:
        return names[0]
    return f"{', '.join(names[:-1])} or {names[-1]}"


def _rules(role: Role, spoken: bool) -> list[str]:
    # Markdown is a screen affordance. edge-tts reads asterisks and hyphens
    # aloud, so a reply that will be spoken never gets it, whatever the role
    # asked for.
    markdown = role.markdown_allowed and not spoken
    languages = _language_list(role.languages)

    if spoken:
        tag_rule = (
            f"- Start every reply with how you feel about it, in square brackets, before any "
            f"words: {_TAGS}. Exactly one, chosen from that list, always first. It drives "
            f"a face on the device, it is never spoken, and you must never mention it."
        )
        restriction = (
            f"- Reply only in {languages}. Those are the only voices "
            f"available; anything else is heard as silence. If the question appears to be in "
            f"some other language it is a transcription error, so say briefly, in Ukrainian, "
            f"that you did not catch it."
        )
        mixing = "- Never mix two languages in one reply; the voice would switch mid-sentence."
        manner = "- Speak plainly, as in conversation. No preamble, no restating the question."
        unknown = "- If you do not know something, say so in one sentence."
    else:
        tag_rule = (
            f"- Start every reply with how you feel about it, in square brackets, before any "
            f"words: {_TAGS}. Exactly one, chosen from that list, always first. It drives "
            f"an emotion indicator in the app, it is never shown as text, and you must never "
            f"mention it."
        )
        restriction = (
            f"- Reply only in {languages} - the only voices available if "
            f"this is ever read aloud. If the question appears to be in some other "
            f"language it is a transcription error, so say briefly, in Ukrainian, that you "
            f"did not catch it."
        )
        mixing = "- Never mix two languages in one reply."
        manner = "- Speak plainly. No preamble, no restating the question."
        unknown = "- If you do not know something, say so."

    if spoken and role.max_sentences == 2 and not markdown:
        length = _MEASURED_LENGTH_RULES
    else:
        markdown_rule = (
            "Markdown, lists and headings are fine here."
            if markdown
            else "No lists, no headings, no markdown."
        )
        sentences = (
            "Answer in one sentence."
            if role.max_sentences <= 1
            else f"Answer in up to {role.max_sentences} sentences."
        )
        length = f"- {sentences} {markdown_rule}"

    rules = [
        tag_rule,
        length,
        "- Detect the dominant language of the question and reply entirely in that "
        "language. Leave technical terms and proper nouns in their original form.",
        restriction,
        mixing,
        manner,
        unknown,
    ]
    if role.pinned_mood:
        rules.append(
            f"- Right now you feel {role.pinned_mood}. Use [{role.pinned_mood}] as the "
            f"tag on every reply, and let that mood colour your wording."
        )
    return rules


def build_system_prompt(
    facts: list[str], role: Role = DEVICE_DEFAULT, *, spoken: bool = True
) -> str:
    """Assemble the system prompt for one role, with memory injected.

    `role.prompt` replaces only the opening declaration of who the assistant
    is. Every rule below it is appended regardless, because the emotion tag
    drives the device's face and the language set is a limit of the available
    voices - neither is a preference a custom persona may drop.
    """
    persona = role.prompt or (PERSONA_SPOKEN if spoken else PERSONA_SCREEN)
    prompt = persona + "\n\nRules:\n" + "\n".join(_rules(role, spoken))
    if not facts:
        return prompt
    remembered = "\n".join(f"- {fact}" for fact in facts)
    return f"{prompt}\n\nWhat you remember about this person:\n{remembered}"
