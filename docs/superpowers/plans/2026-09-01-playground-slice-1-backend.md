# Playground Slice 1, Backend Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the server everything the playground UI will consume — named
roles per surface, a pinnable mood, retrieval that reports its own scores, a
web-only `trace` frame carrying what each turn actually did, and recorded
conversations with retention — with the ESP32's measured prompt provably
unchanged.

**Architecture:** Land the already-built `worktree-shared-identity-backend`
branch on main first, then add one new module (`server/roles.py`, following
`server/accounts.py`'s pattern of its own tables in the same SQLite file),
refactor `server/persona.py` from one frozen string into parts assembled
from a `Role`, and extend `server/memory/store.py` and `server/session.py`.
No new runtime dependency.

**Tech Stack:** FastAPI, SQLAlchemy 2.0 async, aiosqlite, pytest with
`fastapi.testclient.TestClient`, `uv` for everything.

## Global Constraints

- `server/persona.py`'s `BASE` string stays in the file **byte-identical**
  and becomes a golden reference a test asserts against. No task may edit it.
- A role's languages may only ever be a subset of `("uk", "ru", "en")`.
  `server/lang.py`'s `VOICES` has those three; edge-tts emits silence for
  anything else. The API rejects any other code.
- The emotion-tag rule and the language restriction are **invariants**: they
  are appended by `persona.py` on every prompt, including when a role
  supplies a custom persona section. A user-authored prompt must not be able
  to break the device's face or make TTS emit silence.
- The `trace` frame goes only to a connection whose surface is `"web"`.
  The ESP32 never receives it.
- `store_conversations` defaults to **on**; `retention_days` defaults to
  **90**.
- The `DEVICE_TOKEN` bearer path is untouched. No task modifies `_token_ok`.
- `device_id` stays `"default"`. No `User -> devices` table in this plan.
- No new runtime dependency. `bcrypt` (arriving with the merge) is the last
  one added.
- Every new endpoint is gated by `require_login`, never `require_token` —
  these are for the person, not the device.
- `uv run pytest` must be green at the end of every task. Task 1 lands 288
  tests; no later task may reduce that count.

## File Structure

| File | Responsibility |
|---|---|
| `server/roles.py` | **New.** `roles` and `surface_roles` tables, `Role` dataclass, CRUD, which role a surface is using. Owns rows, not wording. |
| `server/persona.py` | Assembles a system prompt from a `Role`. Owns wording, not rows. `BASE` becomes a golden constant. |
| `server/memory/store.py` | Adds `conversations`, `messages`, `app_settings`; `relevant_facts` returns scores; drops `style_overrides`. |
| `server/session.py` | Resolves nothing itself — receives a `Role` and a surface, forces a pinned mood, emits `trace`, records turns. |
| `server/main.py` | Roles/settings endpoints, role resolution per surface, purge on startup. Drops `/settings/style/{surface}`. |
| `tests/test_roles.py` | **New.** Storage, selection, fallback, language validation. |
| `tests/test_persona.py` | Extended: the golden `BASE` assertion, invariants, pinned mood. |
| `tests/test_store.py` | Extended: scored retrieval, conversation writes, retention. |
| `tests/test_session.py` | Extended: pinned mood forced, `trace` emitted, turns recorded. |
| `tests/test_main.py` | Extended: roles/settings endpoints, auth gating, `trace` never to a bearer socket. |
| `tests/fakes.py` | `FakeStore`/`FakeRoles` kept in step with the real signatures. |

---

## Task 1: Land the built backend on main

The branch `worktree-shared-identity-backend` holds 12 commits and 288
passing tests and has never been merged or deployed. Main's working tree
also holds **uncommitted** work from a different investigation — the
twenty-second stall fix — touching exactly the three files the merge also
touches (`server/session.py`, `tests/test_main.py`, `tests/test_session.py`).
That work passes: 249 tests green with it in place. It gets committed on its
own before the merge, so a conflict resolution never has to disentangle two
unrelated changes.

The uncommitted `firmware/main/provision.c` and
`firmware/main/voice_main.c` changes do **not** overlap the merge (the
branch touches no firmware) and are left alone by this task.

**Files:**
- Commit: `server/session.py`, `tests/test_session.py`, `tests/test_main.py`, `RESUME.md`
- Merge: `worktree-shared-identity-backend` into `main`

**Interfaces:**
- Produces: everything later tasks build on — `server/accounts.py`,
  `Session(..., style=ESP32)`, `Session.on_text(text)`,
  `Store.get_style_override/set_style_override`, `_authorise_connection(ws,
  settings) -> str | None` returning `"esp32"` or `"web"`,
  `require_login`, `require_token_or_login`, `Settings.session_secret_key`,
  `Settings.session_cookie_secure`.

- [ ] **Step 1: Confirm the stall work is green before committing it**

Run: `uv run pytest -q`
Expected: `249 passed`. If anything fails, stop — do not commit a red tree,
and do not start the merge.

- [ ] **Step 2: Commit the stall fix, server side only**

```bash
git add server/session.py tests/test_session.py tests/test_main.py RESUME.md
git commit -m "Start a fresh utterance when the device abandons one mid-listen

A second start with no end between them means the utterance in progress was
abandoned on the device's side and will never be ended. Returning early left
the session listening to a device that had stopped sending: every frame of
the new question landed in the old buffer, no reply was produced, and the
first sign of trouble was a 60 s timeout. Measured 28 Aug: start accepted
0.19 s after a cancel, 0.22 s of audio, no end, sixty seconds of nothing."
```

Leave `firmware/` dirty. It is a separate concern and does not block the
merge.

- [ ] **Step 3: Verify the branch is green in its own worktree**

Run: `cd .claude/worktrees/shared-identity-backend && uv run pytest -q && cd -`
Expected: `288 passed`.

- [ ] **Step 4: Merge**

```bash
git merge worktree-shared-identity-backend
```

Expect conflicts in `server/session.py`, `tests/test_session.py` and
`tests/test_main.py` — both sides added code to the same files. Resolution
rule: **keep both sides.** The branch adds `on_text`, the `_respond`/
`_answer` split and a `style` parameter; the commit from Step 2 changes
`on_start`'s handling of a second `start`. They touch different methods.
In `session.py`, `on_start`'s `elif self.state is State.LISTENING:` block
from Step 2 stays, and `_answer`, `on_text` and `style=ESP32` from the
branch stay. In both test files, keep every test from both sides.

- [ ] **Step 5: Verify the merge kept both sides**

Run: `uv run pytest -q`
Expected: `288 passed` — the branch's 288 already include main's, because
the branch was cut from `372f5d5`, the same commit main's tests were written
against. Then confirm nothing was lost:

Run: `uv run pytest -q -k "second_start or abandons or on_text or login or accounts"`
Expected: PASS, and the list includes
`test_a_second_start_while_listening_begins_a_fresh_utterance` and
`test_an_utterance_the_device_abandons_does_not_deafen_the_session`. If
either is missing, the conflict resolution dropped main's side — redo Step 4.

- [ ] **Step 6: Commit the merge**

```bash
git commit --no-edit
```

If the merge auto-committed, skip. Then:

Run: `git log --oneline -3 | cat`
Expected: a merge commit on top.

- [ ] **Step 7: Release the worktree**

```bash
git worktree unlock .claude/worktrees/shared-identity-backend
git worktree remove .claude/worktrees/shared-identity-backend
```

Expected: no error. The branch is merged, so the worktree is now a duplicate
that can only drift. If `remove` refuses because of an untracked build
artifact, inspect it, then re-run with `--force`.

---

## Task 2: Roles storage

**Files:**
- Create: `server/roles.py`
- Create: `tests/test_roles.py`

**Interfaces:**
- Produces:
  - `SPEAKABLE_LANGUAGES = ("uk", "ru", "en")`
  - `Role` — frozen dataclass: `id: int | None`, `name: str`,
    `prompt: str | None`, `max_sentences: int`, `markdown_allowed: bool`,
    `languages: tuple[str, ...]`, `pinned_mood: str | None`
  - `DEVICE_DEFAULT: Role` and `WEB_DEFAULT: Role` — the built-in roles,
    `prompt=None`, ids `None`
  - `Roles(url: str)` with `async init()`, `async close()`,
    `async ensure_defaults()`, `async all() -> list[Role]`,
    `async get(role_id: int) -> Role | None`,
    `async create(name, prompt, max_sentences, markdown_allowed, languages, pinned_mood) -> Role`,
    `async update(role_id, **fields) -> Role | None`,
    `async delete(role_id: int) -> bool`,
    `async active_for(surface: str) -> Role`,
    `async set_active(surface: str, role_id: int) -> bool`
- Consumes: nothing. This module imports no other project module, which is
  what keeps `persona.py` free to import `Role` without a cycle.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_roles.py`:

```python
import pytest

from server.roles import DEVICE_DEFAULT, WEB_DEFAULT, Roles


@pytest.fixture
async def roles(tmp_path):
    r = Roles(f"sqlite+aiosqlite:///{tmp_path}/test.db")
    await r.init()
    await r.ensure_defaults()
    yield r
    await r.close()


async def test_ensure_defaults_creates_one_role_per_surface(roles):
    names = sorted(role.name for role in await roles.all())
    assert names == ["Device default", "Web default"]


async def test_ensure_defaults_is_idempotent(roles):
    await roles.ensure_defaults()
    assert len(await roles.all()) == 2


async def test_the_device_surface_starts_on_the_device_default(roles):
    active = await roles.active_for("esp32")
    assert active.name == "Device default"
    assert active.prompt is None
    assert active.max_sentences == DEVICE_DEFAULT.max_sentences
    assert active.markdown_allowed is False


async def test_the_web_surface_starts_on_the_web_default(roles):
    active = await roles.active_for("web")
    assert active.name == "Web default"
    assert active.max_sentences == WEB_DEFAULT.max_sentences
    assert active.markdown_allowed is True


async def test_an_unknown_surface_falls_back_to_the_web_default(roles):
    active = await roles.active_for("carrier-pigeon")
    assert active.name == "Web default"


async def test_a_created_role_round_trips(roles):
    created = await roles.create(
        name="Coach",
        prompt="You are a blunt running coach.",
        max_sentences=3,
        markdown_allowed=False,
        languages=("uk", "en"),
        pinned_mood="excited",
    )
    fetched = await roles.get(created.id)
    assert fetched == created
    assert fetched.languages == ("uk", "en")
    assert fetched.pinned_mood == "excited"


async def test_creating_a_duplicate_name_is_refused(roles):
    await roles.create(name="Coach", prompt=None, max_sentences=2,
                       markdown_allowed=False, languages=("uk",), pinned_mood=None)
    with pytest.raises(ValueError, match="name"):
        await roles.create(name="Coach", prompt=None, max_sentences=4,
                           markdown_allowed=True, languages=("en",), pinned_mood=None)


async def test_a_language_outside_the_three_voices_is_refused(roles):
    with pytest.raises(ValueError, match="language"):
        await roles.create(name="Deutsch", prompt=None, max_sentences=2,
                           markdown_allowed=False, languages=("de",), pinned_mood=None)


async def test_an_empty_language_set_is_refused(roles):
    with pytest.raises(ValueError, match="language"):
        await roles.create(name="Silent", prompt=None, max_sentences=2,
                           markdown_allowed=False, languages=(), pinned_mood=None)


async def test_a_mood_outside_the_nine_faces_is_refused(roles):
    with pytest.raises(ValueError, match="mood"):
        await roles.create(name="Smug", prompt=None, max_sentences=2,
                           markdown_allowed=False, languages=("uk",), pinned_mood="smug")


async def test_update_changes_only_the_fields_given(roles):
    created = await roles.create(name="Coach", prompt="Blunt.", max_sentences=3,
                                 markdown_allowed=False, languages=("uk",), pinned_mood=None)
    updated = await roles.update(created.id, max_sentences=5)
    assert updated.max_sentences == 5
    assert updated.prompt == "Blunt."
    assert updated.name == "Coach"


async def test_update_validates_languages_too(roles):
    created = await roles.create(name="Coach", prompt=None, max_sentences=3,
                                 markdown_allowed=False, languages=("uk",), pinned_mood=None)
    with pytest.raises(ValueError, match="language"):
        await roles.update(created.id, languages=("fr",))


async def test_update_of_an_unknown_role_is_none(roles):
    assert await roles.update(999, max_sentences=2) is None


async def test_set_active_switches_the_surface(roles):
    created = await roles.create(name="Terse", prompt=None, max_sentences=1,
                                 markdown_allowed=False, languages=("uk",), pinned_mood=None)
    assert await roles.set_active("esp32", created.id) is True
    assert (await roles.active_for("esp32")).name == "Terse"


async def test_set_active_to_an_unknown_role_is_refused(roles):
    assert await roles.set_active("esp32", 999) is False
    assert (await roles.active_for("esp32")).name == "Device default"


async def test_deleting_the_active_role_falls_back_to_the_surface_default(roles):
    created = await roles.create(name="Terse", prompt=None, max_sentences=1,
                                 markdown_allowed=False, languages=("uk",), pinned_mood=None)
    await roles.set_active("esp32", created.id)
    assert await roles.delete(created.id) is True
    assert (await roles.active_for("esp32")).name == "Device default"


async def test_a_built_in_default_cannot_be_deleted(roles):
    device = next(r for r in await roles.all() if r.name == "Device default")
    with pytest.raises(ValueError, match="built-in"):
        await roles.delete(device.id)


async def test_deleting_an_unknown_role_is_false(roles):
    assert await roles.delete(999) is False


async def test_reverting_the_device_role_restores_the_measured_prompt(roles):
    device = next(r for r in await roles.all() if r.name == "Device default")
    await roles.update(device.id, prompt="You are a pirate.")
    assert (await roles.active_for("esp32")).prompt == "You are a pirate."
    await roles.update(device.id, prompt=None)
    assert (await roles.active_for("esp32")).prompt is None
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `uv run pytest tests/test_roles.py -q`
Expected: collection error — `ModuleNotFoundError: No module named 'server.roles'`.

- [ ] **Step 3: Write `server/roles.py`**

```python
"""Named roles: the assistant's configurable personality, per surface.

Its own module for the same reason server/accounts.py is one: Store holds
facts and usage, Accounts holds logins, this holds which roles exist and
which one each surface is using. Same SQLite file, its own tables.

Wording is deliberately *not* here. A Role is data - a persona section and
four knobs; turning it into a system prompt is a pure function in
server/persona.py. That split is what lets persona.py import Role without
this module importing persona back, and it is what makes the prompt
assembly testable without a database.
"""

from dataclasses import dataclass, replace
from datetime import datetime, timezone

from sqlalchemy import Boolean, DateTime, Integer, String, delete, select
from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker, create_async_engine
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column

# server/lang.py's VOICES has an edge-tts voice for exactly these three, and
# edge-tts emits silence for text in a language it has no voice for. A role
# may therefore narrow this set and must never extend it.
SPEAKABLE_LANGUAGES = ("uk", "ru", "en")

# Exactly the nine server/emotion.py enumerates, which are exactly the nine
# faces the firmware draws. A tenth would be logged and ignored by the
# device, so it is refused here instead.
MOODS = (
    "neutral", "happy", "excited", "curious", "confused",
    "surprised", "sad", "annoyed", "sleepy",
)

DEVICE_DEFAULT_NAME = "Device default"
WEB_DEFAULT_NAME = "Web default"
_BUILT_IN_NAMES = (DEVICE_DEFAULT_NAME, WEB_DEFAULT_NAME)


class Base(DeclarativeBase):
    pass


def _now() -> datetime:
    return datetime.now(timezone.utc)


@dataclass(frozen=True)
class Role:
    id: int | None
    name: str
    # None means "use the built-in persona section for this surface". It is
    # also what reverting a customised device role restores, which is why
    # the measured wording lives in persona.py as a constant rather than
    # being copied into this row.
    prompt: str | None
    max_sentences: int
    markdown_allowed: bool
    languages: tuple[str, ...]
    pinned_mood: str | None


DEVICE_DEFAULT = Role(
    id=None, name=DEVICE_DEFAULT_NAME, prompt=None, max_sentences=2,
    markdown_allowed=False, languages=SPEAKABLE_LANGUAGES, pinned_mood=None,
)
WEB_DEFAULT = Role(
    id=None, name=WEB_DEFAULT_NAME, prompt=None, max_sentences=6,
    markdown_allowed=True, languages=SPEAKABLE_LANGUAGES, pinned_mood=None,
)
_SURFACE_DEFAULTS = {"esp32": DEVICE_DEFAULT, "web": WEB_DEFAULT}


class RoleRow(Base):
    __tablename__ = "roles"
    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    name: Mapped[str] = mapped_column(String(64), unique=True)
    prompt: Mapped[str | None] = mapped_column(String(4000), nullable=True)
    max_sentences: Mapped[int] = mapped_column(Integer, default=2)
    markdown_allowed: Mapped[bool] = mapped_column(Boolean, default=False)
    # Stored as a comma-separated string, not a JSON column: the value is at
    # most "uk,ru,en" and SQLite has no array type worth the ceremony.
    languages: Mapped[str] = mapped_column(String(32), default="uk,ru,en")
    pinned_mood: Mapped[str | None] = mapped_column(String(16), nullable=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=_now)


class SurfaceRoleRow(Base):
    __tablename__ = "surface_roles"
    surface: Mapped[str] = mapped_column(String(16), primary_key=True)
    role_id: Mapped[int] = mapped_column(Integer)


def _validate(languages: tuple[str, ...] | None, pinned_mood: str | None) -> None:
    if languages is not None:
        if not languages:
            raise ValueError("a role needs at least one language")
        for code in languages:
            if code not in SPEAKABLE_LANGUAGES:
                raise ValueError(
                    f"language {code!r} has no voice; pick from {SPEAKABLE_LANGUAGES}"
                )
    if pinned_mood is not None and pinned_mood not in MOODS:
        raise ValueError(f"mood {pinned_mood!r} is not one of the nine faces")


def _to_role(row: RoleRow) -> Role:
    return Role(
        id=row.id,
        name=row.name,
        prompt=row.prompt,
        max_sentences=row.max_sentences,
        markdown_allowed=row.markdown_allowed,
        languages=tuple(c for c in row.languages.split(",") if c),
        pinned_mood=row.pinned_mood,
    )


class Roles:
    def __init__(self, url: str) -> None:
        self._engine = create_async_engine(url, future=True)
        self._session: async_sessionmaker[AsyncSession] = async_sessionmaker(
            self._engine, expire_on_commit=False
        )

    async def init(self) -> None:
        async with self._engine.begin() as conn:
            await conn.run_sync(Base.metadata.create_all)

    async def close(self) -> None:
        await self._engine.dispose()

    async def ensure_defaults(self) -> None:
        """Seed one role per surface, and point each surface at its own.

        Idempotent by name, so it is safe on every boot - which is where it
        is called from, because a database that predates roles must not need
        a migration step a person has to remember.
        """
        async with self._session() as s:
            for surface, default in _SURFACE_DEFAULTS.items():
                row = (
                    await s.scalars(select(RoleRow).where(RoleRow.name == default.name))
                ).first()
                if row is None:
                    row = RoleRow(
                        name=default.name,
                        prompt=default.prompt,
                        max_sentences=default.max_sentences,
                        markdown_allowed=default.markdown_allowed,
                        languages=",".join(default.languages),
                        pinned_mood=default.pinned_mood,
                    )
                    s.add(row)
                    await s.flush()
                if await s.get(SurfaceRoleRow, surface) is None:
                    s.add(SurfaceRoleRow(surface=surface, role_id=row.id))
            await s.commit()

    async def all(self) -> list[Role]:
        async with self._session() as s:
            rows = await s.scalars(select(RoleRow).order_by(RoleRow.id.asc()))
            return [_to_role(r) for r in rows.all()]

    async def get(self, role_id: int) -> Role | None:
        async with self._session() as s:
            row = await s.get(RoleRow, role_id)
            return _to_role(row) if row is not None else None

    async def create(
        self, name: str, prompt: str | None, max_sentences: int,
        markdown_allowed: bool, languages: tuple[str, ...], pinned_mood: str | None,
    ) -> Role:
        _validate(languages, pinned_mood)
        async with self._session() as s:
            clash = (await s.scalars(select(RoleRow).where(RoleRow.name == name))).first()
            if clash is not None:
                raise ValueError(f"a role name must be unique; {name!r} is taken")
            row = RoleRow(
                name=name, prompt=prompt, max_sentences=max_sentences,
                markdown_allowed=markdown_allowed, languages=",".join(languages),
                pinned_mood=pinned_mood,
            )
            s.add(row)
            await s.commit()
            await s.refresh(row)
            return _to_role(row)

    async def update(self, role_id: int, **fields) -> Role | None:
        """Change only the fields named. `prompt=None` is a real value here -
        it is how a customised device role is reverted to the measured
        wording - so absence, not None, means "leave alone".
        """
        _validate(fields.get("languages"), fields.get("pinned_mood"))
        async with self._session() as s:
            row = await s.get(RoleRow, role_id)
            if row is None:
                return None
            if "name" in fields:
                clash = (
                    await s.scalars(
                        select(RoleRow).where(
                            RoleRow.name == fields["name"], RoleRow.id != role_id
                        )
                    )
                ).first()
                if clash is not None:
                    raise ValueError(f"a role name must be unique; {fields['name']!r} is taken")
                row.name = fields["name"]
            if "prompt" in fields:
                row.prompt = fields["prompt"]
            if "max_sentences" in fields:
                row.max_sentences = fields["max_sentences"]
            if "markdown_allowed" in fields:
                row.markdown_allowed = fields["markdown_allowed"]
            if "languages" in fields:
                row.languages = ",".join(fields["languages"])
            if "pinned_mood" in fields:
                row.pinned_mood = fields["pinned_mood"]
            await s.commit()
            await s.refresh(row)
            return _to_role(row)

    async def delete(self, role_id: int) -> bool:
        async with self._session() as s:
            row = await s.get(RoleRow, role_id)
            if row is None:
                return False
            if row.name in _BUILT_IN_NAMES:
                raise ValueError(f"{row.name!r} is built-in and cannot be deleted")
            # Any surface pointing here loses its pointer rather than keeping
            # a dangling id; active_for then falls back to that surface's
            # default, which is what makes deleting an in-use role safe.
            await s.execute(delete(SurfaceRoleRow).where(SurfaceRoleRow.role_id == role_id))
            await s.delete(row)
            await s.commit()
            return True

    async def active_for(self, surface: str) -> Role:
        """The role this surface is using, or its built-in default.

        Never raises and never returns None: a missing pointer, a deleted
        role or a surface nobody has heard of all resolve to a usable role,
        because the alternative is a connection that cannot be answered.
        """
        default = _SURFACE_DEFAULTS.get(surface, WEB_DEFAULT)
        async with self._session() as s:
            pointer = await s.get(SurfaceRoleRow, surface)
            if pointer is None:
                named = (
                    await s.scalars(select(RoleRow).where(RoleRow.name == default.name))
                ).first()
                return _to_role(named) if named is not None else default
            row = await s.get(RoleRow, pointer.role_id)
            return _to_role(row) if row is not None else default

    async def set_active(self, surface: str, role_id: int) -> bool:
        async with self._session() as s:
            if await s.get(RoleRow, role_id) is None:
                return False
            pointer = await s.get(SurfaceRoleRow, surface)
            if pointer is None:
                s.add(SurfaceRoleRow(surface=surface, role_id=role_id))
            else:
                pointer.role_id = role_id
            await s.commit()
            return True
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `uv run pytest tests/test_roles.py -q`
Expected: `20 passed`.

- [ ] **Step 5: Run the whole suite**

Run: `uv run pytest -q`
Expected: `308 passed` (288 + 20). No failures elsewhere — nothing imports
this module yet.

- [ ] **Step 6: Commit**

```bash
git add server/roles.py tests/test_roles.py
git commit -m "Make the assistant's personality rows, not a constant

A role is a persona section plus four knobs, and a surface points at one, so
the device and the web app can run different personalities off the same
memory. Storage only: wording stays in persona.py, which is what keeps that
module importable from here without a cycle. Languages are validated against
the three edge-tts has voices for, since a fourth would be inaudible rather
than merely wrong."
```

---

## Task 3: A prompt assembled from a Role, with the measured wording pinned

`persona.py` currently holds one frozen string (`BASE`) for the device and a
generator (`_web_base`) for the browser, selected by `style is ESP32`. A role
with a custom persona section cannot be expressed that way. So the prompt
becomes parts assembled from a `Role`, and `BASE` stops being used to build
anything: it stays in the file, byte for byte, as the **golden reference** a
test asserts the assembly reproduces exactly. That is what preserves the
measurement (`RESUME.md`: rewording made the emotion spread worse on three
runs) while letting the wording be configured.

A role's `prompt` replaces **only** the opening "who you are" declaration.
The emotion-tag rule and the language restriction are always appended, so a
user-authored prompt can neither blind the device's face nor ask for a
language edge-tts has no voice for.

`spoken` is a property of the message, not the surface: a typed question gets
the on-screen wording, a spoken one gets the read-aloud wording, and markdown
is force-disabled whenever the reply will be spoken because TTS reads
asterisks out loud.

**Files:**
- Modify: `server/persona.py`
- Modify: `tests/test_persona.py`

**Interfaces:**
- Consumes: `Role`, `DEVICE_DEFAULT`, `WEB_DEFAULT` from `server/roles.py` (Task 2).
- Produces: `build_system_prompt(facts: list[str], role: Role = DEVICE_DEFAULT, *, spoken: bool = True) -> str`.
  `BASE` remains exported for the golden test. `Style`, `ESP32` and `WEB` are
  **removed** — Task 4 removes their last callers.

- [ ] **Step 1: Write the failing tests**

Replace the last five tests in `tests/test_persona.py` (everything from
`test_default_style_is_esp32_and_matches_existing_wording` onward — they
import `Style`, `ESP32` and `WEB`, which cease to exist) with:

```python
def test_the_device_default_reproduces_the_measured_prompt_exactly():
    """The golden test. BASE was measured (RESUME.md); an attempt to reword it
    made the emotion spread worse on three runs of emotion_survey.py. The
    assembly must reproduce it byte for byte, or the measurement no longer
    describes what ships."""
    from server.persona import BASE, build_system_prompt
    from server.roles import DEVICE_DEFAULT

    assert build_system_prompt([], DEVICE_DEFAULT, spoken=True) == BASE


def test_the_default_arguments_are_the_device_defaults():
    from server.persona import BASE, build_system_prompt

    assert build_system_prompt([]) == BASE


def test_a_screen_reply_allows_markdown_when_the_role_does():
    from server.persona import build_system_prompt
    from server.roles import WEB_DEFAULT

    prompt = build_system_prompt([], WEB_DEFAULT, spoken=False).lower()
    assert "markdown, lists and headings are fine" in prompt
    assert "up to 6 sentences" in prompt


def test_a_spoken_reply_never_allows_markdown_even_if_the_role_does():
    """TTS reads asterisks and hyphens out loud. Whatever the role says,
    a reply that will be spoken gets the no-markdown rule."""
    from server.persona import build_system_prompt
    from server.roles import WEB_DEFAULT

    prompt = build_system_prompt([], WEB_DEFAULT, spoken=True).lower()
    assert "no lists, no headings, no markdown" in prompt
    assert "markdown, lists and headings are fine" not in prompt


def test_a_custom_persona_section_replaces_only_the_opening():
    from server.emotion import EMOTIONS
    from server.persona import build_system_prompt
    from server.roles import Role, SPEAKABLE_LANGUAGES

    coach = Role(id=1, name="Coach", prompt="You are a blunt running coach.",
                 max_sentences=2, markdown_allowed=False,
                 languages=SPEAKABLE_LANGUAGES, pinned_mood=None)
    prompt = build_system_prompt([], coach, spoken=True)
    assert prompt.startswith("You are a blunt running coach.")
    assert "voice companion" not in prompt
    # The invariants survive a custom persona.
    for name in EMOTIONS:
        assert f"[{name}]" in prompt, name
    assert "Reply only in Ukrainian, Russian or English." in prompt


def test_narrowing_the_languages_narrows_the_rule():
    from server.persona import build_system_prompt
    from server.roles import Role

    ukrainian_only = Role(id=1, name="UA", prompt=None, max_sentences=2,
                          markdown_allowed=False, languages=("uk",), pinned_mood=None)
    prompt = build_system_prompt([], ukrainian_only, spoken=True)
    assert "Reply only in Ukrainian." in prompt
    assert "Russian" not in prompt.split("Reply only in", 1)[1].split("\n", 1)[0]


def test_two_languages_read_as_a_pair():
    from server.persona import build_system_prompt
    from server.roles import Role

    pair = Role(id=1, name="Pair", prompt=None, max_sentences=2,
                markdown_allowed=False, languages=("uk", "en"), pinned_mood=None)
    assert "Reply only in Ukrainian or English." in build_system_prompt([], pair, spoken=True)


def test_a_pinned_mood_becomes_a_rule():
    from server.persona import build_system_prompt
    from server.roles import Role, SPEAKABLE_LANGUAGES

    sleepy = Role(id=1, name="Sleepy", prompt=None, max_sentences=2,
                  markdown_allowed=False, languages=SPEAKABLE_LANGUAGES,
                  pinned_mood="sleepy")
    prompt = build_system_prompt([], sleepy, spoken=True)
    assert "[sleepy]" in prompt
    assert "you feel sleepy" in prompt.lower()


def test_no_pinned_mood_adds_no_mood_rule():
    from server.persona import BASE, build_system_prompt
    from server.roles import DEVICE_DEFAULT

    # "you feel" alone would be unsatisfiable: BASE's own tag rule says "how
    # you feel about it". The pinned-mood rule is what must be absent.
    assert "right now you feel" not in build_system_prompt([], DEVICE_DEFAULT, spoken=True).lower()
    assert build_system_prompt([], DEVICE_DEFAULT, spoken=True) == BASE


def test_one_sentence_reads_as_one_sentence():
    from server.persona import build_system_prompt
    from server.roles import Role, SPEAKABLE_LANGUAGES

    terse = Role(id=1, name="Terse", prompt=None, max_sentences=1,
                 markdown_allowed=False, languages=SPEAKABLE_LANGUAGES, pinned_mood=None)
    assert "Answer in one sentence." in build_system_prompt([], terse, spoken=False)
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `uv run pytest tests/test_persona.py -q`
Expected: FAIL — `TypeError: build_system_prompt() got an unexpected keyword
argument 'spoken'`, and `ImportError` for `SPEAKABLE_LANGUAGES` only if Task
2 was skipped.

- [ ] **Step 3: Rewrite `server/persona.py` below `BASE`**

Keep the module docstring and `BASE` **exactly as they are**. Delete the
`from dataclasses import dataclass` import, the `Style` dataclass, `ESP32`,
`WEB`, `_web_base` and the old `build_system_prompt`. Put the new import at
the **top** of the file, directly after the module docstring — ruff enforces
E402 in this project, and an import placed below `BASE` fails the lint:

```python
from server.roles import DEVICE_DEFAULT, Role
```

Then append, below `BASE`:

```python
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
```

- [ ] **Step 4: Run the golden test first, on its own**

Run: `uv run pytest tests/test_persona.py::test_the_device_default_reproduces_the_measured_prompt_exactly -q`
Expected: PASS. If it fails, print both strings and diff them — fix the
**assembly**, never `BASE`:

```bash
uv run python -c "
from server.persona import BASE, build_system_prompt
from server.roles import DEVICE_DEFAULT
import difflib
a = BASE.splitlines(); b = build_system_prompt([], DEVICE_DEFAULT, spoken=True).splitlines()
print('\n'.join(difflib.unified_diff(a, b, 'BASE', 'assembled', lineterm='')))
"
```

- [ ] **Step 5: Run the persona tests, then the whole suite**

Run: `uv run pytest tests/test_persona.py -q`
Expected: `18 passed`.

Run: `uv run pytest -q`
Expected: failures in `tests/test_main.py` and `tests/test_session.py` only,
from imports of `Style`/`ESP32`/`WEB`. Tasks 4 and 5 fix those. Do not patch
them here.

- [ ] **Step 6: Commit**

```bash
git add server/persona.py tests/test_persona.py
git commit -m "Assemble the prompt from a role, and pin the measured wording with a test

BASE stops building anything and becomes the golden string the assembly must
reproduce byte for byte, so the device's measured prompt is now protected by
a test rather than by being impossible to change. A role's prompt replaces
only the opening declaration: the emotion tag rule and the language
restriction are appended regardless, since one drives the face and the other
is a limit of the voices that exist. Markdown is force-disabled whenever the
reply will be spoken, because TTS reads asterisks aloud."
```

---

## Task 4: Roles reach the pipeline (API and session, one task)
**Two phases, one task, one review.** The endpoints and the session change
together because removing `Style` cannot be done in either file alone: the
suite is red between phase A and phase B by construction, which is why they
are not separate tasks. Commit at the end of each phase; the task's gate is
green tests after phase B.

### Phase A: the endpoints


Also deletes the `style_overrides` path this replaces: the table, its two
Store methods, `_resolve_style`, `StyleIn` and both `/settings/style/{surface}`
endpoints. They exist only on the branch merged in Task 1 and have never
been deployed, so there is nothing to migrate.

**Files:**
- Modify: `server/main.py`
- Modify: `server/memory/store.py`
- Modify: `tests/fakes.py`
- Modify: `tests/test_main.py`
- Modify: `tests/test_store.py`

**Interfaces:**
- Consumes: `Roles`, `Role`, `SPEAKABLE_LANGUAGES`, `MOODS` (Task 2).
- Produces:
  - `app.state.roles` — a `Roles` instance, injectable via
    `create_app(roles=...)`
  - `GET /roles` -> `[{"id","name","prompt","max_sentences","markdown_allowed","languages","pinned_mood"}]`
  - `POST /roles` -> `{"id": int}`; 409 on a duplicate name, 422 on a bad language or mood
  - `PUT /roles/{role_id}` -> `{"status":"ok"}`; 404 unknown, 409 duplicate name, 422 invalid
  - `DELETE /roles/{role_id}` -> `{"status":"ok"}`; 404 unknown, 409 built-in
  - `GET /settings/surfaces` -> `{"esp32": {...role...}, "web": {...role...}}`
  - `PUT /settings/surfaces/{surface}` body `{"role_id": int}` -> `{"status":"ok"}`; 404 unknown role
  - all six gated by `require_login`

- [ ] **Step 1: Write the failing tests**

In `tests/test_main.py`, delete `from server.persona import BASE` if it is
only used by the style tests you are about to remove, delete every test
mentioning `/settings/style`, and add:

```python
# ---------------------------------------------------------------- roles API

def _login(c):
    r = c.post("/login", json={"username": "test", "password": "test123"})
    assert r.status_code == 200
    return c


def test_roles_require_login():
    with client() as c:
        assert c.get("/roles").status_code == 401
        assert c.post("/roles", json={"name": "x"}).status_code == 401
        assert c.put("/roles/1", json={"max_sentences": 2}).status_code == 401
        assert c.delete("/roles/1").status_code == 401
        assert c.get("/settings/surfaces").status_code == 401
        assert c.put("/settings/surfaces/web", json={"role_id": 1}).status_code == 401


def test_roles_lists_the_two_defaults_once_logged_in():
    with client() as c:
        names = [r["name"] for r in _login(c).get("/roles").json()]
    assert sorted(names) == ["Device default", "Web default"]


def test_creating_a_role_then_listing_it():
    with client() as c:
        _login(c)
        created = c.post("/roles", json={
            "name": "Coach", "prompt": "You are a blunt coach.", "max_sentences": 3,
            "markdown_allowed": False, "languages": ["uk", "en"], "pinned_mood": "excited",
        })
        assert created.status_code == 200
        role = next(r for r in c.get("/roles").json() if r["name"] == "Coach")
    assert role["id"] == created.json()["id"]
    assert role["languages"] == ["uk", "en"]
    assert role["pinned_mood"] == "excited"


def test_a_duplicate_role_name_is_a_conflict():
    with client() as c:
        _login(c)
        body = {"name": "Coach", "prompt": None, "max_sentences": 2,
                "markdown_allowed": False, "languages": ["uk"], "pinned_mood": None}
        assert c.post("/roles", json=body).status_code == 200
        assert c.post("/roles", json=body).status_code == 409


def test_a_language_with_no_voice_is_rejected():
    with client() as c:
        _login(c)
        r = c.post("/roles", json={"name": "DE", "prompt": None, "max_sentences": 2,
                                   "markdown_allowed": False, "languages": ["de"],
                                   "pinned_mood": None})
    assert r.status_code == 422


def test_a_mood_that_is_not_a_face_is_rejected():
    with client() as c:
        _login(c)
        r = c.post("/roles", json={"name": "Smug", "prompt": None, "max_sentences": 2,
                                   "markdown_allowed": False, "languages": ["uk"],
                                   "pinned_mood": "smug"})
    assert r.status_code == 422


def test_updating_a_role_changes_only_what_was_sent():
    with client() as c:
        _login(c)
        role_id = c.post("/roles", json={"name": "Coach", "prompt": "Blunt.",
                                         "max_sentences": 3, "markdown_allowed": False,
                                         "languages": ["uk"], "pinned_mood": None}).json()["id"]
        assert c.put(f"/roles/{role_id}", json={"max_sentences": 5}).status_code == 200
        role = next(r for r in c.get("/roles").json() if r["id"] == role_id)
    assert role["max_sentences"] == 5 and role["prompt"] == "Blunt."


def test_clearing_a_prompt_reverts_to_the_built_in_wording():
    """prompt: null is a real value, not an omission - it is the revert."""
    with client() as c:
        _login(c)
        device = next(r for r in c.get("/roles").json() if r["name"] == "Device default")
        c.put(f"/roles/{device['id']}", json={"prompt": "You are a pirate."})
        assert next(r for r in c.get("/roles").json()
                    if r["id"] == device["id"])["prompt"] == "You are a pirate."
        c.put(f"/roles/{device['id']}", json={"prompt": None})
        assert next(r for r in c.get("/roles").json()
                    if r["id"] == device["id"])["prompt"] is None


def test_updating_an_unknown_role_is_404():
    with client() as c:
        assert _login(c).put("/roles/999", json={"max_sentences": 2}).status_code == 404


def test_a_built_in_role_cannot_be_deleted():
    with client() as c:
        _login(c)
        device = next(r for r in c.get("/roles").json() if r["name"] == "Device default")
        assert c.delete(f"/roles/{device['id']}").status_code == 409


def test_switching_the_active_role_for_a_surface():
    with client() as c:
        _login(c)
        role_id = c.post("/roles", json={"name": "Terse", "prompt": None, "max_sentences": 1,
                                         "markdown_allowed": False, "languages": ["uk"],
                                         "pinned_mood": None}).json()["id"]
        assert c.put("/settings/surfaces/esp32", json={"role_id": role_id}).status_code == 200
        surfaces = c.get("/settings/surfaces").json()
    assert surfaces["esp32"]["name"] == "Terse"
    assert surfaces["web"]["name"] == "Web default"


def test_switching_to_an_unknown_role_is_404():
    with client() as c:
        assert _login(c).put("/settings/surfaces/web",
                             json={"role_id": 999}).status_code == 404


def test_deleting_the_active_role_returns_the_surface_to_its_default():
    with client() as c:
        _login(c)
        role_id = c.post("/roles", json={"name": "Terse", "prompt": None, "max_sentences": 1,
                                         "markdown_allowed": False, "languages": ["uk"],
                                         "pinned_mood": None}).json()["id"]
        c.put("/settings/surfaces/esp32", json={"role_id": role_id})
        assert c.delete(f"/roles/{role_id}").status_code == 200
        surfaces = c.get("/settings/surfaces").json()
    assert surfaces["esp32"]["name"] == "Device default"
```

`client()` must now build a real `Roles` on a temp file, since `FakeStore`
never held roles. Change the helper at the top of `tests/test_main.py`:

```python
@contextmanager
def client(store=None, accounts=None, roles=None, **kw):
    kw.setdefault("session_cookie_secure", False)
    import tempfile
    from server.roles import Roles

    tmp = tempfile.TemporaryDirectory()
    real_roles = roles
    if real_roles is None:
        real_roles = Roles(f"sqlite+aiosqlite:///{tmp.name}/roles.db")
    app = create_app(
        settings=Settings(_env_file=None, **kw),
        stt=FakeSTT(),
        llm=FakeLLM("Все добре."),
        tts=FakeTTS(),
        store=store or FakeStore(),
        embedder=FakeEmbedder(),
        accounts=accounts or FakeAccounts(),
        roles=real_roles,
    )
    try:
        with TestClient(app) as c:
            yield c
    finally:
        tmp.cleanup()
```

In `tests/test_store.py`, delete the two tests covering
`get_style_override`/`set_style_override`.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `uv run pytest tests/test_main.py -q`
Expected: FAIL — `ImportError` on `Style`/`ESP32` from `server.persona`, then
404s on `/roles`.

- [ ] **Step 3: Delete the style-override path**

In `server/memory/store.py`, delete the `StyleOverride` class and the
`get_style_override` and `set_style_override` methods. Remove `Boolean` from
the `sqlalchemy` import if nothing else uses it.

In `server/main.py`, delete `_resolve_style`, the `StyleIn` model, both
`/settings/style/{surface}` routes, and change the persona import from
`from server.persona import ESP32, WEB, Style` to nothing — `main.py` no
longer needs it.

- [ ] **Step 4: Add Roles to the app and the endpoints**

In `server/main.py`, add to the imports:

```python
from server.roles import MOODS, SPEAKABLE_LANGUAGES, Roles
```

Add the request models next to `MemoryIn`:

```python
class RoleIn(BaseModel):
    name: str
    prompt: str | None = None
    max_sentences: int = 2
    markdown_allowed: bool = False
    languages: list[str] = list(SPEAKABLE_LANGUAGES)
    pinned_mood: str | None = None


class RolePatch(BaseModel):
    """Every field optional, and `prompt: null` means "revert to built-in".

    model_fields_set is what separates "not sent" from "sent as null", which
    is the whole reason this is a second model rather than RoleIn with
    defaults.
    """
    name: str | None = None
    prompt: str | None = None
    max_sentences: int | None = None
    markdown_allowed: bool | None = None
    languages: list[str] | None = None
    pinned_mood: str | None = None


class ActiveRoleIn(BaseModel):
    role_id: int
```

Extend `create_app`'s signature with `roles=None` and add to the lifespan,
right after the `accounts` block:

```python
        app.state.roles = roles
        if not roles_injected:
            app.state.roles = Roles(f"sqlite+aiosqlite:///{settings.db_path}")
        await app.state.roles.init()
        await app.state.roles.ensure_defaults()
```

with `roles_injected = roles is not None` next to `accounts_injected`, and in
the shutdown half:

```python
        if not roles_injected and app.state.roles is not None:
            await app.state.roles.close()
```

`init()` and `ensure_defaults()` run for an injected instance too — seeding
is idempotent and a test that injects a bare `Roles` still needs its tables.

Add a serialiser above `create_app`:

```python
def _role_json(role) -> dict:
    return {
        "id": role.id,
        "name": role.name,
        "prompt": role.prompt,
        "max_sentences": role.max_sentences,
        "markdown_allowed": role.markdown_allowed,
        "languages": list(role.languages),
        "pinned_mood": role.pinned_mood,
    }
```

Then the routes, after the memory routes:

```python
    @app.get("/roles", dependencies=[Depends(require_login)])
    async def list_roles() -> list[dict]:
        return [_role_json(r) for r in await app.state.roles.all()]

    @app.post("/roles", dependencies=[Depends(require_login)])
    async def create_role(body: RoleIn) -> dict:
        try:
            role = await app.state.roles.create(
                name=body.name, prompt=body.prompt, max_sentences=body.max_sentences,
                markdown_allowed=body.markdown_allowed,
                languages=tuple(body.languages), pinned_mood=body.pinned_mood,
            )
        except ValueError as exc:
            # A taken name is a conflict; a language with no voice or a mood
            # that is not a face is an unprocessable value.
            raise HTTPException(status_code=409 if "taken" in str(exc) else 422,
                                detail=str(exc)) from exc
        return {"id": role.id}

    @app.put("/roles/{role_id}", dependencies=[Depends(require_login)])
    async def update_role(role_id: int, body: RolePatch) -> dict:
        fields = {}
        for name in body.model_fields_set:
            value = getattr(body, name)
            fields[name] = tuple(value) if name == "languages" and value is not None else value
        try:
            role = await app.state.roles.update(role_id, **fields)
        except ValueError as exc:
            raise HTTPException(status_code=409 if "taken" in str(exc) else 422,
                                detail=str(exc)) from exc
        if role is None:
            raise HTTPException(status_code=404, detail="not found")
        return {"status": "ok"}

    @app.delete("/roles/{role_id}", dependencies=[Depends(require_login)])
    async def delete_role(role_id: int) -> dict:
        try:
            deleted = await app.state.roles.delete(role_id)
        except ValueError as exc:
            raise HTTPException(status_code=409, detail=str(exc)) from exc
        if not deleted:
            raise HTTPException(status_code=404, detail="not found")
        return {"status": "ok"}

    @app.get("/settings/surfaces", dependencies=[Depends(require_login)])
    async def get_surfaces() -> dict:
        return {
            surface: _role_json(await app.state.roles.active_for(surface))
            for surface in ("esp32", "web")
        }

    @app.put("/settings/surfaces/{surface}", dependencies=[Depends(require_login)])
    async def set_surface_role(surface: str, body: ActiveRoleIn) -> dict:
        if not await app.state.roles.set_active(surface, body.role_id):
            raise HTTPException(status_code=404, detail="not found")
        return {"status": "ok"}
```

In `ws_endpoint`, replace `style = await _resolve_style(app.state.store,
surface)` with:

```python
        role = await app.state.roles.active_for(surface)
```

and pass `role=role, surface=surface` to `Session(...)` instead of
`style=style`. `Session` accepts them in phase B; until then the suite has
one known failure, which Step 5 confirms is the only one.

- [ ] **Step 5: Run the tests**

Run: `uv run pytest tests/test_main.py -q`
Expected: the roles tests PASS; the WebSocket tests FAIL with
`TypeError: Session.__init__() got an unexpected keyword argument 'role'`.
Expected and temporary — phase B below is what clears it. Do not stop here
and do not paper over it by keeping a `style` parameter alive.

Run: `uv run pytest tests/test_roles.py tests/test_persona.py tests/test_store.py -q`
Expected: all PASS.

- [ ] **Step 6: Commit**

```bash
git add server/main.py server/memory/store.py tests/
git commit -m "Put roles behind the login, and retire style_overrides

Six endpoints for what the settings page needs: list, create, update,
delete, read which role each surface is using, and switch it. prompt: null
is a real value on the patch model, because reverting a customised device
role to the measured wording is the one edit that has to be expressible.
style_overrides goes: it never left the branch it was written on, so there
is nothing to migrate and no reason to keep two shapes for one idea."
```

---


### Phase B: the session obeys its role


**Files:**
- Modify: `server/session.py`
- Modify: `tests/test_session.py`

**Interfaces:**
- Consumes: `Role`, `DEVICE_DEFAULT`, `WEB_DEFAULT` (Task 2);
  `build_system_prompt(facts, role, *, spoken)` (Task 3); `role=`/`surface=`
  passed by `ws_endpoint` (Task 4).
- Produces: `Session(..., role: Role = DEVICE_DEFAULT, surface: str = "esp32")`.
  `self.style` is gone. `self.role` and `self.surface` are read by Tasks 6
  and 7.

- [ ] **Step 7: Write the failing tests**

In `tests/test_session.py`, replace the three tests `test_default_style_is_esp32`,
`test_web_style_reaches_the_system_prompt` and
`test_a_style_shaped_like_esp32_defaults_still_uses_base` with:

```python
async def test_default_role_is_the_device_default():
    from server.roles import DEVICE_DEFAULT

    session, _ = build()
    assert session.role is DEVICE_DEFAULT
    assert session.surface == "esp32"


async def test_a_typed_question_gets_the_screen_wording():
    from server.roles import WEB_DEFAULT

    llm = FakeLLM()
    session = Session(
        transport=FakeTransport(), stt=FakeSTT(), llm=llm, tts=FakeTTS(),
        settings=Settings(_env_file=None), embedder=FakeEmbedder(),
        role=WEB_DEFAULT, surface="web",
    )
    await session.on_text("Як справи?")
    await session.wait_for_reply()
    assert "markdown, lists and headings are fine" in llm.prompts[0][0]["content"].lower()


async def test_a_spoken_question_never_gets_markdown_permission():
    """Same role, spoken instead of typed: TTS would read the asterisks."""
    from server.roles import WEB_DEFAULT

    llm = FakeLLM()
    session = Session(
        transport=FakeTransport(), stt=FakeSTT(), llm=llm, tts=FakeTTS(),
        settings=Settings(_env_file=None), embedder=FakeEmbedder(),
        role=WEB_DEFAULT, surface="web",
    )
    await utter(session)
    prompt = llm.prompts[0][0]["content"].lower()
    assert "no lists, no headings, no markdown" in prompt
    assert "markdown, lists and headings are fine" not in prompt


async def test_a_role_shaped_like_the_device_default_gets_the_measured_prompt():
    """The old identity check made a freshly-built lookalike get the *web*
    prompt, which was surprising enough to need a test explaining it. Roles
    are compared by value, so a lookalike now gets exactly BASE - the
    surprise is gone rather than documented."""
    from server.persona import BASE
    from server.roles import Role, SPEAKABLE_LANGUAGES

    lookalike = Role(id=7, name="Copy", prompt=None, max_sentences=2,
                     markdown_allowed=False, languages=SPEAKABLE_LANGUAGES,
                     pinned_mood=None)
    llm = FakeLLM()
    session = Session(
        transport=FakeTransport(), stt=FakeSTT(), llm=llm, tts=FakeTTS(),
        settings=Settings(_env_file=None), embedder=FakeEmbedder(), role=lookalike,
    )
    await utter(session)
    assert llm.prompts[0][0]["content"] == BASE


async def test_a_pinned_mood_overrides_the_tag_the_model_chose():
    from server.roles import Role, SPEAKABLE_LANGUAGES

    sleepy = Role(id=8, name="Sleepy", prompt=None, max_sentences=2,
                  markdown_allowed=False, languages=SPEAKABLE_LANGUAGES,
                  pinned_mood="sleepy")
    session = Session(
        transport=(transport := FakeTransport()), stt=FakeSTT(),
        llm=FakeLLM("[happy] Все добре."), tts=FakeTTS(),
        settings=Settings(_env_file=None), embedder=FakeEmbedder(), role=sleepy,
    )
    await utter(session)
    emotions = [f["value"] for f in transport.sent if f.get("type") == "emotion"]
    assert emotions == ["sleepy"]


async def test_no_pinned_mood_leaves_the_models_tag_alone():
    session = Session(
        transport=(transport := FakeTransport()), stt=FakeSTT(),
        llm=FakeLLM("[happy] Все добре."), tts=FakeTTS(),
        settings=Settings(_env_file=None), embedder=FakeEmbedder(),
    )
    await utter(session)
    emotions = [f["value"] for f in transport.sent if f.get("type") == "emotion"]
    assert emotions == ["happy"]
```

If `FakeTransport` exposes its frames under a different attribute than
`sent`, use that one — check `tests/fakes.py`; it already has a `types`
property, so a `sent` list of dicts exists under some name.

- [ ] **Step 8: Run to verify they fail**

Run: `uv run pytest tests/test_session.py -q`
Expected: FAIL — `TypeError: Session.__init__() got an unexpected keyword
argument 'role'`.

- [ ] **Step 9: Change the session**

In `server/session.py`, change the imports:

```python
from server.persona import build_system_prompt
from server.roles import DEVICE_DEFAULT
```

(`ESP32` is gone.) In `__init__`, replace the `style=ESP32` parameter with
`role=DEVICE_DEFAULT, surface="esp32"`, and the assignment `self.style =
style` with:

```python
        self.role = role
        self.surface = surface
```

In `_answer`, change the prompt call:

```python
            build_system_prompt(
                self.standing_instructions + self.facts, self.role, spoken=speak
            ),
```

`spoken=speak` is the point: a typed question is read, not heard, so it gets
the on-screen wording even on a surface that can also speak.

In the nested `say()`, replace the emotion line:

```python
                emotion = self.role.pinned_mood or tag.emotion or from_text(sentence)
                await self.transport.send_json({"type": "emotion", "value": emotion})
                log.info("emotion %s (%s)", emotion,
                         "pinned" if self.role.pinned_mood
                         else "tagged" if tag.emotion else "guessed")
```

- [ ] **Step 10: Run the tests**

Run: `uv run pytest tests/test_session.py -q`
Expected: all PASS.

Run: `uv run pytest -q`
Expected: green — Task 4's known WebSocket failures clear here. Record the
count; it should be ~318.

- [ ] **Step 11: Commit**

```bash
git add server/session.py tests/test_session.py
git commit -m "Let the session take a role, and pin the mood when one is set

The prompt is now chosen by value rather than by identity, so a role built
from the device's own defaults gets the measured prompt instead of silently
getting the web one - the surprise the old test had to document is gone. A
pinned mood wins over the tag the model chose, which is what makes the face
something you can drive from the app."
```

---

## Task 5: Retrieval reports its scores

`relevant_facts` sorts by cosine similarity and throws the number away. The
inspector's whole value is showing *why* a fact was retrieved, so the score
comes back with the text. One method returning both, not a scored sibling
next to an unscored one — two ways to rank the same rows would drift.

**Files:**
- Modify: `server/memory/store.py`
- Modify: `server/session.py`
- Modify: `tests/fakes.py`
- Modify: `tests/test_store.py`

**Interfaces:**
- Produces: `Store.relevant_facts(device_id, query_embedding, limit=6) ->
  list[tuple[str, float]]`, sorted by score descending.
  `Session.retrieved: list[tuple[str, float]]` — read by Task 6.

- [ ] **Step 1: Update the tests to expect scores**

In `tests/test_store.py`, rewrite the five `relevant_facts` tests:

```python
async def test_relevant_facts_ranks_by_similarity_not_recency(store):
    """Three facts, deliberately inserted so that the correct answer matches
    neither insertion order nor its reverse.

    With two facts, any ordering the sort produces is also produced by some
    trivial ORDER BY, so the test passed even with the sort removed. The
    middle fact is what makes the assertion about similarity rather than
    about rowid.
    """
    await store.add_facts(
        "dev1",
        ["Lives in Kyiv", "Has a cat named Musya", "Likes short answers"],
        [[0.0, 1.0], [1.0, 0.0], [0.7, 0.7]],
    )
    ranked = await store.relevant_facts("dev1", [1.0, 0.0])
    assert [text for text, _ in ranked] == [
        "Has a cat named Musya",
        "Likes short answers",
        "Lives in Kyiv",
    ]
    assert ranked[0][1] > ranked[1][1] > ranked[2][1]
    assert ranked[0][1] == pytest.approx(1.0)


async def test_relevant_facts_respects_the_limit(store):
    await store.add_facts("dev1", ["a", "b", "c"], [[1.0, 0.0], [0.9, 0.1], [0.0, 1.0]])
    assert len(await store.relevant_facts("dev1", [1.0, 0.0], limit=2)) == 2


async def test_relevant_facts_ignores_other_devices(store):
    await store.add_facts("dev1", ["mine"], [[1.0, 0.0]])
    await store.add_facts("dev2", ["theirs"], [[1.0, 0.0]])
    assert [t for t, _ in await store.relevant_facts("dev1", [1.0, 0.0])] == ["mine"]


async def test_relevant_facts_skips_rows_with_no_embedding(store):
    await store.add_facts("dev1", ["unembedded"])
    assert await store.relevant_facts("dev1", [1.0, 0.0]) == []


async def test_relevant_facts_with_no_query_is_empty(store):
    await store.add_facts("dev1", ["a"], [[1.0, 0.0]])
    assert await store.relevant_facts("dev1", None) == []
```

Also fix `test_user_facts_do_not_appear_in_auto_retrieval` to unpack tuples.
Add `import pytest` at the top of the file if it is not already there (it is).

In `tests/fakes.py`, change `FakeStore.relevant_facts` to return pairs:

```python
    async def relevant_facts(self, device_id: str, query_embedding, limit: int = 6):
        if query_embedding is None:
            return []
        # A fixed, descending score: the fake's job is the shape, not ranking.
        return [(f, 1.0 - i / 100) for i, f in enumerate(self._facts[:limit])]
```

Match the attribute name `FakeStore` actually uses for its facts list.

- [ ] **Step 2: Run to verify they fail**

Run: `uv run pytest tests/test_store.py -q -k relevant`
Expected: FAIL — comparing `str` to `tuple`.

- [ ] **Step 3: Return the scores**

In `server/memory/store.py`, change the last two lines of `relevant_facts`:

```python
        scored.sort(key=lambda pair: pair[1], reverse=True)
        return scored[:limit]
```

and update its docstring's first line to `"""Auto facts and their cosine
similarity to `query_embedding`, best first."""`.

- [ ] **Step 4: Keep the session's own use working**

In `server/session.py`'s `_answer`, replace the retrieval block:

```python
        if self.store is not None:
            query_vector = await self.embedder.embed_query(text)
            self.retrieved = await self.store.relevant_facts(
                self.device_id, query_vector, limit=self.settings.relevant_facts_limit
            )
            self.facts = [fact for fact, _ in self.retrieved]
```

and initialise it in `__init__` next to `self.facts`:

```python
        self.retrieved: list[tuple[str, float]] = []
```

- [ ] **Step 5: Run the suite**

Run: `uv run pytest -q`
Expected: green, same count as Task 4.

- [ ] **Step 6: Commit**

```bash
git add server/memory/store.py server/session.py tests/
git commit -m "Return the similarity with the fact, not just the ranking

The number is the interesting part when you are trying to work out why the
assistant answered the way it did, and it was being computed and discarded.
One method returns both; a scored sibling alongside an unscored one would be
two implementations of the same ranking, free to drift."
```

---

## Task 6: The trace frame

One JSON frame per answered turn, to `web` connections only, carrying what
the turn actually did. Best-effort: if assembling it raises, the reply has
already been delivered and the failure is logged, never surfaced.

**Files:**
- Modify: `server/session.py`
- Modify: `tests/test_session.py`
- Modify: `tests/test_main.py`

**Interfaces:**
- Consumes: `self.retrieved` (Task 5), `self.role`, `self.surface` (Task 4).
- Produces: a frame
  `{"type": "trace", "value": {"role", "surface", "facts": [{"text","score"}],
  "prompt", "prompt_tokens", "completion_tokens", "emotion", "spoken",
  "stt_ms", "reply_ms"}}`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_session.py`:

```python
async def test_a_web_session_gets_a_trace_frame():
    from server.roles import WEB_DEFAULT

    session = Session(
        transport=(transport := FakeTransport()), stt=FakeSTT(),
        llm=FakeLLM("[happy] Все добре."), tts=FakeTTS(),
        settings=Settings(_env_file=None), embedder=FakeEmbedder(),
        role=WEB_DEFAULT, surface="web",
    )
    await session.on_text("Як справи?")
    await session.wait_for_reply()

    traces = [f["value"] for f in transport.sent if f.get("type") == "trace"]
    assert len(traces) == 1
    trace = traces[0]
    assert trace["role"] == "Web default"
    assert trace["surface"] == "web"
    assert trace["emotion"] == "happy"
    assert trace["spoken"] is False
    assert trace["prompt_tokens"] > 0
    assert "Rules:" in trace["prompt"]


async def test_the_device_never_gets_a_trace_frame():
    session = Session(
        transport=(transport := FakeTransport()), stt=FakeSTT(), llm=FakeLLM(),
        tts=FakeTTS(), settings=Settings(_env_file=None), embedder=FakeEmbedder(),
    )
    await utter(session)
    assert "trace" not in transport.types


async def test_the_trace_carries_the_retrieved_facts_and_their_scores():
    from tests.fakes import FakeStore

    session = Session(
        transport=(transport := FakeTransport()), stt=FakeSTT(), llm=FakeLLM(),
        tts=FakeTTS(), settings=Settings(_env_file=None), embedder=FakeEmbedder(),
        store=FakeStore(facts=["Lives in Chernivtsi"]), surface="web",
    )
    await session.on_text("Де я живу?")
    await session.wait_for_reply()

    trace = next(f["value"] for f in transport.sent if f.get("type") == "trace")
    assert trace["facts"] == [{"text": "Lives in Chernivtsi", "score": 1.0}]


async def test_a_broken_trace_does_not_lose_the_reply(monkeypatch):
    """The frame is a debugging aid; the answer is the product."""
    from server.roles import WEB_DEFAULT

    session = Session(
        transport=(transport := FakeTransport()), stt=FakeSTT(),
        llm=FakeLLM("Все добре."), tts=FakeTTS(),
        settings=Settings(_env_file=None), embedder=FakeEmbedder(),
        role=WEB_DEFAULT, surface="web",
    )
    monkeypatch.setattr(session, "_trace", lambda **kw: (_ for _ in ()).throw(RuntimeError("boom")))
    await session.on_text("Як справи?")
    await session.wait_for_reply()
    assert "reply" in transport.types
    assert "trace" not in transport.types
```

And in `tests/test_main.py`:

```python
def test_a_bearer_socket_never_receives_a_trace_frame():
    with client(device_token="s3cret") as c:
        with c.websocket_connect("/ws", headers={"Authorization": "Bearer s3cret"}) as ws:
            ws.send_text(json.dumps({"type": "text", "value": "Як справи?"}))
            seen = []
            for _ in range(12):
                frame = json.loads(ws.receive_text())
                seen.append(frame["type"])
                if frame["type"] == "state" and frame["value"] == "idle":
                    break
    assert "trace" not in seen
```

- [ ] **Step 2: Run to verify they fail**

Run: `uv run pytest tests/test_session.py -q -k trace`
Expected: FAIL — no `trace` frame is ever sent.

- [ ] **Step 3: Emit it**

In `server/session.py`'s `_answer`, hoist the prompt into a local so the
frame can carry the real text, and capture the emotion. Replace the
`messages = build_messages(...)` call with:

```python
        system_prompt = build_system_prompt(
            # persona.py takes one flat list; standing instructions come
            # first so a relevant fact never pushes a user's own rule out of
            # the prompt if both were ever truncated upstream.
            self.standing_instructions + self.facts, self.role, spoken=speak
        )
        messages = build_messages(
            system_prompt, self.history, text, self.settings.max_context_tokens
        )
```

Add `chosen_emotion: str | None = None` beside the other `nonlocal`-shared
locals (next to `voice` and `language`), and in `say()` record it where the
frame is sent:

```python
            nonlocal voice, language, chosen_emotion
            ...
                chosen_emotion = emotion
```

Then, at the very end of `_answer`, after `self.usage.add_turn(...)`:

```python
        if self.surface == "web":
            try:
                await self._trace(
                    prompt=system_prompt,
                    prompt_tokens=prompt_tokens,
                    reply=reply,
                    emotion=chosen_emotion,
                    spoken=speak,
                    stt_ms=stt_ms,
                    reply_ms=(time.monotonic() - reply_started) * 1000,
                )
            except Exception:
                # The answer is already delivered. A debugging aid must never
                # be the reason a turn looks like it failed.
                log.exception("failed to send trace frame")
```

Add the method in the helpers section:

```python
    async def _trace(self, *, prompt, prompt_tokens, reply, emotion, spoken,
                     stt_ms, reply_ms) -> None:
        """What this turn actually did, for the playground's inspector.

        Web only. The device is on a metered radio with a 55 KB free-heap
        margin (RESUME.md) and no use for the payload.
        """
        await self.transport.send_json({
            "type": "trace",
            "value": {
                "role": self.role.name,
                "surface": self.surface,
                "facts": [{"text": t, "score": round(s, 4)} for t, s in self.retrieved],
                "prompt": prompt,
                "prompt_tokens": prompt_tokens,
                "completion_tokens": estimate_tokens(reply),
                "emotion": emotion,
                "spoken": spoken,
                "stt_ms": round(stt_ms, 1),
                "reply_ms": round(reply_ms, 1),
            },
        })
```

Give `_answer` an `stt_ms: float = 0.0` keyword parameter, and pass the
figure `_respond` already measures:

```python
        started = time.perf_counter()
        transcript = await self.stt.transcribe(pcm, self.settings.sample_rate)
        ...
        stt_ms = (time.perf_counter() - started) * 1000
        log.info("stt %.0f ms: %s", stt_ms, text)

        await self._answer(text, speak=True, audio_seconds=transcript.seconds,
                           stt_ms=stt_ms)
```

- [ ] **Step 4: Run the tests**

Run: `uv run pytest tests/test_session.py tests/test_main.py -q`
Expected: all PASS.

Run: `uv run pytest -q`
Expected: green, ~324.

- [ ] **Step 5: Commit**

```bash
git add server/session.py tests/
git commit -m "Say what the turn actually did, to the browser only

One frame per answered turn: the role, the facts retrieved with their
scores, the assembled prompt, token counts and where the time went - which
is the difference between a chat box and a playground. The device never gets
it; it has 55 KB of heap to spare and nothing to do with the payload. Sending
it is best-effort, because a debugging aid must not be able to turn a
delivered answer into a failed one."
```

---

## Task 7: Recorded conversations, with retention

Nothing has ever stored what was said — `session_usage` holds counters only.
This records it, with no UI: slice 3 charts it, and starting now means there
is history to chart by then. It is personal data, including other people's,
so recording is switchable and old rows expire.

**Files:**
- Modify: `server/memory/store.py`
- Modify: `server/session.py`
- Modify: `server/main.py`
- Modify: `tests/fakes.py`
- Modify: `tests/test_store.py`, `tests/test_session.py`, `tests/test_main.py`

**Interfaces:**
- Produces:
  - `Store.app_settings() -> {"store_conversations": bool, "retention_days": int}`
  - `Store.set_app_settings(store_conversations=None, retention_days=None) -> dict`
  - `Store.start_conversation(device_id, surface, role_name) -> int`
  - `Store.record_turn(conversation_id, question, reply, emotion, prompt_tokens, completion_tokens, latency_ms) -> None`
  - `Store.end_conversation(conversation_id) -> None`
  - `Store.conversation_rows(device_id, limit=50) -> list[dict]`
  - `Store.purge_expired() -> int`
  - `GET /settings/app`, `PUT /settings/app` — both `require_login`

- [ ] **Step 1: Write the failing store tests**

Append to `tests/test_store.py`:

```python
# ------------------------------------------------------- conversation history

async def test_app_settings_default_to_recording_on_for_ninety_days(store):
    assert await store.app_settings() == {"store_conversations": True, "retention_days": 90}


async def test_app_settings_round_trip(store):
    await store.set_app_settings(store_conversations=False, retention_days=7)
    assert await store.app_settings() == {"store_conversations": False, "retention_days": 7}


async def test_setting_one_app_setting_leaves_the_other(store):
    await store.set_app_settings(retention_days=30)
    settings = await store.app_settings()
    assert settings == {"store_conversations": True, "retention_days": 30}


async def test_a_recorded_turn_keeps_both_halves(store):
    cid = await store.start_conversation("dev1", "web", "Web default")
    await store.record_turn(cid, question="Де я живу?", reply="У Чернівцях.",
                            emotion="neutral", prompt_tokens=120,
                            completion_tokens=8, latency_ms=845.0)
    rows = await store.conversation_rows("dev1")
    assert len(rows) == 1
    assert rows[0]["surface"] == "web"
    assert rows[0]["role_name"] == "Web default"
    assert rows[0]["turns"] == 1
    assert rows[0]["messages"][0] == {"role": "user", "text": "Де я живу?", "emotion": None}
    assert rows[0]["messages"][1]["role"] == "assistant"
    assert rows[0]["messages"][1]["text"] == "У Чернівцях."
    assert rows[0]["messages"][1]["emotion"] == "neutral"


async def test_conversations_are_scoped_per_device(store):
    await store.start_conversation("dev1", "web", "Web default")
    assert await store.conversation_rows("dev2") == []


async def test_ending_a_conversation_stamps_it(store):
    cid = await store.start_conversation("dev1", "esp32", "Device default")
    await store.end_conversation(cid)
    assert (await store.conversation_rows("dev1"))[0]["ended_at"] is not None


async def test_purge_removes_conversations_past_retention(store):
    from datetime import datetime, timedelta, timezone

    from server.memory.store import Conversation

    cid = await store.start_conversation("dev1", "web", "Web default")
    await store.record_turn(cid, question="q", reply="r", emotion=None,
                            prompt_tokens=1, completion_tokens=1, latency_ms=1.0)
    await store.set_app_settings(retention_days=1)
    async with store._session() as s:  # noqa: SLF001 - fixture-level surgery
        row = await s.get(Conversation, cid)
        row.started_at = datetime.now(timezone.utc) - timedelta(days=5)
        await s.commit()

    assert await store.purge_expired() == 1
    assert await store.conversation_rows("dev1") == []


async def test_purge_keeps_conversations_inside_retention(store):
    cid = await store.start_conversation("dev1", "web", "Web default")
    await store.record_turn(cid, question="q", reply="r", emotion=None,
                            prompt_tokens=1, completion_tokens=1, latency_ms=1.0)
    assert await store.purge_expired() == 0
    assert len(await store.conversation_rows("dev1")) == 1


async def test_retention_of_zero_days_never_purges(store):
    cid = await store.start_conversation("dev1", "web", "Web default")
    await store.set_app_settings(retention_days=0)
    from datetime import datetime, timedelta, timezone

    from server.memory.store import Conversation

    async with store._session() as s:  # noqa: SLF001
        row = await s.get(Conversation, cid)
        row.started_at = datetime.now(timezone.utc) - timedelta(days=4000)
        await s.commit()
    assert await store.purge_expired() == 0
```

- [ ] **Step 2: Run to verify they fail**

Run: `uv run pytest tests/test_store.py -q -k "app_settings or conversation or purge"`
Expected: FAIL — `AttributeError: 'Store' object has no attribute 'app_settings'`.

- [ ] **Step 3: Add the tables and methods**

In `server/memory/store.py`, add `Text` to the `sqlalchemy` import and the
models after `SessionUsage`:

```python
class Conversation(Base):
    __tablename__ = "conversations"
    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    device_id: Mapped[str] = mapped_column(String(64), index=True)
    surface: Mapped[str] = mapped_column(String(16))
    role_name: Mapped[str] = mapped_column(String(64))
    started_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=_now)
    ended_at: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True
    )


class Message(Base):
    __tablename__ = "messages"
    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    conversation_id: Mapped[int] = mapped_column(Integer, index=True)
    # "user" or "assistant" - the same two names the LLM history uses, so a
    # recorded turn and a prompt turn do not need translating between them.
    role: Mapped[str] = mapped_column(String(16))
    text: Mapped[str] = mapped_column(Text)
    emotion: Mapped[str | None] = mapped_column(String(16), nullable=True)
    prompt_tokens: Mapped[int] = mapped_column(Integer, default=0)
    completion_tokens: Mapped[int] = mapped_column(Integer, default=0)
    latency_ms: Mapped[float] = mapped_column(Float, default=0.0)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=_now)


class AppSetting(Base):
    """Key/value, because there are two of these and both are user-editable.

    Settings.py is for deploy-time configuration a person sets once on
    Railway; this is for switches the app itself flips at runtime.
    """
    __tablename__ = "app_settings"
    key: Mapped[str] = mapped_column(String(32), primary_key=True)
    value: Mapped[str] = mapped_column(String(64))
```

And the methods on `Store`:

```python
    _DEFAULT_APP_SETTINGS = {"store_conversations": "1", "retention_days": "90"}

    async def app_settings(self) -> dict:
        async with self._session() as s:
            rows = await s.scalars(select(AppSetting))
            stored = {r.key: r.value for r in rows.all()}
        merged = {**self._DEFAULT_APP_SETTINGS, **stored}
        return {
            "store_conversations": merged["store_conversations"] == "1",
            "retention_days": int(merged["retention_days"]),
        }

    async def set_app_settings(
        self, store_conversations: bool | None = None, retention_days: int | None = None
    ) -> dict:
        pairs = {}
        if store_conversations is not None:
            pairs["store_conversations"] = "1" if store_conversations else "0"
        if retention_days is not None:
            pairs["retention_days"] = str(max(0, int(retention_days)))
        async with self._session() as s:
            for key, value in pairs.items():
                row = await s.get(AppSetting, key)
                if row is None:
                    s.add(AppSetting(key=key, value=value))
                else:
                    row.value = value
            await s.commit()
        return await self.app_settings()

    async def start_conversation(self, device_id: str, surface: str, role_name: str) -> int:
        async with self._session() as s:
            row = Conversation(device_id=device_id, surface=surface, role_name=role_name)
            s.add(row)
            await s.commit()
            await s.refresh(row)
            return row.id

    async def record_turn(
        self, conversation_id: int, *, question: str, reply: str, emotion: str | None,
        prompt_tokens: int, completion_tokens: int, latency_ms: float,
    ) -> None:
        """Two rows, not one: a turn is a question and an answer, and slice 3
        wants to read them back in order without unpacking a composite row."""
        async with self._session() as s:
            s.add(Message(conversation_id=conversation_id, role="user", text=question,
                          emotion=None, prompt_tokens=prompt_tokens,
                          completion_tokens=0, latency_ms=0.0))
            s.add(Message(conversation_id=conversation_id, role="assistant", text=reply,
                          emotion=emotion, prompt_tokens=0,
                          completion_tokens=completion_tokens, latency_ms=latency_ms))
            await s.commit()

    async def end_conversation(self, conversation_id: int) -> None:
        async with self._session() as s:
            row = await s.get(Conversation, conversation_id)
            if row is not None:
                row.ended_at = _now()
                await s.commit()

    async def conversation_rows(self, device_id: str, limit: int = 50) -> list[dict]:
        async with self._session() as s:
            conversations = (
                await s.scalars(
                    select(Conversation)
                    .where(Conversation.device_id == device_id)
                    .order_by(Conversation.id.desc())
                    .limit(limit)
                )
            ).all()
            out = []
            for c in conversations:
                messages = (
                    await s.scalars(
                        select(Message)
                        .where(Message.conversation_id == c.id)
                        .order_by(Message.id.asc())
                    )
                ).all()
                out.append({
                    "id": c.id,
                    "surface": c.surface,
                    "role_name": c.role_name,
                    "started_at": c.started_at,
                    "ended_at": c.ended_at,
                    "turns": sum(1 for m in messages if m.role == "user"),
                    "messages": [
                        {"role": m.role, "text": m.text, "emotion": m.emotion}
                        for m in messages
                    ],
                })
            return out

    async def purge_expired(self) -> int:
        """Delete conversations past the retention window. Returns how many.

        `retention_days = 0` means keep forever, which is why this reads the
        setting rather than taking a parameter: the switch and the sweep must
        agree, and there is exactly one place to set it.
        """
        settings = await self.app_settings()
        days = settings["retention_days"]
        if days <= 0:
            return 0
        cutoff = _now() - timedelta(days=days)
        async with self._session() as s:
            doomed = (
                await s.scalars(select(Conversation.id).where(Conversation.started_at < cutoff))
            ).all()
            if not doomed:
                return 0
            await s.execute(delete(Message).where(Message.conversation_id.in_(doomed)))
            await s.execute(delete(Conversation).where(Conversation.id.in_(doomed)))
            await s.commit()
            return len(doomed)
```

Add `timedelta` to the `datetime` import at the top of the file.

- [ ] **Step 4: Run the store tests**

Run: `uv run pytest tests/test_store.py -q`
Expected: all PASS.

- [ ] **Step 5: Record from the session**

Append to `tests/test_session.py`:

```python
async def test_a_turn_is_recorded_when_recording_is_on():
    from tests.fakes import FakeStore

    store = FakeStore()
    session = Session(
        transport=FakeTransport(), stt=FakeSTT(), llm=FakeLLM("[happy] Все добре."),
        tts=FakeTTS(), settings=Settings(_env_file=None), embedder=FakeEmbedder(),
        store=store, surface="web",
    )
    await session.on_text("Як справи?")
    await session.wait_for_reply()
    assert store.turns == [("Як справи?", "Все добре.", "happy")]


async def test_nothing_is_recorded_when_recording_is_off():
    from tests.fakes import FakeStore

    store = FakeStore(store_conversations=False)
    session = Session(
        transport=FakeTransport(), stt=FakeSTT(), llm=FakeLLM("Все добре."),
        tts=FakeTTS(), settings=Settings(_env_file=None), embedder=FakeEmbedder(),
        store=store, surface="web",
    )
    await session.on_text("Як справи?")
    await session.wait_for_reply()
    assert store.turns == []
    assert store.started == []


async def test_finishing_ends_the_conversation():
    from tests.fakes import FakeStore

    store = FakeStore()
    session = Session(
        transport=FakeTransport(), stt=FakeSTT(), llm=FakeLLM("Все добре."),
        tts=FakeTTS(), settings=Settings(_env_file=None), embedder=FakeEmbedder(),
        store=store, surface="web",
    )
    await session.on_text("Як справи?")
    await session.wait_for_reply()
    await session.finish()
    assert store.ended == store.started
```

Extend `FakeStore.__init__` with `store_conversations: bool = True`, and add:

```python
        self.turns: list[tuple[str, str, str | None]] = []
        self.started: list[int] = []
        self.ended: list[int] = []
        self._store_conversations = store_conversations

    async def app_settings(self) -> dict:
        return {"store_conversations": self._store_conversations, "retention_days": 90}

    async def start_conversation(self, device_id: str, surface: str, role_name: str) -> int:
        cid = len(self.started) + 1
        self.started.append(cid)
        return cid

    async def record_turn(self, conversation_id, *, question, reply, emotion,
                          prompt_tokens, completion_tokens, latency_ms) -> None:
        self.turns.append((question, reply, emotion))

    async def end_conversation(self, conversation_id: int) -> None:
        self.ended.append(conversation_id)

    async def purge_expired(self) -> int:
        return 0
```

Then in `server/session.py`: add `self.conversation_id: int | None = None` to
`__init__`, and at the end of `_answer`, before the `trace` block:

```python
        if self.store is not None:
            # Read per turn, not per session, so switching recording off in
            # Settings takes effect on the next question rather than the next
            # connection.
            recording = (await self.store.app_settings())["store_conversations"]
            if recording:
                if self.conversation_id is None:
                    self.conversation_id = await self.store.start_conversation(
                        self.device_id, self.surface, self.role.name
                    )
                await self.store.record_turn(
                    self.conversation_id,
                    question=text,
                    reply=reply,
                    emotion=chosen_emotion,
                    prompt_tokens=prompt_tokens,
                    completion_tokens=estimate_tokens(reply),
                    latency_ms=(time.monotonic() - reply_started) * 1000,
                )
```

and in `finish()`, after the usage log:

```python
        if self.conversation_id is not None:
            await self.store.end_conversation(self.conversation_id)
```

- [ ] **Step 6: The settings endpoints and the startup sweep**

In `server/main.py`, add the model beside `MemoryIn`:

```python
class AppSettingsIn(BaseModel):
    store_conversations: bool | None = None
    retention_days: int | None = None
```

the routes beside the surfaces routes:

```python
    @app.get("/settings/app", dependencies=[Depends(require_login)])
    async def get_app_settings() -> dict:
        return await app.state.store.app_settings()

    @app.put("/settings/app", dependencies=[Depends(require_login)])
    async def put_app_settings(body: AppSettingsIn) -> dict:
        return await app.state.store.set_app_settings(
            store_conversations=body.store_conversations,
            retention_days=body.retention_days,
        )
```

and the sweep in the lifespan, right after `backfill_embeddings`:

```python
        if not injected:
            purged = await app.state.store.purge_expired()
            if purged:
                log.info("purged %d conversations past the retention window", purged)
```

Add to `tests/test_main.py`:

```python
def test_app_settings_require_login():
    with client() as c:
        assert c.get("/settings/app").status_code == 401
        assert c.put("/settings/app", json={"retention_days": 7}).status_code == 401


def test_app_settings_round_trip_over_http():
    with client() as c:
        _login(c)
        assert c.get("/settings/app").json() == {
            "store_conversations": True, "retention_days": 90,
        }
        updated = c.put("/settings/app", json={"store_conversations": False}).json()
    assert updated["store_conversations"] is False
```

`FakeStore.set_app_settings` needs to exist for that test:

```python
    async def set_app_settings(self, store_conversations=None, retention_days=None) -> dict:
        if store_conversations is not None:
            self._store_conversations = store_conversations
        return await self.app_settings()
```

- [ ] **Step 7: Run everything**

Run: `uv run pytest -q`
Expected: green, ~345 passed. If `test_app_settings_round_trip_over_http`
fails on `retention_days`, the fake is ignoring it — store it on the fake the
same way as the flag.

- [ ] **Step 8: Commit**

```bash
git add server/memory/store.py server/session.py server/main.py tests/
git commit -m "Start recording what was actually said, with an expiry date

Nothing has ever stored a transcript, so the dashboard in slice 3 would have
opened on an empty table. This records question and reply as two rows per
turn, defaults to on, and sweeps anything past the retention window at boot.
The switch is read per turn rather than per connection, so turning recording
off in Settings takes effect on the next question rather than the next
reboot - it is other people's words in there too, not only mine."
```

---

## Self-Review

**Spec coverage.** Every slice-1 backend item in the spec maps to a task:
merge (T1), roles as rows with a surface pointer (T2), `BASE` byte-identical
plus configurable persona and the ESP32 reversal (T3), the language subset
rule (T2 validation + T3 wording), roles/settings API replacing
`style_overrides` and the session that obeys a role (T4, two phases), pinned
mood (T3 wording + T4 phase B forcing), scored retrieval (T5), web-only
`trace` (T6), recording with retention and an off switch (T7).

**Deliberately deferred to the frontend plan**, all from the spec's own list:
deleting `GET /memory`'s HTML page, the static mount and SPA fallback, the
`Dockerfile`, `SESSION_SECRET_KEY` on Railway, and the post-deploy cleanup of
other people's facts. Each needs the built bundle to exist first.

**One deviation from the spec, on purpose.** The spec sketched
`resolve_prompt(role, facts)` living in `server/roles.py` while also saying
prompt assembly stays in `persona.py`. It cannot be both. Assembly is in
`persona.py` and `roles.py` imports nothing from the project, which is what
keeps the dependency one-directional.

**Types checked across tasks.** `Role` fields are used with the same names
and types in T2/T3/T4/T5/T7. `relevant_facts` returns `list[tuple[str,
float]]` in T6 and is consumed as pairs in T6/T7. `record_turn` is called
with exactly the keyword names T8 defines. `Session(role=, surface=)` is
introduced and consumed inside T4, whose two phases are red between them by
construction — the only intentional red point in the plan, and the reason
they are one task rather than two.

**Test counts** are stated per task as a check that nothing silently
disappears: 288 after T1, 308 after T2, ~318 after T4, ~324 after T6, ~345
after T7.
