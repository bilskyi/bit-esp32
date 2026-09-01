# Playground, slice 1 — chat, roles, mood, memory, inspector

## Goal

A logged-in web app, served by the existing Railway service, that makes the
assistant's behaviour **visible** and **configurable**: a streaming chat
playground with a per-message inspector, named roles switchable per surface,
mood display and pinning, and the memory editor the `/memory` page was
trying to be. It also begins **recording conversations** — storage only, no
UI — so slice 3's dashboard has real history to show the day it is built
rather than starting from an empty table.

This is slice 1 of four. Document RAG is slice 2, the dashboard UI slice 3,
MCP connections slice 4. Each gets its own spec.

## Ground state, verified 1 Sep

- Branch `worktree-shared-identity-backend`: 12 commits, **288 tests
  passing**, **not merged and not deployed** — `/settings/style/web` returns
  404 in production, which is the proof.
- The live deployment **is** locked: unauthenticated requests get 401,
  `/healthz` reports `auth:true`. The bearer check has existed since 25 Aug;
  when the Railway variable was actually populated is not recoverable from
  git, and is presumably how the traffic below got in.
- The live volume holds facts from **other people** (`RESUME.md`, 29 Aug:
  *"none of them mine"*). They are still there, still retrieved into "What
  you remember about this person."
- **No transcript of anything has ever been stored.** `session_usage` holds
  counters only: turns, audio seconds, tokens, TTS characters.

## Decisions, and why

**Merge the built backend before writing a line of frontend.** The React app
consumes `/login`, `/settings/...` and the text control frame. Building it
against an unmerged branch means either developing on the worktree or
mocking endpoints that already exist. The merge is the first task, and it is
a merge, not a rewrite — 288 green tests are the acceptance criterion.

**The device's prompt becomes editable. This reverses an earlier deliberate
constraint.** Today `PUT /settings/style/esp32` returns 400 and
`build_system_prompt` reaches for the measured `BASE` via an `is` identity
check, because that wording was measured and a reword made the emotion
spread worse on three runs (`RESUME.md`). That protection was right when
nothing could edit prompts safely; it is wrong for an app whose entire
purpose is configuring the assistant. So: the measured `BASE` stays the
**default** and stays byte-identical in the repo, but a role may override
the device surface behind an explicit control that says the wording was
measured, with one-click revert to the measured default. The safeguard moves
from "impossible" to "reversible and labelled" — and
`scripts/emotion_survey.py` already exists to re-measure the cost of any
override.

**Roles are rows; a surface points at one.** A role is a name plus the
knobs: system prompt, max sentences, markdown allowed, permitted languages,
and an optional pinned mood. A role's permitted languages may only ever be
a **subset of Ukrainian, Russian and English**: `lang.VOICES` has entries
for those three and edge-tts emits silence for anything else, so a role
offering a fourth language would produce a reply nobody can hear. The UI
presents three checkboxes, not a free-text field, and the API rejects any
other code. `surface_roles` maps `esp32` and `web` each to
one active role, so the device can run "Terse" while the laptop runs
"Coach". The unmerged `style_overrides` table is replaced rather than
extended — it exists only on an unmerged branch, holds at most one row, and
has never been deployed, so there is nothing to migrate and no reason to
carry two overlapping shapes. Concretely: the `style_overrides` table, its
`set_style_override`/`_resolve_style` helpers and the `GET/PUT
/settings/style/{surface}` endpoints are **deleted**, not deprecated
alongside the new ones, and the tests covering them are rewritten against
roles rather than left asserting a shape that no longer exists.

**Mood is always shown; pinning is opt-in per role.** The `emotion` frame
already streams per reply, so display is wiring, not new machinery. Pinning
adds one line to the system prompt and forces the frame's value, which is
what makes it a playground rather than a spectator seat — you can hear what
`[sleepy]` does to a reply and watch the device's face follow.

**The inspector is a new frame, and the ESP32 never sees it.** A `trace`
frame carries what the turn actually did: retrieved facts with their cosine
scores, the assembled prompt, prompt tokens, and per-stage latency. It is
emitted only when the connection authorised as `web`. The device is on a
metered radio with a 55 KB free-heap margin and no use for the payload;
`_authorise_connection` already returns the surface, so the gate is one
condition, not a parallel code path.

**`relevant_facts` returns scores.** It currently returns `list[str]` and
throws the similarity away after sorting. It will return `list[tuple[str,
float]]` and callers will map to text where they only need text. One
method, not a scored sibling next to an unscored one — two implementations
of "rank the facts" would drift, the same argument that put spoken and typed
questions through one `_answer`.

**Conversations are stored by default, with retention and an off switch.**
Recording is what makes slice 3 possible, and starting now means history
exists by then. It is also personal data, including other people's, so:
`store_conversations` defaults on but is switchable from Settings,
`retention_days` defaults to 90, and expired rows are purged at startup and
after each session ends. Facts extraction is unaffected — it already runs
on session end and does not read this table.

**One page per concern.** Login, Chat, Settings. Dashboard and Connections
are not stubbed, not linked, not present — an empty nav item that does
nothing is worse than an absent one.

## Architecture

One Railway service, one process, unchanged in shape. FastAPI serves the WS
protocol (ESP32 untouched), the JSON API (bearer *or* cookie), and now a
built React bundle at `/`. Same origin throughout, so the session cookie
needs no CORS and no cross-site flags.

The static mount is registered **last**, after `/ws`, `/login`, `/memory`,
`/settings` and `/healthz`, with an SPA fallback that returns `index.html`
for unmatched GETs so client-side routes survive a refresh. Anything that
looks like an API path and is not one keeps returning 404 rather than HTML.

Chat rides the pipeline that already exists: `on_text` skips STT and lands
in `_answer`, the same place transcription lands. The inspector is fed from
inside `_answer`, which is the one place that knows the facts, the prompt
and the timings — so there is nothing to keep in sync.

## What's new

- **`server/roles.py`** — `Role`, `Roles` (CRUD + `active_for(surface)`),
  and `resolve_prompt(role, facts)`. Prompt assembly stays in
  `persona.py`; this module owns storage and selection only.
- **`server/memory/store.py`** — `conversations` and `messages` tables, an
  `app_settings` key/value table for `store_conversations` and
  `retention_days`, `purge_expired()`, and `relevant_facts` returning
  scores.
- **`server/persona.py`** — `build_system_prompt` takes a `Role`; `BASE`
  unchanged, byte for byte; a pinned mood appends one rule; the `is ESP32`
  identity check gives way to "this role has no prompt override, so use
  `BASE`".
- **`server/session.py`** — `_answer` records the turn, emits `trace` when
  `surface == "web"`, and honours a pinned mood.
- **`server/main.py`** — `GET/POST/PUT/DELETE /roles`, `PUT
  /settings/surface/{surface}` (which role is active), `GET/PUT
  /settings/app` (recording + retention), the static mount and SPA
  fallback. `GET /memory`'s hand-written HTML page is deleted.
- **`web/`** — React + Vite, mobile-first: Login, Chat (streaming replies,
  mic button, per-message collapsible inspector), Settings (roles editor,
  active role per surface, mood pinning, memory list/add/delete, recording
  and retention).
- **`Dockerfile`** — replaces the nixpacks builder. A Node stage builds the
  bundle, a Python stage installs and runs. Nixpacks has to be coaxed into
  a dual-runtime build; a two-stage Dockerfile is the predictable version of
  the same thing, and `railway.toml` loses `builder = "nixpacks"` while
  keeping the 120 s healthcheck the embedding model needs.

## Error handling

Wrong password: 401, one generic message. Expired cookie mid-use: the next
call 401s and the app returns to login. A role deleted while active on a
surface: that surface falls back to the built-in default rather than
erroring, and deleting the last remaining role is refused. An `esp32`
override saved and later reverted restores the measured `BASE` exactly. A
typed message arriving mid-reply is an interrupt through the same path the
device's button uses. Recording switched off mid-session stops writing
immediately and leaves existing rows alone. A `trace` frame is best-effort:
if assembling it raises, the reply still goes out and the failure is logged.

## Testing

Backend, in this project's existing shape: `tests/test_roles.py` for CRUD,
active-role resolution, fallback on delete, and that a role with no prompt
override yields `BASE` byte-identically; `tests/test_store.py` additions for
conversation writes, retention purging, and scored retrieval;
`tests/test_main.py` additions for the roles and settings endpoints behind
`require_login`, the SPA fallback not shadowing API 404s, and that a `trace`
frame reaches a cookie-authorised socket and never a bearer-authorised one.

**What no test here covers: the React app in a real browser.** There is no
browser tool in this environment. Automated coverage stops at the API; the
login flow, streaming render, inspector layout and mic permission need a
person driving it, the same way `voice_main.c` needs the bench.

## Deployment

`SESSION_SECRET_KEY` must exist as a Railway variable before this deploys —
same category as `DEVICE_TOKEN`. The builder changes from nixpacks to the
Dockerfile. After the first deploy: create the account with
`scripts/create_account.py`, log in, open Settings → memory, and delete the
facts that belong to other people. That cleanup is the reason the memory
editor is in slice 1 rather than being deferred with the rest of the admin
surface.

## Deliberately out of scope

Document RAG (slice 2) — no upload control, not even disabled. The dashboard
UI (slice 3) — this slice records, it does not chart. MCP and tool-calling
(slice 4). Multi-user accounts: one account, one `device_id`; per-person
separation arrives with slice 3, and the deployment lock is what holds the
line until then. Real-time cross-surface conversation sync. Passkeys. A PWA
shell.

## Order of work

1. Merge `worktree-shared-identity-backend` to main; 288 tests green.
2. Roles and mood: `server/roles.py`, `persona.py`, the endpoints. Pure
   backend, fully testable.
3. Scored retrieval and the `trace` frame.
4. Conversation storage, retention, purge.
5. The React app.
6. Dockerfile, `SESSION_SECRET_KEY`, deploy, then the memory cleanup.

Steps 2-4 are independent of the frontend and ship value even if step 5
slips. Step 1 blocks everything.

**This spec produces two implementation plans, not one:** steps 1-4 as a
backend plan, step 5-6 as a frontend plan written against the endpoints the
first one delivers. A single plan spanning SQLAlchemy tables and React
components would be too large to execute or review well, and the frontend's
tasks cannot be written precisely until the `trace` frame's exact shape
exists in code.

## One thing to schedule, not to do here

Slice 2 puts other people's chat exports into an LLM prompt. This repo has a
data-governance reviewer (`ai-team-assistant:security-reviewer`) for exactly
that question. It belongs at the front of slice 2, not retrofitted after
uploads work.
