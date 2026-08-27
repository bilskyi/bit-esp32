# Emotion survey — the six runs behind "7, 7, 6" vs "5, 5, 6"

`RESUME.md` and the Task 4 write-up in
`docs/superpowers/plans/2026-08-27-server-emotions.md` carry the summary
numbers from this survey — distinct-emotion counts, `neutral` share, median
reply length. This document is the per-emotion breakdown behind those
summaries, kept separately because it would otherwise be lost: only the
summary rows survived in the handoff, not the distributions.

**Corpus:** `scripts/emotion_survey.py`'s `CORPUS` — thirty questions, ten
per language (Ukrainian, Russian, English), run through the real Groq model
(`openai/gpt-oss-120b`) with the real system prompt, three runs per prompt
wording, interleaved so a slow API window could not land entirely on one
wording.

**Source files, as found on disk:**

| File | Contents |
|---|---|
| `/tmp/emotion-before.json` | 30 rows, old (kept) wording — the "recorded run" cited elsewhere for its 29/30 tag rate |
| `/tmp/emotion-after.json` | 30 rows, rewritten (reverted) wording — its paired "recorded run" |
| scratchpad `noise.json` (see below) | four run summaries, two per wording, gathered to measure noise before the decision was made |

The scratchpad path is session-specific:
`/private/tmp/claude-501/-Users-bilskyi-Documents-BIT/d2bca0bc-19b0-490a-b07d-069d137b2e3a/scratchpad/noise.json`.
All three files were present and readable; nothing here is invented. The two
`/tmp/emotion-*.json` files carry full per-reply rows (including the raw
reply text, not reproduced below on purpose — see "Why no raw replies"). The
`noise.json` summaries carry `counts` per emotion directly and no raw text.

Raw replies are not pasted here even though two of the six runs have them
available: model output for a fixed corpus dates badly, and the distribution
is the number that matters. Anyone who needs the sentences behind a given
run's `happy` count can regenerate them from the two `/tmp` files while they
still exist, or re-run the survey.

## Old wording (kept)

| Run | Source | Distinct /9 | neutral | happy | excited | curious | confused | surprised | sad | annoyed | sleepy | `neutral` share | Median chars | Tag rate |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | `noise.json` | 7 | 10 | 7 | 0 | 4 | 3 | 0 | 2 | 2 | 2 | 33% | 72 | 28/30 |
| 2 | `noise.json` | 6 | 12 | 6 | 0 | 4 | 3 | 0 | 3 | 2 | 0 | 40% | 66 | 30/30 |
| 3 | `/tmp/emotion-before.json` | 7 | 11 | 6 | 0 | 4 | 3 | 0 | 3 | 1 | 2 | 37% | 52 | 29/30 |

## Rewritten wording (reverted)

| Run | Source | Distinct /9 | neutral | happy | excited | curious | confused | surprised | sad | annoyed | sleepy | `neutral` share | Median chars | Tag rate |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | `noise.json` | 5 | 17 | 6 | 0 | 0 | 2 | 0 | 3 | 2 | 0 | 57% | 48 | 29/30 |
| 2 | `noise.json` | 6 | 16 | 6 | 0 | 0 | 3 | 0 | 2 | 2 | 1 | 53% | 40 | 29/30 |
| 3 | `/tmp/emotion-after.json` | 5 | 17 | 6 | 0 | 0 | 3 | 0 | 3 | 1 | 0 | 57% | 51 | 29/30 |

## What the breakdown adds to the summary

- **`curious` only ever appears with the old wording.** Both `curious` counts
  under the rewritten wording are zero across all three runs — the summary's
  "5, 5, 6" distinct count already implies something is consistently missing
  relative to "7, 7, 6", and this is which emotion it is. The rewritten
  wording did not just lose ground broadly; it lost `curious` specifically,
  every time.
- **`excited` and `surprised` are zero in every one of the six runs, both
  wordings.** This is the fact `RESUME.md` states as the real open question,
  and the breakdown is what confirms it is not an artifact of one bad run —
  it is six for six.
- **`happy`, `sad`, `confused` and `annoyed` are stable across both wordings**
  (roughly 6, 2-3, 2-3, 1-2 each run) — the rewrite's damage is concentrated
  in `neutral` swallowing `curious`, not a general flattening of every
  emotion.
- **The one leaked bracket in each of two runs** (old run 1, new run 1, both
  from `noise.json`) does not appear in the two `/tmp` "recorded" runs, which
  is why `RESUME.md`'s "brackets reaching spoken text: 0" line cites the
  recorded run specifically rather than claiming zero across all six.
