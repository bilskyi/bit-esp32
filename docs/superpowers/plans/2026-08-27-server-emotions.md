# Server-side emotions — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the model reach more than two or three of the nine faces the device can draw, and produce the number that says whether it did.

**Architecture:** Three independent changes, in a deliberate order. A bench script measures which emotions the model asks for today. The persona's tag rule is then rewritten — not extended — so the tag describes the answer rather than a feeling the model does not have, and the same script re-run says whether that worked. Separately, the text heuristic that covers a missing tag grows from four reachable emotions to seven, with the other two recorded as decisions.

**Tech Stack:** Python 3.12, pytest with `asyncio_mode = "auto"`, `uv` for running everything, Groq (`openai/gpt-oss-120b`) through the existing `server.providers.groq_llm.GroqLLM`.

**Spec:** `docs/superpowers/specs/2026-08-27-server-emotions-design.md`

## Global Constraints

- The nine emotion names in `server/emotion.py` must stay exactly the nine in `firmware/main/face.c`. `tests/test_emotion.py::test_the_names_match_the_firmware_exactly` reads the C directly. Do not add or rename one.
- The system prompt with five remembered facts must estimate under **400 tokens** (`tests/test_persona.py::test_stays_small_enough_to_leave_room_for_history`). The rewrite in Task 2 lands at 380. **Do not raise this ceiling in this plan.**
- Brevity outranks variety. `tests/test_persona.py::test_caps_reply_length` requires the literal strings `one or two short sentences`, `thirty words` and `never longer` to survive in the prompt.
- No bracketed tag may ever reach TTS. This is the module's first stated invariant and several tests guard it.
- Run everything through `uv`: `uv run pytest`, `uv run python scripts/...`. The project venv is 3.12 and is not activated by default.
- All 189 existing tests must still pass at the end of every task.

---

### Task 1: The survey that makes "more varied" a number

**Files:**
- Create: `scripts/emotion_survey.py`
- Test: none. This is a bench tool that talks to the real Groq API, like `scripts/mic_to_stt.py` and `scripts/fake_device.py`, neither of which has one. Its verification is the run itself, and its deliverable is the baseline it prints.

**Interfaces:**
- Consumes: `server.config.Settings`, `server.context.build_messages`, `server.context.estimate_tokens`, `server.emotion.EMOTIONS`, `server.emotion.LeadingTag`, `server.emotion.from_text`, `server.persona.build_system_prompt`, `server.providers.groq_llm.GroqLLM`.
- Produces: a JSON array of row objects with keys `lang`, `question`, `reply`, `tagged`, `emotion`, `chars`, `brackets`. Task 2 re-runs this script and compares two such files.

- [ ] **Step 1: Write the script**

Create `scripts/emotion_survey.py`:

```python
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
    """
    messages = build_messages(system, [], question, settings.max_context_tokens)
    tag = LeadingTag()
    out = ""
    async for delta in llm.stream(messages, settings.max_tokens):
        out += tag.feed(delta)
    out += tag.flush()
    spoken = out.strip()
    return {
        "question": question,
        "reply": spoken,
        "tagged": tag.emotion,
        "emotion": tag.emotion or from_text(spoken),
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
    corpus = CORPUS[: args.limit] if args.limit else CORPUS

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
```

- [ ] **Step 2: Smoke-run it on three questions**

Run: `uv run python scripts/emotion_survey.py --limit 3`

Expected: three numbered lines, each naming an emotion from the nine and a
reply in Ukrainian, then the distribution block. If it prints
`GROQ_API_KEY is not set`, the `.env` in the repo root is missing or empty —
stop and say so rather than working around it.

- [ ] **Step 3: Take the baseline**

Run: `uv run python scripts/emotion_survey.py --out /tmp/emotion-before.json`

Expected: thirty lines, then the distribution. **Copy the summary block into
the commit message** — the count of distinct emotions, the tagged/guessed
split, and the median and max reply length. These are the numbers Task 2 is
judged against; nothing else records them.

- [ ] **Step 4: Confirm nothing else broke**

Run: `uv run pytest -q`
Expected: all tests pass. (The script is not imported by anything, so this is
a guard against an accidental edit, not a real risk.)

- [ ] **Step 5: Commit**

```bash
git add scripts/emotion_survey.py
git commit -m "$(cat <<'EOF'
Count the faces the model actually asks for

The device draws nine and nobody has counted. Thirty questions, ten per
language, through the real model and the real prompt, consumed by the
same LeadingTag the session uses rather than a copy of it.

It reports three things besides the distribution, because they are what a
prompt edit breaks: whether the model tagged at all, how long the replies
got, and whether a bracket reached text that would have been spoken.
Brevity is the one to watch - it outranks variety here.

Baseline, today's prompt:
<paste the summary block from step 3>

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: The tag rule stops asking how the model feels

**Files:**
- Modify: `server/persona.py:16-19` (the first bullet of `BASE`)
- Test: `tests/test_persona.py:50-55` (rewrite) plus two new tests

**Interfaces:**
- Consumes: the script from Task 1, re-run.
- Produces: nothing importable. `build_system_prompt(facts) -> str` is unchanged in signature and in every other rule.

- [ ] **Step 1: Write the failing tests**

In `tests/test_persona.py`, **replace** `test_puts_the_tag_rule_before_everything_else` (currently lines 50-55) with this, and append the two new tests after it:

```python
def test_puts_the_tag_rule_before_everything_else():
    """It has to be the very first token of the reply, so it is the first
    thing the model is told.

    Asserts the position and the subject, not the wording: the wording is
    deliberately tuned and a test pinned to it would have to be edited every
    time it is, which teaches everyone to edit the test rather than read it.
    """
    prompt = build_system_prompt([])
    first_rule = prompt.split("Rules:", 1)[1].strip().split("\n")[0]
    assert first_rule.startswith("-")
    assert "square brackets" in first_rule
    assert "before any words" in first_rule


def test_the_tag_is_read_off_the_answer_rather_than_off_a_feeling():
    """The rule used to open with "how you feel about it". An assistant
    answering "what is the weather" honestly feels nothing about it, and
    [neutral] is the correct answer to the question as asked - which is how
    eight of the nine emotions stopped appearing. It now points at the reply.
    """
    prompt = build_system_prompt([]).lower()
    assert "how you feel" not in prompt
    assert "read off what you are about to say" in prompt


def test_neutral_is_denied_its_second_job():
    """[neutral] eats the other eight when it doubles as "unsure which one"."""
    prompt = build_system_prompt([]).lower()
    assert "not for being unsure" in prompt
```

- [ ] **Step 2: Run them to verify they fail**

Run: `uv run pytest tests/test_persona.py -q`

Expected: **exactly 2 failures**, and knowing which two matters.

- `test_the_tag_is_read_off_the_answer_rather_than_off_a_feeling` — fails on
  `assert "how you feel" not in prompt`. **Red.**
- `test_neutral_is_denied_its_second_job` — fails on the missing
  `not for being unsure`. **Red.**
- `test_puts_the_tag_rule_before_everything_else` — **passes already**, and is
  meant to. Verified against today's prompt: its first rule contains both
  `square brackets` and `before any words`. That test is being *loosened*, not
  driven — it used to pin the exact sentence this task rewrites, and its job
  now is to keep guarding position and subject across both wordings. A test
  that stays green through a rewrite it was never about is the correct
  outcome, not a missing red.

If you see three failures, the loosened test was mistyped.

- [ ] **Step 3: Rewrite the rule**

In `server/persona.py`, replace the first bullet of `BASE` (lines 16-19) with:

```python
BASE = """You are a warm, direct voice companion. Your replies are spoken aloud.

Rules:
- Begin every reply with a face in square brackets, before any words. Not a \
feeling you are reporting: the expression this answer should be worn with, \
read off what you are about to say. Exactly one of [neutral] [happy] \
[excited] [curious] [confused] [surprised] [sad] [annoyed] [sleepy], always \
first. [neutral] is for an answer that leans nowhere, not for being unsure. \
It drives a face on the device, it is never spoken, and never mentioned.
- Answer in one or two short sentences. Thirty words at the very most. Never longer. No lists, no headings, no markdown.
```

Leave every rule after this one exactly as it is. Also update the module
docstring's second paragraph, which still describes the old framing:

```python
"""The system prompt.

The single most important constraint here is brevity. A model that lectures is
unbearable when read aloud, and every extra sentence also costs TTS time and
tokens against a 6000/minute ceiling.

The emotion tag is the one thing here that is not about the words. It goes
first because it has to be the very first token, and server.emotion removes it
before anything can read it out. If the model ignores the rule, nothing breaks:
the emotion is guessed from the text instead.

It asks what expression the *answer* should wear rather than how the model
feels, because a model answering "what is the weather" feels nothing about it
and [neutral] is the honest reply to the question as it used to be asked.
"""
```

- [ ] **Step 4: Run the persona tests**

Run: `uv run pytest tests/test_persona.py -q`
Expected: PASS, all of them — including
`test_stays_small_enough_to_leave_room_for_history`, which caps the prompt at
400 tokens. The rewrite measures 380 with five facts. If it fails, the wording
drifted while being pasted; tighten it back rather than raising the cap.

- [ ] **Step 5: Run the whole suite**

Run: `uv run pytest -q`
Expected: all tests pass. `tests/test_emotion.py` is untouched by this task.

- [ ] **Step 6: Re-run the survey and compare**

Run: `uv run python scripts/emotion_survey.py --out /tmp/emotion-after.json`

Read the two summary blocks side by side. The question is how many distinct
emotions appeared, before and after. **Two numbers must not have got worse,
and they matter more than the first one:** the median reply length, and the
count of replies where brackets reached the spoken text (which must stay 0).

If variety did not move, say so plainly and stop — the glossary is the spec's
recorded next move and it is deliberately out of this plan's scope, because it
does not fit under the 400-token cap. Do not raise the cap to smuggle it in.

- [ ] **Step 7: Commit**

```bash
git add server/persona.py tests/test_persona.py
git commit -m "$(cat <<'EOF'
Ask what face the answer wears, not how the model feels about it

"Start every reply with how you feel about it" is a question that a
weather answer has no answer to, so [neutral] was the honest reply and
the other eight emotions went unused. The rule now points at the reply
itself: the expression this answer should be worn with, read off what is
about to be said. [neutral] is also explicitly denied its second job, the
safe choice under uncertainty, which is the job in which it ate the rest.

Rewritten rather than extended, and measured while writing: the first
draft cost 393 tokens against the 400 the persona test enforces. This one
is 380. A glossary of the nine does not fit under that cap and is a
separate decision.

Distribution, before and after, same thirty questions:
<paste both summary blocks>

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: The heuristic reaches seven, and names the two it will not

**Files:**
- Modify: `server/emotion.py:49-103` (the `_SORRY` block and `from_text`)
- Test: `tests/test_emotion.py` (extend the `from_text` section)

**Interfaces:**
- Consumes: nothing from Tasks 1-2. This task is independent and could be done first; it is last because it is the safety net, not the thing being measured.
- Produces: `server.emotion.GUESSABLE: frozenset[str]` — the seven emotions `from_text` may return. `from_text(text: str) -> str` keeps its signature and now always returns a member of `GUESSABLE`.

- [ ] **Step 1: Write the failing tests**

Append to the `from_text` section of `tests/test_emotion.py`, after
`test_the_heuristic_always_returns_something_the_firmware_knows`:

```python
@pytest.mark.parametrize(
    "text",
    [
        "Не зрозумів, про що ти.",
        "Не понял вопрос, уточни.",
        "I'm not sure what you mean.",
    ],
)
def test_not_understanding_the_question_is_confused(text):
    """Different from sadness: sadness apologises for the answer, confusion
    asks for the question again."""
    assert from_text(text) == "confused"


@pytest.mark.parametrize(
    "text",
    ["Ого, не знав такого.", "Ничего себе!", "Wow, that is new to me."],
)
def test_a_reaction_is_surprised(text):
    assert from_text(text) == "surprised"


@pytest.mark.parametrize(
    "text",
    ["Привіт! Радий тебе чути.", "Спасибо, что спросил.", "Thanks, glad to help."],
)
def test_greetings_and_thanks_are_happy(text):
    assert from_text(text) == "happy"


def test_an_apology_beats_not_understanding():
    """Both signals fire here and the apology is the more specific fact, for
    the same reason it already beats the question mark."""
    assert from_text("Вибач, не зрозумів питання.") == "sad"


def test_not_understanding_beats_the_question_mark():
    """It ends in a question and is not curiosity - it is a request for the
    question again."""
    assert from_text("Не зрозумів, що саме ти маєш на увазі?") == "confused"


def test_surprise_beats_the_exclamation_mark():
    assert from_text("Ого!") == "surprised"


def test_a_greeting_beats_the_exclamation_mark():
    """"Привіт!" is warmth before it is excitement."""
    assert from_text("Привіт!") == "happy"


def test_the_heuristic_names_what_it_cannot_produce():
    """annoyed and sleepy are a decision, not an omission.

    annoyed cannot be read off the assistant's own reply - the reply is polite
    by construction, so a rule inferring irritation from it either never fires
    or fires in the wrong place. sleepy must not be read off it at all: the
    firmware falls asleep on its own timer after 90 s of idle and a server
    guessing sleepiness from words would fight that timer. Both stay reachable
    the way they were always meant to be, through the model's tag.

    Asserted rather than left implicit, so a tenth name added to EMOTIONS
    forces a decision instead of passing silently.
    """
    assert GUESSABLE < EMOTIONS
    assert EMOTIONS - GUESSABLE == {"annoyed", "sleepy"}


def test_every_guess_lands_inside_the_advertised_set():
    corpus = [
        "",
        "?",
        "!",
        "...",
        "1234",
        "Привіт",
        "У Києві зараз близько двадцяти градусів.",
        "А в тебе як?",
        "Авжеж, зробимо!",
        "Вибач, я не розчув.",
        "Не зрозумів, про що ти.",
        "Ого, не знав такого.",
        "Спасибо, что спросил.",
        "Goodnight, sleep well.",
        "I have asked you this three times already.",
    ]
    for text in corpus:
        assert from_text(text) in GUESSABLE, text
```

And extend the import at the top of the file (currently lines 3-11) to pull in
`GUESSABLE`:

```python
from server.emotion import (
    DEFAULT,
    EMOTIONS,
    GUESSABLE,
    HEAD_LIMIT,
    LeadingTag,
    from_text,
    split_tag,
    strip_tags,
)
```

- [ ] **Step 2: Run them to verify they fail**

Run: `uv run pytest tests/test_emotion.py -q`

Expected: a collection error —
`ImportError: cannot import name 'GUESSABLE' from 'server.emotion'`. That one
error masks the rest, which is fine: it is the first thing Step 3 fixes.

- [ ] **Step 3: Extend the heuristic**

In `server/emotion.py`, after the existing `_SORRY` tuple (which ends at line
65) add the three new phrase tuples and the `GUESSABLE` constant:

```python
# The reply did not understand the *question*, which is a different face from
# not knowing the answer: confusion asks for the question again, sadness
# apologises for the answer. Checked after _SORRY, because "Вибач, не
# зрозумів" is both and the apology is the more specific fact.
_CONFUSED = (
    "не зрозумів",
    "не зрозуміла",
    "що саме",
    "уточни",
    "уточніть",
    "не понял вопрос",
    "что именно",
    "what do you mean",
    "not sure what you mean",
    "could you clarify",
)

# Checked before the exclamation mark, because "Ого!" would otherwise be read
# as excitement.
_SURPRISED = (
    "ого",
    "нічого собі",
    "оце так",
    "ничего себе",
    "надо же",
    "wow",
    "no way",
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
```

Then replace `from_text` (currently lines 90-103) with:

```python
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
    if any(phrase in low for phrase in _CONFUSED):
        return "confused"
    if any(phrase in low for phrase in _SURPRISED):
        return "surprised"
    if any(phrase in low for phrase in _HAPPY):
        return "happy"
    if clean.endswith("?"):
        return "curious"
    if "!" in clean:
        return "excited"
    return DEFAULT
```

Finally, update the module docstring's numbered point 2 to say what the
fallback now covers — it currently promises only "a heuristic":

```
2. There must always be an emotion, tag or no tag. A missing tag falls back to
   a heuristic rather than leaving the face on whatever it showed last. That
   heuristic reaches seven of the nine; see GUESSABLE for the two it will not
   guess and why.
```

- [ ] **Step 4: Run the emotion tests**

Run: `uv run pytest tests/test_emotion.py -q`

Expected: PASS, including the pre-existing ones.

The phrase lists above are not a guess: the whole rule chain was run against
every input in this file, old and new, before this plan was written. All
twenty-three expectations matched and nothing in the corpus escaped
`GUESSABLE`. Three pre-existing tests were the ones at risk, because the new
rules sit above them and could have stolen their inputs — none does:

- `test_an_exclamation_is_excited` — "Авжеж, зробимо!" → `excited`
- `test_a_question_back_is_curious` — "А в тебе як?" → `curious`
- `test_the_heuristic_ignores_tags_left_in_the_text` — "[happy] Все добре!" → `excited`

One behaviour does change, and no test pinned it either way:
`from_text("Привіт")` was `neutral` and is now `happy`. That is the point of
the change. `test_the_heuristic_always_returns_something_the_firmware_knows`
covers that input and only asserts membership, so it stays green.

- [ ] **Step 5: Run the whole suite**

Run: `uv run pytest -q`
Expected: all tests pass.

- [ ] **Step 6: Commit**

```bash
git add server/emotion.py tests/test_emotion.py
git commit -m "$(cat <<'EOF'
Let the fallback reach seven faces, and say which two it will not

The heuristic could produce four of the nine, so if the model ever
stopped tagging, five faces became unreachable. It now reads confusion,
surprise and warmth as well, in an order that matters more than the rules
do: confusion sits above the question mark because "Не зрозумів, що саме?"
ends in one and is not curiosity, and surprise above the exclamation mark
because "Ого!" is not excitement.

annoyed and sleepy stay out, recorded as a decision rather than left as a
gap. A polite reply carries no irritation to read, and the firmware
already falls asleep on its own 90 s timer - a server guessing sleepiness
from words would fight it. Both remain reachable through the model's tag.
GUESSABLE names the seven and a test asserts the two exclusions, so a
tenth emotion forces a decision instead of passing silently.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Record it in the handoff

**Files:**
- Modify: `RESUME.md` — the "Measured, so nobody re-derives it" table and the face section's "Verified off the bench" list

**Interfaces:**
- Consumes: the two summary blocks from Tasks 1 and 2, and the final test count.
- Produces: nothing importable.

- [ ] **Step 1: Update the measured table**

`RESUME.md` currently carries one emotion row:

```
| Emotion tag | model tagged 8/8 replies, three languages, nothing leaked |
```

Replace it with the real distribution, using the numbers from the two survey
runs. Write what was actually measured — if variety did not move, the row says
so. Example shape, with the real figures substituted:

```
| Emotion tag | tagged N/30 replies, three languages, nothing leaked |
| Emotion spread | X of 9 before the prompt rewrite, Y after, over 30 questions |
| Reply length | median A chars before, B after - the rewrite's likely casualty |
```

- [ ] **Step 2: Update the test count and the fallback's reach**

The face section's "Verified off the bench" list says `189 server tests`. Run
`uv run pytest -q` and put the new total in. In the same list, add one line
recording that the fallback reaches seven of nine by decision, so the next
session does not read it as an unfinished job.

- [ ] **Step 3: Note what is still open**

Under "Agreed next, in order", add the three items the spec deliberately left
open, so they are not rediscovered from scratch:

- a glossary of the nine emotions, decided on the survey numbers, needing the
  400-token cap raised on purpose
- emotion during `listening` and `thinking` — the firmware accepts a frame at
  any moment (`s_face_emotion_seq` re-triggers even on a repeat) and the
  server uses that exactly once per reply
- mood across turns — the emotion is written neither to `history` nor to the
  store, so nothing carries between replies

- [ ] **Step 4: Commit**

```bash
git add RESUME.md
git commit -m "$(cat <<'EOF'
Record what the emotion survey measured, and what it left open

The handoff carried one line about emotions - 8/8 tagged - which said
nothing about how many of the nine ever appeared. It now carries the
distribution over thirty questions, before and after the prompt rewrite,
and the reply-length figures that say whether variety cost brevity.

Three things the spec left open are written down rather than left to be
rediscovered: the glossary, emotion during listening and thinking, and
mood across turns.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-review

**Spec coverage**

| Spec section | Task |
|---|---|
| The prompt rewrite, exact wording | Task 2, Step 3 |
| Token budget stays under 400 | Global Constraints; Task 2, Step 4 |
| Glossary deferred, cap not raised | Global Constraints; Task 2, Step 6 |
| The survey: corpus, LeadingTag reuse, four reported figures, JSON, sequential | Task 1, Step 1 |
| Heuristic: seven emotions, the ordering, the two exclusions | Task 3, Step 3 |
| Tests: per-language reachability, three precedence tests, the registry test | Task 3, Step 1 |
| `test_puts_the_tag_rule_before_everything_else` rewritten to guard position | Task 2, Step 1 |
| Existing emotion tests expected to pass unchanged | Task 3, Step 4 |
| Order of work: survey → prompt → heuristic | Task order |

The spec's "what this leaves open" section is not implemented by design; Task 4
records it instead so it survives the handoff.

**Type consistency**

`GUESSABLE` is `frozenset[str]`, defined in Task 3 Step 3 and used in Task 3
Step 1 under the same name. `from_text(text: str) -> str` and
`LeadingTag.feed/flush` keep their existing signatures throughout. The survey's
row dict keys — `lang`, `question`, `reply`, `tagged`, `emotion`, `chars`,
`brackets` — are written in `ask`/`main` and read in `report` with no drift.
