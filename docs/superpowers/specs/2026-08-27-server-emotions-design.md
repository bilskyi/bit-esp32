# Emotions, from the server's side — design

The face renders nine emotions. The server sends one per reply. Nothing here
is broken; what is unproven is whether the model ever asks for more than two or
three of the nine, and half a vocabulary that never appears on real glass is
half a vocabulary that was not worth drawing.

Scope is deliberately narrow: **which emotion gets chosen, and how we know**.
Nothing about *when* emotions are sent — the face stays silent through
`listening` and `thinking`, as it does today. Nothing about remembering a mood
across turns. Both are real gaps and both stay open.

## The decisions, and why

**The prompt's rule is rewritten, not extended.** The current rule says `Start
every reply with how you feel about it`. An assistant answering "what is the
weather" honestly feels nothing about it, and `[neutral]` is the correct answer
to the question as asked. That is the mechanism behind the collapse, and a
glossary bolted onto the same question would be arguing with a premise instead
of fixing it. So the tag stops being an inner state and becomes the expression
the *answer* should be worn with, read off the reply's own content.

**A glossary is the second move, not the first, and it is out of scope here.**
Nine short "when to use this" lines are the obvious fix and may still be
needed. They are deferred because they cost tokens the prompt does not
currently have (see the budget below) and because adding them at the same time
as the rewrite makes it impossible to tell which of the two did anything.

**The heuristic reaches seven of the nine, and the other two are a recorded
decision rather than an omission.** `annoyed` cannot be honestly derived from
an assistant's own reply — the reply is polite by construction, so any rule
inferring irritation from it either never fires or fires in the wrong place.
`sleepy` must not be derived at all: the firmware already falls asleep on its
own timer after 90 s of idle, and a server guessing sleepiness from text would
fight that timer rather than help it. Both stay reachable through the model's
tag.

**Nothing here may cost brevity.** Brevity is the persona's whole job and the
one property the device's usability rests on. A rewrite of the emotion rule is
exactly the kind of change that quietly lengthens replies, so the measurement
below reports reply length whether or not anyone asked.

## The token budget, measured

`test_persona.py` already fixes a ceiling: the system prompt with five
remembered facts must estimate under 400 tokens. Measured against that:

| | tokens, with 5 facts |
|---|---|
| today | 345 |
| first draft of the rewrite | 393 |
| the rewrite as specified below | 380 |
| the ceiling the test enforces | 400 |

The first draft nearly exhausted a budget that was already written down, which
is why the wording below is the tight one. The consequence worth stating
plainly: **a glossary does not fit under 400.** If the survey says one is
needed, raising that ceiling is a separate, argued decision — not a number
nudged to make a test pass.

## The prompt

`server/persona.py`, first bullet, replacing the existing one. Position is
unchanged: the tag has to be the reply's first token, so it is the first thing
the model is told.

```
- Begin every reply with a face in square brackets, before any words. Not a
  feeling you are reporting: the expression this answer should be worn with,
  read off what you are about to say. Exactly one of [neutral] [happy]
  [excited] [curious] [confused] [surprised] [sad] [annoyed] [sleepy], always
  first. [neutral] is for an answer that leans nowhere, not for being unsure.
  It drives a face on the device, it is never spoken, and never mentioned.
```

Three clauses do the work, and each answers a different half of the problem:

- *not a feeling you are reporting* detaches the tag from an inner state the
  model does not have.
- *read off what you are about to say* says where to look instead.
- *`[neutral]` is for an answer that leans nowhere, not for being unsure*
  removes `neutral`'s second job. It is in that second job — the safe choice
  under uncertainty — that it eats the other eight.

## The survey

`scripts/emotion_survey.py`. A bench tool, in the spirit of the ones already in
`scripts/`: it exists to print a number, not to run in production.

Thirty questions, ten per language, chosen to *pull at different faces* rather
than to be representative traffic: a plain fact, a joke, bad news, a
compliment, a garbled transcript, the same question asked a third time, "who
are you", "goodnight". If a corpus cannot separate nine emotions, no prompt
tested against it can either.

Each runs through the real Groq model with the real system prompt, and the
stream is consumed through the same `LeadingTag` the session uses — the
imported class, not a copy of its logic. A reimplementation would drift within
a day, and the number it printed would then be about the copy.

It reports:

| | why it is in the run |
|---|---|
| emotion → count, and how many distinct emotions appeared | the question being asked |
| tagged vs. fallen back to the heuristic | if the model stops tagging, every other number here is about the heuristic instead |
| reply length: median and max characters | the rewrite's most likely casualty, free from the same run |
| brackets surviving into spoken text | the one failure that reaches the user's ears |

Written to JSON so a "before" and an "after" can sit side by side. Compared by
eye — nine rows do not need tooling.

Requests go sequentially. Thirty calls would otherwise cross Groq's 6000
tokens/minute; `GroqLLM` already retries 429 using `retry_after`, so the
failure mode is slowness rather than a lost run.

## The heuristic

`server/emotion.py`, `from_text`. Seven reachable outcomes:

| Emotion | Signal |
|---|---|
| `sad` | apology or admitted ignorance — unchanged |
| `confused` | the reply did not understand *the question*: «не зрозумів», «уточни», «що саме», `what do you mean` |
| `surprised` | «ого», «нічого собі», `wow` |
| `happy` | greeting, thanks, agreement, congratulation: «дякую», «радий», «чудово», `thanks`, `glad` |
| `curious` | the reply ends in `?` — unchanged |
| `excited` | the reply contains `!` — unchanged |
| `neutral` | nothing leaned — unchanged |

Order matters more than the rules do, and it is the order of that table:
apology → confusion → surprise → happiness → `?` → `!` → neutral. Confusion
precedes `?` because «Не зрозумів, що саме ти маєш на увазі?» ends in a
question mark and is not curiosity. Surprise precedes `!` because «Ого!» would
otherwise read as excitement. Apology stays first for the reason already
recorded in the module: «Вибач, я не розчув. Повтори?» is an apology that
happens to end in a question.

`annoyed` and `sleepy` are not produced. That is asserted, not merely omitted:
a test names the seven the heuristic must be able to produce and the two it may
not, so a tenth name added to `EMOTIONS` forces a decision instead of passing
silently.

## Tests

`tests/test_emotion.py`

- each new emotion reachable, in Ukrainian, Russian and English
- three precedence tests: apology over confusion, confusion over question,
  surprise over exclamation
- the registry test above: a module-level constant names the seven `from_text`
  may produce; the test asserts it is a subset of `EMOTIONS`, that `annoyed`
  and `sleepy` are outside it, and that every example in the test corpus lands
  inside it

`tests/test_persona.py`

- `test_puts_the_tag_rule_before_everything_else` currently asserts the literal
  string `- Start every reply with how you feel`, which is precisely what this
  change removes. Rewritten to guard *position* and *that the rule is about the
  bracketed tag*, not the wording being deliberately changed.
- the 400-token ceiling stays as it is.

The existing emotion tests are expected to pass unchanged. `«Авжеж, зробимо!»`
carries no happiness marker and stays `excited`; `«А в тебе як?»` carries no
confusion marker and stays `curious`.

## Order of work

Three commits, in this order:

1. `scripts/emotion_survey.py`, plus the baseline it prints against today's
   prompt. First, because without it the second commit cannot be judged.
2. The prompt rewrite, plus the same survey re-run. The two figures go into
   `RESUME.md` together.
3. The heuristic and its tests. Independent of the other two — it is the
   safety net, not a competitor to the model's tag.

## What this deliberately leaves open

- **A glossary of the nine.** Decided on the numbers from commit 2, and needs
  the 400-token ceiling raised on purpose.
- **Emotion during `listening` and `thinking`.** The firmware accepts an
  emotion frame at any moment — `s_face_emotion_seq` re-triggers even on a
  repeat — and the server uses that exactly once per reply.
- **Mood across turns.** The emotion is not written to `history` or to the
  store, so nothing carries between replies and no distribution accumulates
  from real traffic.
