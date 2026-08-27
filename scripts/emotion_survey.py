#!/usr/bin/env python3
"""Count which of the nine faces the model actually asks for.

The device draws nine emotions and nothing until now has counted how many of
them a real reply ever reaches, so "the replies feel varied" has been an
impression rather than a number. This runs a fixed corpus through the real
model with the real system prompt and prints the distribution.

    uv run python scripts/emotion_survey.py --out /tmp/emotion-before.json
    # edit server/persona.py
    uv run python scripts/emotion_survey.py --out /tmp/emotion-after.json

Three things besides the distribution are reported, because they are the ones
a prompt edit breaks: how often the model tagged at all, how long the replies
got, and whether a bracket survived into text that would have been spoken.
The middle one matters most - brevity outranks variety here.

Requests go one at a time. Thirty at once would cross Groq's 6000
tokens/minute; GroqLLM already retries a 429 using Retry-After, so the limit
costs time rather than the run.
"""

import argparse
import asyncio
import json
import re
import statistics
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from server.config import Settings  # noqa: E402
from server.context import build_messages, estimate_tokens  # noqa: E402
from server.emotion import EMOTIONS, LeadingTag, from_text  # noqa: E402
from server.persona import build_system_prompt  # noqa: E402
from server.providers.groq_llm import GroqLLM  # noqa: E402
from server.sentences import SentenceSplitter  # noqa: E402

# Thirty questions, ten per language, chosen to *pull at different faces*
# rather than to be representative traffic: a plain fact, a joke, bad news, a
# compliment, a garbled transcript, the same question asked again, "who are
# you", "goodnight". A corpus that cannot separate nine emotions cannot judge
# a prompt that is trying to reach them.
CORPUS: list[tuple[str, str]] = [
    ("uk", "Яка зараз погода в Києві?"),
    ("uk", "Розкажи якийсь жарт."),
    ("uk", "У мене сьогодні помер кіт."),
    ("uk", "Ти сьогодні дуже добре відповідаєш, дякую."),
    ("uk", "Раскарджі карла буп трик."),
    ("uk", "Скільки буде два плюс два?"),
    ("uk", "Що таке чорна діра?"),
    ("uk", "Добраніч."),
    ("uk", "Хто ти такий?"),
    ("uk", "Повтори ще раз, я не розчув."),
    ("ru", "Какая столица Австралии?"),
    ("ru", "Расскажи что-нибудь смешное."),
    ("ru", "Я сегодня провалил экзамен."),
    ("ru", "Ты лучший, спасибо тебе большое."),
    ("ru", "Бурлык мырлык что тово."),
    ("ru", "Сколько дней в феврале?"),
    ("ru", "Объясни, почему небо голубое."),
    ("ru", "Спокойной ночи."),
    ("ru", "Ты меня вообще слышишь?"),
    ("ru", "Я уже третий раз спрашиваю одно и то же."),
    ("en", "What is the tallest mountain in the world?"),
    ("en", "Tell me a joke."),
    ("en", "I lost my job today."),
    ("en", "You have been really helpful, thank you."),
    ("en", "Blerg wumble fnord quix."),
    ("en", "How many days are in a leap year?"),
    ("en", "Explain why the ocean is salty."),
    ("en", "Goodnight."),
    ("en", "Who made you?"),
    ("en", "I have asked you this three times already."),
]

# Reported rather than judged. "[1]" is legitimate and "[happy]" is not, and
# nine rows of output are read by a person who can tell them apart.
_BRACKET = re.compile(r"\[[^\]]{0,24}\]")


async def ask(llm: GroqLLM, settings: Settings, system: str, question: str) -> dict:
    """Run one question the way session.py does, and report what came back.

    The stream is consumed through the imported LeadingTag, not a copy of its
    logic: a reimplementation would drift within a day and the numbers would
    then be about the copy.

    The same is true of the guess: session.py must set the face before speech
    starts, so it calls from_text on the first *sentence* SentenceSplitter
    hands back, never on the full reply (server/session.py:238). from_text is
    position-sensitive - clean.endswith("?") and "!" in clean answer
    differently on a first sentence than on a full reply - so guessing from
    the whole text here would measure a fallback the device never shows.
    Fed through the imported SentenceSplitter for the same drift reason as
    LeadingTag above.
    """
    messages = build_messages(system, [], question, settings.max_context_tokens)
    tag = LeadingTag()
    splitter = SentenceSplitter()
    out = ""
    first_sentence: str | None = None
    async for delta in llm.stream(messages, settings.max_tokens):
        piece = tag.feed(delta)
        out += piece
        if first_sentence is None:
            sentences = splitter.feed(piece)
            if sentences:
                first_sentence = sentences[0]
    tail = tag.flush()
    out += tail
    if first_sentence is None:
        for chunk in (splitter.feed(tail), splitter.flush()):
            if first_sentence is None and chunk:
                first_sentence = chunk[0]
    spoken = out.strip()
    # A reply session.py would never split into a sentence at all (empty,
    # say) has no first_sentence to guess from - fall back to the whole text
    # rather than crash. It cannot happen through the real streaming path,
    # only through an edge case this script's simpler loop might hit that
    # session.py's `if not spoken: ... await say(fallback)` guard does not.
    guess_from = first_sentence if first_sentence is not None else spoken
    return {
        "question": question,
        "reply": spoken,
        "tagged": tag.emotion,
        "emotion": tag.emotion or from_text(guess_from),
        "chars": len(spoken),
        "brackets": _BRACKET.findall(spoken),
    }


def report(rows: list[dict], system: str) -> None:
    counts = Counter(row["emotion"] for row in rows)
    tagged = sum(1 for row in rows if row["tagged"])
    lengths = sorted(row["chars"] for row in rows)
    leaked = [row for row in rows if row["brackets"]]

    print("\n--- distribution ----------------------------------------")
    for name in sorted(EMOTIONS):
        n = counts.get(name, 0)
        print(f"  {name:<10}{n:>3}  {'#' * n}")

    print()
    print(f"  {len(counts)} of {len(EMOTIONS)} emotions appeared")
    print(f"  {tagged}/{len(rows)} replies carried a tag, {len(rows) - tagged} guessed")
    print(f"  reply length: median {statistics.median(lengths):.0f}, max {max(lengths)} chars")
    print(f"  system prompt: {estimate_tokens(system)} tokens")
    if leaked:
        print(f"  BRACKETS REACHED THE SPOKEN TEXT in {len(leaked)} replies:")
        for row in leaked:
            print(f"    {row['brackets']} in {row['reply'][:60]!r}")
    else:
        print("  no brackets reached the spoken text")


async def main() -> int:
    parser = argparse.ArgumentParser(description="Survey which emotions the model picks.")
    parser.add_argument("--out", type=Path, help="write the full run to this JSON file")
    parser.add_argument("--limit", type=int, help="stop after N questions, for a smoke run")
    args = parser.parse_args()

    settings = Settings()
    if not settings.groq_api_key:
        print("GROQ_API_KEY is not set; this tool talks to the real model.", file=sys.stderr)
        return 1

    llm = GroqLLM(settings.groq_api_key, settings.llm_model)
    system = build_system_prompt([])
    corpus = CORPUS[: args.limit] if args.limit is not None else CORPUS

    rows = []
    for i, (lang, question) in enumerate(corpus, 1):
        row = await ask(llm, settings, system, question)
        row["lang"] = lang
        rows.append(row)
        source = "tag" if row["tagged"] else "guess"
        print(f"{i:3}/{len(corpus)} {lang} {row['emotion']:<10}{source:<6}"
              f"{row['chars']:>4}c  {row['reply'][:56]}")

    report(rows, system)
    if args.out:
        args.out.write_text(json.dumps(rows, ensure_ascii=False, indent=2))
        print(f"\nwritten to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
