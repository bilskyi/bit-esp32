# Rebuild the frontend in the approved design — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Replace the current web UI with the design the client approved, and
make every section it shows either genuinely work or say honestly that it does
not exist yet. No fabricated data anywhere.

**The design is the spec.** `/private/tmp/claude-501/-Users-bilskyi-Documents-BIT/7cdb8248-97af-4214-bdb3-e6cad2936649/scratchpad/designs/e1-precision.html`
— a working self-contained page, iterated against browser screenshots and
approved after a subtraction pass. Lift its CSS, tokens, markup patterns and
Ukrainian copy. When this plan and that file disagree, the file wins on
appearance and this plan wins on what the data may claim.

**Architecture:** The existing React + Vite app keeps its shape — one socket
owned above the tabs, one fetch wrapper, one 401 convention. What changes is
the shell (seven sections instead of two tabs), the visual language, and two
new read endpoints.

## Global constraints

- **Never fabricate.** A figure appears only if the server really returns it.
  A section with no backend says so in the assistant's voice; it does not show
  plausible numbers. This overrides fidelity to the mockup.
- Type scale exactly 11/12/13/14/15/22px. Inter for everything, JetBrains Mono
  only for the prompt, filenames, scores and timestamps. Never Cyrillic prose
  in mono.
- Tokens from the mockup verbatim, both themes. A border means a genuinely
  separate object; otherwise group with an 11px label and a hairline.
- The device's pixel eyes come from `web/public/face-frames.json` via the
  existing `Face.tsx` path, at integer scale multiples only.
- `server/persona.py`'s `BASE` stays byte-identical; the golden test keeps
  passing. The ESP32's bearer path is untouched; no new frame reaches it.
- `uv run pytest` green (baseline **400**), `npm --prefix web test` green
  (baseline **35**), `npx tsc -b --force` and `npm run build` clean, oxlint no
  worse than its 3 known warnings.
- Every task ends with `npm --prefix web run shots` clean at five widths in
  both schemes.

## What is real, and what must say it is not

| Section | Backend today | This plan |
|---|---|---|
| Розмова | `/ws`, full | Real |
| Знання | `/memory/{id}` facts + instructions | Real. **Documents: honest "not yet"** — nothing ingests them |
| Характер | `/roles`, `/settings/surfaces` | Real |
| Журнал | `conversation_rows`, `usage_rows` exist, unexposed | Real, via two new endpoints (Task 5) |
| Пристрої | none | Partial: the persona the `esp32` surface runs, and that device's real usage rows. **No heap, uptime or firmware** — nothing reports them |
| Зʼєднання | none | Honest empty state, in her voice |
| Головна | composed of the above | Only what the above genuinely return |

---

## Task 1: The design system and the shell

**Files:** `web/src/tokens.css`, `web/src/app.css`, `web/src/Shell.tsx`, `web/src/App.tsx`

- [ ] Replace `tokens.css` with the mockup's tokens, both themes, complete in
  bare `:root` before any media/`[data-theme]` block.
- [ ] Rebuild `app.css` from the mockup's stylesheet: the `.grp` primitive,
  30px sidebar rows, 36px list rows, 4/8/12/16/24 spacing, 6px radii, the
  panel top-edge highlight, one header glow.
- [ ] `Shell.tsx`: sidebar with the seven sections and their live counts, the
  Поверхні group, the free-tier meter; a breadcrumb top bar with the status
  pill and theme toggle. Section state stays in React — **a client route whose
  first segment is an API prefix 404s on reload**, so no router.
- [ ] Sidebar collapses to a 7-item bottom bar below 900px.
- [ ] `⌘K` focuses the ask field, and `g` then `h/t/k/c/d/p/a` switches
  section. Do not claim a command palette that does not exist.
- [ ] Verify: build, tsc, shots at five widths both schemes.

## Task 2: Розмова, with the inspector

**Files:** `web/src/Chat.tsx`, `web/src/Message.tsx`, `web/src/Inspector.tsx`

- [ ] Restyle to the mockup. Keep `useTurn` and its cancel accounting
  untouched — that mechanism has been patched four times and is now sound.
- [ ] Inspector inline under the turn (the mockup's own conclusion: the
  retrieval table needs ~850px). Scores with meters, the assembled prompt in
  mono, the figures row. `traceStatus.ts` keeps deciding the no-trace copy.
- [ ] Keep speech-to-text shown as absent, never `0 ms`.
- [ ] Verify: `npm test` still 35, shots clean.

## Task 3: Знання

**Files:** `web/src/Knowledge.tsx` (new, replaces `Memory.tsx`)

- [ ] Three groups from `/memory/default`: facts it extracted, instructions
  you gave, and **Документи as an honest empty state** — nothing uploads them
  yet, and the copy says that plainly rather than showing a fake file list.
- [ ] Dates rendered per row (they are how a stranger's fact is spotted).
- [ ] Add a standing instruction; delete a row; both destructive controls
  behind typed confirmation with copy naming exactly what goes.
- [ ] Verify.

## Task 4: Характер, Зʼєднання, Пристрої

**Files:** `web/src/Personality.tsx`, `web/src/Connections.tsx`, `web/src/Devices.tsx`

- [ ] Характер: the roles list and editor, ported from `RoleEditor.tsx` with
  its patch-diff logic intact (`prompt: null` is the revert; explicit nulls on
  non-nullable fields are refused). Which surface uses which role.
- [ ] Зʼєднання: an honest empty state in her voice — nothing is connected,
  MCP is not wired, and what changes when it is.
- [ ] Пристрої: the one known `device_id`, the persona its surface runs, and
  its real usage rows. State plainly that heap, uptime and firmware are not
  reported by anything yet. An "add a device" affordance may explain how a
  device joins; it must not pretend to register one.
- [ ] Verify.

## Task 5: Журнал, and the two endpoints that make it real

**Files:** `server/main.py`, `tests/test_main.py`, `web/src/Journal.tsx`

- [ ] `GET /conversations/{device_id}` and `GET /usage/{device_id}`, both
  `require_login`, returning the existing `Store.conversation_rows` and
  `Store.usage_rows`. Tests: 401 without a login, correct shape with one,
  and that `DELETE /conversations/{id}` still works.
- [ ] Журнал: recorded conversations with surface and turn count, tokens
  against the free-tier ceiling, and a questions-per-day chart **drawn from
  real rows** — if there are too few days of data, say so instead of padding.
- [ ] Verify: pytest count rises and is explainable.

## Task 6: Головна

**Files:** `web/src/Home.tsx` (new)

- [ ] The greeting, her line, the figures line, the ask control, «Куди я
  дістаю», the one-line link to Журнал, and the small live screen.
- [ ] Every figure sourced from a real call. Where the mockup shows something
  the server cannot supply — document counts, fragment totals — either omit it
  or label it as not yet available.
- [ ] The waiting item about facts predating the owner's use is **real**:
  compute it from `created_at` on the facts the server returns. If none
  qualify, the item does not appear.
- [ ] Asking from Головна hands off to Розмова with the question sent.
- [ ] Verify, then delete `Settings.tsx` and any file no longer imported.

## Task 7: Ship it

- [ ] Full suite, both harnesses, shots at five widths in both schemes.
- [ ] `docker build`, then run the container and smoke-test as before.
- [ ] Deploy; check the **device** first, then the app; confirm the inspector
  fills on a real question.
- [ ] Update `README.md` and `RESUME.md`.
