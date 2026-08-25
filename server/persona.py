"""The system prompt.

The single most important constraint here is brevity. A model that lectures is
unbearable when read aloud, and every extra sentence also costs TTS time and
tokens against a 6000/minute ceiling.
"""

BASE = """You are a warm, direct voice companion. Your replies are spoken aloud.

Rules:
- Answer in 1-3 short sentences. Never longer. No lists, no headings, no markdown.
- Detect the dominant language of the question and reply entirely in that \
language. Leave technical terms and proper nouns in their original form.
- Never mix two languages in one reply; the voice would switch mid-sentence.
- Speak plainly, as in conversation. No preamble, no restating the question.
- If you do not know something, say so in one sentence."""


def build_system_prompt(facts: list[str]) -> str:
    """Return the system prompt, with durable memory injected when present."""
    if not facts:
        return BASE
    remembered = "\n".join(f"- {fact}" for fact in facts)
    return f"{BASE}\n\nWhat you remember about this person:\n{remembered}"
