# Shared Identity Backend Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the existing voice-companion server a second, real-login-gated
door (alongside the ESP32's untouched `DEVICE_TOKEN`), a text-chat path
through the same pipeline, and a per-surface, persisted, configurable
persona — so a future web client and the ESP32 are provably one assistant,
not two.

**Architecture:** One FastAPI app, one SQLite file, three new small modules
(`server/accounts.py`, additions to `server/memory/store.py`,
`server/persona.py`) plus targeted changes to `server/session.py` and
`server/main.py`. No new service, no new database, no new auth system for
the ESP32.

**Tech Stack:** FastAPI, Starlette's built-in `SessionMiddleware` (already a
transitive dependency — zero new packages for sessions), `bcrypt` (one new,
deliberate dependency for password hashing), SQLAlchemy 2.0 async (already
in use), pytest + `fastapi.testclient.TestClient` (already in use).

This plan implements the spec at
`docs/superpowers/specs/2026-08-29-shared-identity-web-client-design.md`.
It does **not** include the React web client — that is a separate plan,
built after this one, since it consumes the endpoints this plan produces.

## Global Constraints

- `DEVICE_TOKEN` bearer auth for the ESP32 stays exactly as it is today —
  no task in this plan modifies `_token_ok`'s existing behaviour or the
  bearer check itself, only adds an alternative alongside it.
- `device_id` stays `"default"`. No `User → devices` table. There is one
  user and one device.
- The ESP32 persona wording (`server/persona.py`'s `BASE` constant) must
  remain **byte-identical** to what it is before this plan — it was
  measured, per `RESUME.md`, and no task here may edit that string.
- `bcrypt` is an approved new dependency (password hashing is a real place
  to use a vetted library, not roll your own). No other new runtime
  dependency is expected in this plan.
- Live, real-time cross-surface conversation sync is explicitly out of
  scope here — only long-term facts and per-surface style are shared.
- Every new endpoint that edits or reads personal data (settings, style)
  is gated by `require_login`, not `require_token` — those are for the
  human via the web app, not the device.

---

## Task 1: Password accounts

**Files:**
- Create: `server/accounts.py`
- Create: `scripts/create_account.py`
- Create: `tests/test_accounts.py`
- Modify: `pyproject.toml` (add `bcrypt` dependency)

**Interfaces:**
- Produces: `Accounts(url: str)` with `async init()`, `async close()`,
  `async create_user(username: str, password: str) -> None`,
  `async verify_password(username: str, password: str) -> bool`. Later
  tasks construct one exactly like `Store(url)` is constructed today.

- [ ] **Step 1: Add the `bcrypt` dependency**

Edit `pyproject.toml`'s `[project] dependencies` list (currently ends with
`"fastembed>=0.5",`) to add one line after it:

```toml
    "fastembed>=0.5",
    "bcrypt>=4.0",
```

Run: `uv sync`
Expected: `bcrypt` appears in the install output, no other new packages
(it has no heavy transitive dependencies).

- [ ] **Step 2: Write the failing tests**

Create `tests/test_accounts.py`:

```python
import pytest

from server.accounts import Accounts


@pytest.fixture
async def accounts(tmp_path):
    a = Accounts(f"sqlite+aiosqlite:///{tmp_path}/test.db")
    await a.init()
    yield a
    await a.close()


async def test_verify_password_is_false_for_an_unknown_user(accounts):
    assert await accounts.verify_password("nobody", "whatever") is False


async def test_created_user_can_verify_their_password(accounts):
    await accounts.create_user("oleksandr", "correct-horse")
    assert await accounts.verify_password("oleksandr", "correct-horse") is True


async def test_wrong_password_is_rejected(accounts):
    await accounts.create_user("oleksandr", "correct-horse")
    assert await accounts.verify_password("oleksandr", "wrong") is False


async def test_creating_a_user_twice_replaces_the_password(accounts):
    await accounts.create_user("oleksandr", "first-password")
    await accounts.create_user("oleksandr", "second-password")
    assert await accounts.verify_password("oleksandr", "first-password") is False
    assert await accounts.verify_password("oleksandr", "second-password") is True


async def test_password_is_not_stored_in_plain_text(accounts):
    from sqlalchemy import select

    from server.accounts import User

    await accounts.create_user("oleksandr", "correct-horse")
    async with accounts._session() as s:
        user = (await s.scalars(select(User).where(User.username == "oleksandr"))).first()
    assert "correct-horse" not in user.password_hash
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `uv run pytest tests/test_accounts.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'server.accounts'`

- [ ] **Step 4: Write `server/accounts.py`**

```python
"""Password accounts for the web login.

A separate module from server/memory/store.py on purpose: Store holds facts
and usage, a different bounded concern from who is allowed to log in. Same
SQLite file, same Store.__init__(url) pattern, its own table.
"""

import bcrypt
from sqlalchemy import Integer, String, select
from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker, create_async_engine
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column


class Base(DeclarativeBase):
    pass


class User(Base):
    __tablename__ = "users"
    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    username: Mapped[str] = mapped_column(String(64), unique=True)
    password_hash: Mapped[str] = mapped_column(String(255))


class Accounts:
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

    async def create_user(self, username: str, password: str) -> None:
        """Create the account, or replace the password if it already exists."""
        password_hash = bcrypt.hashpw(password.encode("utf-8"), bcrypt.gensalt()).decode("ascii")
        async with self._session() as s:
            existing = (await s.scalars(select(User).where(User.username == username))).first()
            if existing is not None:
                existing.password_hash = password_hash
            else:
                s.add(User(username=username, password_hash=password_hash))
            await s.commit()

    async def verify_password(self, username: str, password: str) -> bool:
        async with self._session() as s:
            user = (await s.scalars(select(User).where(User.username == username))).first()
            if user is None:
                # Hash something anyway - a real username and an unknown one
                # should not be distinguishable by response time.
                bcrypt.hashpw(password.encode("utf-8"), bcrypt.gensalt())
                return False
            return bcrypt.checkpw(password.encode("utf-8"), user.password_hash.encode("ascii"))
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `uv run pytest tests/test_accounts.py -v`
Expected: 5 passed

- [ ] **Step 6: Write the operator tool to set the one account's password**

Create `scripts/create_account.py`:

```python
"""One-time (or repeatable) tool to set the web login password.

There is exactly one account. Run locally against DB_PATH, or against
Railway's volume: `railway run python scripts/create_account.py --username you --password ...`
"""

import argparse
import asyncio

from server.accounts import Accounts
from server.config import Settings


async def main(username: str, password: str) -> None:
    settings = Settings()
    accounts = Accounts(f"sqlite+aiosqlite:///{settings.db_path}")
    await accounts.init()
    await accounts.create_user(username, password)
    await accounts.close()
    print(f"account '{username}' set")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--username", required=True)
    parser.add_argument("--password", required=True)
    args = parser.parse_args()
    asyncio.run(main(args.username, args.password))
```

This is not exercised by pytest — it is an operator tool, tested manually:

Run: `uv run python scripts/create_account.py --username test --password test123`
Expected: prints `account 'test' set`, and `voice.db` (or wherever `DB_PATH`
points) now has a `users` table with one row.

- [ ] **Step 7: Commit**

```bash
git add server/accounts.py scripts/create_account.py tests/test_accounts.py pyproject.toml uv.lock
git commit -m "Add password accounts, separate from the facts/usage store"
```

---

## Task 2: Session cookies and login/logout

**Files:**
- Modify: `server/config.py`
- Modify: `server/main.py`
- Modify: `tests/fakes.py`
- Modify: `tests/test_main.py`

**Interfaces:**
- Consumes: `Accounts.verify_password(username, password) -> bool` from Task 1.
- Produces: `create_app(..., accounts=None)` — same optional-DI pattern as
  `store`/`embedder`. A `require_login` FastAPI dependency other tasks
  attach to routes via `dependencies=[Depends(require_login)]`. Session
  state is read as `websocket.session.get("user")` (on a WebSocket) or
  `request.session.get("user")` (on an HTTP request) once
  `SessionMiddleware` is installed.

- [ ] **Step 1: Add the session secret setting**

In `server/config.py`, add after `device_token: str = ""` (around line 69):

```python
    device_token: str = ""

    # Signs the web login's session cookie. Must be set on Railway before
    # this deploys, same category as DEVICE_TOKEN and GROQ_API_KEY. Starlette
    # will still sign cookies with an empty key (fine for local dev and
    # tests), just not securely.
    session_secret_key: str = ""
```

- [ ] **Step 2: Add `FakeAccounts` to the test doubles**

In `tests/fakes.py`, add after the `FakeEmbedder` class:

```python
class FakeAccounts:
    def __init__(self, username: str = "test", password: str = "test123") -> None:
        self._username = username
        self._password = password

    async def verify_password(self, username: str, password: str) -> bool:
        return username == self._username and password == self._password
```

- [ ] **Step 3: Write the failing tests**

In `tests/test_main.py`, update the import line and `client()` helper:

```python
from tests.fakes import FakeAccounts, FakeEmbedder, FakeLLM, FakeSTT, FakeStore, FakeTTS


@contextmanager
def client(store=None, accounts=None, **kw):
    app = create_app(
        settings=Settings(_env_file=None, **kw),
        stt=FakeSTT(),
        llm=FakeLLM("Все добре."),
        tts=FakeTTS(),
        store=store or FakeStore(),
        embedder=FakeEmbedder(),
        accounts=accounts or FakeAccounts(),
    )
    with TestClient(app) as c:
        yield c
```

Then add these tests at the end of the file:

```python
def test_login_with_the_right_password_sets_a_session_cookie():
    with client() as c:
        r = c.post("/login", json={"username": "test", "password": "test123"})
        assert r.status_code == 200
        assert "session" in r.cookies


def test_login_with_the_wrong_password_is_rejected():
    with client() as c:
        r = c.post("/login", json={"username": "test", "password": "wrong"})
    assert r.status_code == 401


def test_login_with_an_unknown_username_is_rejected():
    with client() as c:
        r = c.post("/login", json={"username": "nobody", "password": "anything"})
    assert r.status_code == 401


def test_logout_clears_the_session():
    with client() as c:
        c.post("/login", json={"username": "test", "password": "test123"})
        r = c.post("/logout")
        assert r.status_code == 200
        assert "session" not in r.cookies or r.cookies.get("session") == ""
```

- [ ] **Step 4: Run the tests to verify they fail**

Run: `uv run pytest tests/test_main.py -k login -v`
Expected: FAIL — `TypeError: create_app() got an unexpected keyword argument 'accounts'`

- [ ] **Step 5: Wire `Accounts`, `SessionMiddleware`, and the login routes into `server/main.py`**

Update the imports at the top of `server/main.py`:

```python
from fastapi import Depends, FastAPI, Header, HTTPException, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse
from pydantic import BaseModel
from starlette.middleware.sessions import SessionMiddleware

from server.accounts import Accounts
from server.config import Settings
```

Add a `LoginIn` model next to `MemoryIn`:

```python
class LoginIn(BaseModel):
    username: str
    password: str
```

Update `create_app`'s signature and lifespan (the `accounts` wiring mirrors
`store`'s `injected` pattern exactly):

```python
def create_app(
    settings=None, stt=None, llm=None, tts=None, store=None, embedder=None, accounts=None
) -> FastAPI:
    """Build the app. Providers are injectable so tests need no network."""
    settings = settings or Settings()
    logging.basicConfig(level=settings.log_level.upper())

    injected = store is not None
    accounts_injected = accounts is not None

    if not settings.session_secret_key:
        log.warning("SESSION_SECRET_KEY is not set - session cookies are signed with an empty key")

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        app.state.store = store
        if not injected:
            app.state.store = Store(f"sqlite+aiosqlite:///{settings.db_path}")
            await app.state.store.init()
        app.state.accounts = accounts
        if not accounts_injected:
            app.state.accounts = Accounts(f"sqlite+aiosqlite:///{settings.db_path}")
            await app.state.accounts.init()
        if settings.provider_mode == "mock":
            log.warning("PROVIDER_MODE=mock: speech is not actually transcribed")
            app.state.stt = stt or MockSTT()
            app.state.llm = llm or MockLLM()
            app.state.embedder = embedder or MockEmbedder()
        else:
            app.state.stt = stt or GroqSTT(settings.groq_api_key, settings.stt_model)
            app.state.llm = llm or GroqLLM(settings.groq_api_key, settings.llm_model)
            app.state.embedder = embedder or FastEmbedEmbedder(
                settings.embedding_model, settings.embedding_cache_dir or None
            )
        app.state.tts = tts or EdgeTTS(rate=settings.sample_rate)
        if not injected:
            await app.state.store.backfill_embeddings(app.state.embedder.embed_documents)
        yield
        if not injected and app.state.store is not None:
            await app.state.store.close()
        if not accounts_injected and app.state.accounts is not None:
            await app.state.accounts.close()

    app = FastAPI(title="voice-companion", lifespan=lifespan)
    app.state.settings = settings
    # https_only=False: Railway terminates TLS at its edge and forwards to
    # this container over plain HTTP, so the app itself never sees "https".
    # Setting this True here would make the cookie fail to round-trip in
    # production, not just in tests. A known simplification, not an oversight.
    app.add_middleware(SessionMiddleware, secret_key=settings.session_secret_key or "dev-only-insecure-key")
```

Add the `require_login` dependency next to `require_token`:

```python
    async def require_token(authorization: str = Header(default="")) -> None:
        if not _token_ok(authorization, settings):
            raise HTTPException(status_code=401, detail="unauthorized")

    async def require_login(request: Request) -> None:
        if not request.session.get("user"):
            raise HTTPException(status_code=401, detail="unauthorized")
```

Add the login/logout routes (placed after `healthz`, before the memory routes):

```python
    @app.post("/login")
    async def login(request: Request, body: LoginIn) -> dict:
        if not await app.state.accounts.verify_password(body.username, body.password):
            raise HTTPException(status_code=401, detail="wrong username or password")
        request.session["user"] = body.username
        return {"status": "ok"}

    @app.post("/logout")
    async def logout(request: Request) -> dict:
        request.session.clear()
        return {"status": "ok"}
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `uv run pytest tests/test_main.py -v`
Expected: all pass, including the 4 new login/logout tests and every
pre-existing test in the file (the `client()` signature change is additive).

- [ ] **Step 7: Commit**

```bash
git add server/main.py server/config.py tests/fakes.py tests/test_main.py
git commit -m "Add session-cookie login, additive to the existing DEVICE_TOKEN"
```

---

## Task 3: `/ws` accepts a session cookie as well as the bearer token

**Files:**
- Modify: `server/main.py`
- Modify: `tests/test_main.py`

**Interfaces:**
- Consumes: `SessionMiddleware` from Task 2 (already installed app-wide).
- Produces: `_authorise_connection(ws, settings) -> str | None`, returning
  `"esp32"`, `"web"`, or `None`. Task 5 consumes the returned surface name
  to resolve which persona style a connection gets.

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_main.py`:

```python
def test_websocket_accepts_a_valid_session_cookie_with_no_bearer_token():
    with client(device_token="s3cret") as c:
        c.post("/login", json={"username": "test", "password": "test123"})
        with c.websocket_connect("/ws") as ws:
            ws.send_text(json.dumps({"type": "start"}))
            assert json.loads(ws.receive_text())["value"] == "listening"


def test_websocket_still_rejects_a_bad_bearer_with_no_session():
    with client(device_token="s3cret") as c, pytest.raises(WebSocketDisconnect):
        with c.websocket_connect("/ws", headers={"Authorization": "Bearer wrong"}) as ws:
            ws.receive_text()
```

- [ ] **Step 2: Run the tests to verify the first one fails**

Run: `uv run pytest tests/test_main.py -k "valid_session_cookie" -v`
Expected: FAIL — the connection is rejected (4401) because today's
`_authorised` only checks the bearer header. (The second test already
passes against today's code; it is here to lock in that the new code path
does not loosen it.)

- [ ] **Step 3: Replace `_authorised` with `_authorise_connection`**

In `server/main.py`, replace:

```python
def _authorised(ws: WebSocket, settings: Settings) -> bool:
    return _token_ok(ws.headers.get("authorization", ""), settings)
```

with:

```python
def _authorise_connection(ws: WebSocket, settings: Settings) -> str | None:
    """Which surface authorised this connection, or None if neither did.

    A real bearer token always wins as "esp32", regardless of whether
    settings.auth_required is even true - this is deliberately stricter than
    _token_ok's "no token configured means anyone passes" behaviour, so an
    open dev server does not silently mislabel a browser as the device.
    """
    header = ws.headers.get("authorization", "")
    scheme, _, token = header.partition(" ")
    if settings.device_token and scheme.lower() == "bearer" and token == settings.device_token:
        return "esp32"
    if ws.session.get("user"):
        return "web"
    if not settings.auth_required:
        return "esp32"
    return None
```

Update `ws_endpoint`'s opening lines from:

```python
    @app.websocket("/ws")
    async def ws_endpoint(websocket: WebSocket) -> None:
        if not _authorised(websocket, settings):
            log.warning("rejected unauthorised device")
            await websocket.close(code=4401)
            return
        await websocket.accept()
```

to:

```python
    @app.websocket("/ws")
    async def ws_endpoint(websocket: WebSocket) -> None:
        surface = _authorise_connection(websocket, settings)
        if surface is None:
            log.warning("rejected unauthorised connection")
            await websocket.close(code=4401)
            return
        await websocket.accept()
```

- [ ] **Step 4: Run the full test file to verify everything passes**

Run: `uv run pytest tests/test_main.py -v`
Expected: all pass, including both new tests and every pre-existing
websocket-auth test (`test_websocket_rejects_a_missing_token_when_auth_is_on`,
`test_websocket_rejects_a_wrong_token`, `test_websocket_accepts_the_right_token`,
`test_websocket_is_open_when_no_device_token_is_configured`).

- [ ] **Step 5: Commit**

```bash
git add server/main.py tests/test_main.py
git commit -m "Accept a session cookie as well as DEVICE_TOKEN on /ws"
```

---

## Task 4: A configurable, per-surface persona `Style`

**Files:**
- Modify: `server/persona.py`
- Modify: `server/session.py`
- Modify: `tests/test_persona.py`
- Modify: `tests/test_session.py`

**Interfaces:**
- Produces: `Style` (frozen dataclass: `max_sentences: int`,
  `markdown_allowed: bool`), module-level `ESP32` and `WEB` instances, and
  `build_system_prompt(facts: list[str], style: Style = ESP32) -> str`.
  `Session.__init__` gains a `style=ESP32` parameter, stored as
  `self.style`. Task 5 resolves which `Style` a connection gets and passes
  it into `Session(...)`.

- [ ] **Step 1: Write the failing persona tests**

Add to `tests/test_persona.py`:

```python
def test_default_style_is_esp32_and_matches_existing_wording():
    from server.persona import ESP32, build_system_prompt

    assert build_system_prompt([]) == build_system_prompt([], ESP32)


def test_web_style_allows_markdown():
    from server.persona import WEB, build_system_prompt

    prompt = build_system_prompt([], WEB).lower()
    assert "markdown, lists and headings are fine" in prompt


def test_esp32_style_still_forbids_markdown():
    from server.persona import ESP32, build_system_prompt

    prompt = build_system_prompt([], ESP32).lower()
    assert "no lists, no headings, no markdown" in prompt


def test_web_style_sentence_count_is_configurable():
    from server.persona import Style, build_system_prompt

    custom = Style(max_sentences=4, markdown_allowed=True)
    assert "up to 4 sentences" in build_system_prompt([], custom)


def test_web_style_still_asks_for_every_emotion():
    from server.emotion import EMOTIONS
    from server.persona import WEB, build_system_prompt

    prompt = build_system_prompt([], WEB)
    for name in EMOTIONS:
        assert f"[{name}]" in prompt, name
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `uv run pytest tests/test_persona.py -v`
Expected: FAIL — `ImportError: cannot import name 'ESP32' from 'server.persona'`

- [ ] **Step 3: Add `Style` and the web prompt variant to `server/persona.py`**

Add near the top, after the module docstring, before `BASE`:

```python
from dataclasses import dataclass


@dataclass(frozen=True)
class Style:
    max_sentences: int
    markdown_allowed: bool
```

Leave `BASE` completely untouched. After `BASE`'s closing `"""`, add:

```python
ESP32 = Style(max_sentences=2, markdown_allowed=False)
WEB = Style(max_sentences=6, markdown_allowed=True)


def _web_base(style: Style) -> str:
    length_rule = (
        f"Answer in up to {style.max_sentences} sentences."
        if style.max_sentences > 1
        else "Answer in one sentence."
    )
    markdown_rule = (
        "Markdown, lists and headings are fine here."
        if style.markdown_allowed
        else "No lists, no headings, no markdown."
    )
    return f"""You are a warm, direct assistant. Your replies are read on screen, not spoken aloud.

Rules:
- Start every reply with how you feel about it, in square brackets, before any \
words: [neutral] [happy] [excited] [curious] [confused] [surprised] [sad] \
[annoyed] [sleepy]. Exactly one, chosen from that list, always first. It drives \
an emotion indicator in the app, it is never shown as text, and you must never \
mention it.
- {length_rule} {markdown_rule}
- Detect the dominant language of the question and reply entirely in that \
language. Leave technical terms and proper nouns in their original form.
- Reply only in Ukrainian, Russian or English - the only voices available if \
this is ever read aloud. If the question appears to be in some other \
language it is a transcription error, so say briefly, in Ukrainian, that you \
did not catch it.
- Never mix two languages in one reply.
- Speak plainly. No preamble, no restating the question.
- If you do not know something, say so."""
```

Replace `build_system_prompt`:

```python
def build_system_prompt(facts: list[str], style: Style = ESP32) -> str:
    """Return the system prompt, with durable memory injected when present."""
    base = BASE if style is ESP32 else _web_base(style)
    if not facts:
        return base
    remembered = "\n".join(f"- {fact}" for fact in facts)
    return f"{base}\n\nWhat you remember about this person:\n{remembered}"
```

- [ ] **Step 4: Run the persona tests to verify they pass**

Run: `uv run pytest tests/test_persona.py -v`
Expected: all pass, including every pre-existing test (they all call
`build_system_prompt` with the default `style=ESP32`, which still produces
the byte-identical `BASE`-derived output).

- [ ] **Step 5: Write the failing `Session` tests**

Add to `tests/test_session.py`:

```python
async def test_default_style_is_esp32():
    from server.persona import ESP32

    session, _ = build()
    assert session.style is ESP32


async def test_web_style_reaches_the_system_prompt():
    from server.persona import WEB

    llm = FakeLLM()
    session = Session(
        transport=FakeTransport(),
        stt=FakeSTT(),
        llm=llm,
        tts=FakeTTS(),
        settings=Settings(_env_file=None),
        embedder=FakeEmbedder(),
        style=WEB,
    )
    await utter(session)
    assert "markdown" in llm.prompts[0][0]["content"].lower()
```

- [ ] **Step 6: Run the tests to verify they fail**

Run: `uv run pytest tests/test_session.py -k style -v`
Expected: FAIL — `TypeError: Session.__init__() got an unexpected keyword argument 'style'`

- [ ] **Step 7: Add `style` to `Session`**

In `server/session.py`, update the import line:

```python
from server.persona import ESP32, build_system_prompt
```

Update `Session.__init__`:

```python
class Session:
    def __init__(
        self, transport, stt, llm, tts, settings, store=None, device_id="default", embedder=None,
        style=ESP32,
    ):
        self.transport = transport
        self.stt = stt
        self.llm = llm
        self.tts = tts
        self.settings = settings
        self.store = store
        self.device_id = device_id
        self.embedder = embedder
        self.style = style
```

(Everything else in `__init__` is unchanged.) In `_respond`, change:

```python
        messages = build_messages(
            # persona.py takes one flat list; standing instructions come
            # first so a relevant fact never pushes a user's own rule out of
            # the prompt if both were ever truncated upstream.
            build_system_prompt(self.standing_instructions + self.facts),
            self.history,
            text,
            self.settings.max_context_tokens,
        )
```

to:

```python
        messages = build_messages(
            # persona.py takes one flat list; standing instructions come
            # first so a relevant fact never pushes a user's own rule out of
            # the prompt if both were ever truncated upstream.
            build_system_prompt(self.standing_instructions + self.facts, self.style),
            self.history,
            text,
            self.settings.max_context_tokens,
        )
```

- [ ] **Step 8: Run the full test suite to verify everything passes**

Run: `uv run pytest -q`
Expected: all pass (249 + the new tests from this task).

- [ ] **Step 9: Commit**

```bash
git add server/persona.py server/session.py tests/test_persona.py tests/test_session.py
git commit -m "Make persona a per-surface, configurable Style instead of one hardcoded prompt"
```

---

## Task 5: Persist style overrides and expose a settings API

**Files:**
- Modify: `server/memory/store.py`
- Modify: `server/main.py`
- Modify: `tests/fakes.py`
- Modify: `tests/test_store.py`
- Modify: `tests/test_main.py`

**Interfaces:**
- Consumes: `Style`, `ESP32`, `WEB` from `server/persona.py` (Task 4);
  `_authorise_connection` from `server/main.py` (Task 3); `require_login`
  from `server/main.py` (Task 2).
- Produces: `Store.get_style_override(surface) -> dict | None`,
  `Store.set_style_override(surface, max_sentences, markdown_allowed) -> None`.
  `GET/PUT /settings/style/{surface}`. `ws_endpoint` now resolves and passes
  a real `Style` into `Session(...)` instead of relying on the default.

- [ ] **Step 1: Write the failing `Store` tests**

Add to `tests/test_store.py`:

```python
async def test_get_style_override_is_none_by_default(store):
    assert await store.get_style_override("web") is None


async def test_set_style_override_roundtrips(store):
    await store.set_style_override("web", max_sentences=4, markdown_allowed=True)
    assert await store.get_style_override("web") == {"max_sentences": 4, "markdown_allowed": True}


async def test_set_style_override_twice_replaces_it(store):
    await store.set_style_override("web", max_sentences=4, markdown_allowed=True)
    await store.set_style_override("web", max_sentences=2, markdown_allowed=False)
    assert await store.get_style_override("web") == {"max_sentences": 2, "markdown_allowed": False}


async def test_style_overrides_are_independent_per_surface(store):
    await store.set_style_override("web", max_sentences=4, markdown_allowed=True)
    assert await store.get_style_override("esp32") is None
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `uv run pytest tests/test_store.py -k style_override -v`
Expected: FAIL — `AttributeError: 'Store' object has no attribute 'get_style_override'`

- [ ] **Step 3: Add the `StyleOverride` table and methods to `server/memory/store.py`**

Update the sqlalchemy import line (currently
`from sqlalchemy import Integer, Float, LargeBinary, String, DateTime, select, delete`)
to also import `Boolean`:

```python
from sqlalchemy import Integer, Float, LargeBinary, String, DateTime, Boolean, select, delete
```

Add a new model after `SessionUsage`:

```python
class StyleOverride(Base):
    __tablename__ = "style_overrides"
    surface: Mapped[str] = mapped_column(String(16), primary_key=True)
    max_sentences: Mapped[int] = mapped_column(Integer)
    markdown_allowed: Mapped[bool] = mapped_column(Boolean)
```

Add methods to `Store` (anywhere after `__init__`/`init`, e.g. after
`usage_rows`):

```python
    async def get_style_override(self, surface: str) -> dict | None:
        async with self._session() as s:
            row = await s.get(StyleOverride, surface)
            if row is None:
                return None
            return {"max_sentences": row.max_sentences, "markdown_allowed": row.markdown_allowed}

    async def set_style_override(self, surface: str, max_sentences: int, markdown_allowed: bool) -> None:
        async with self._session() as s:
            row = await s.get(StyleOverride, surface)
            if row is None:
                s.add(StyleOverride(
                    surface=surface, max_sentences=max_sentences, markdown_allowed=markdown_allowed
                ))
            else:
                row.max_sentences = max_sentences
                row.markdown_allowed = markdown_allowed
            await s.commit()
```

- [ ] **Step 4: Run the `Store` tests to verify they pass**

Run: `uv run pytest tests/test_store.py -v`
Expected: all pass.

- [ ] **Step 5: Add style methods to `FakeStore`**

In `tests/fakes.py`, add `self._styles: dict[str, dict] = {}` to
`FakeStore.__init__`, and these two methods to the class:

```python
    async def get_style_override(self, surface: str) -> dict | None:
        return self._styles.get(surface)

    async def set_style_override(self, surface: str, max_sentences: int, markdown_allowed: bool) -> None:
        self._styles[surface] = {"max_sentences": max_sentences, "markdown_allowed": markdown_allowed}
```

- [ ] **Step 6: Write the failing API tests**

Add to `tests/test_main.py`:

```python
def test_style_settings_requires_login():
    with client() as c:
        r = c.get("/settings/style/web")
    assert r.status_code == 401


def test_style_settings_roundtrips_after_login():
    with client() as c:
        c.post("/login", json={"username": "test", "password": "test123"})
        put = c.put("/settings/style/web", json={"max_sentences": 4, "markdown_allowed": True})
        assert put.status_code == 200
        got = c.get("/settings/style/web").json()
    assert got == {"surface": "web", "max_sentences": 4, "markdown_allowed": True}


def test_style_settings_default_before_any_override():
    with client() as c:
        c.post("/login", json={"username": "test", "password": "test123"})
        got = c.get("/settings/style/esp32").json()
    assert got == {"surface": "esp32", "max_sentences": 2, "markdown_allowed": False}


def test_a_web_style_override_does_not_affect_esp32():
    with client() as c:
        c.post("/login", json={"username": "test", "password": "test123"})
        c.put("/settings/style/web", json={"max_sentences": 4, "markdown_allowed": True})
        got = c.get("/settings/style/esp32").json()
    assert got == {"surface": "esp32", "max_sentences": 2, "markdown_allowed": False}
```

- [ ] **Step 7: Run the tests to verify they fail**

Run: `uv run pytest tests/test_main.py -k style_settings -v`
Expected: FAIL — 404, the routes don't exist yet.

- [ ] **Step 8: Add the settings API and wire style resolution into `ws_endpoint`**

In `server/main.py`, add this import line (near the existing
`server.providers.mock` import):

```python
from server.persona import ESP32, WEB, Style
```

Add a `StyleIn` model next to `LoginIn`:

```python
class StyleIn(BaseModel):
    max_sentences: int
    markdown_allowed: bool
```

Add a resolver function next to `_authorise_connection`:

```python
async def _resolve_style(store, surface: str) -> Style:
    default = ESP32 if surface == "esp32" else WEB
    if store is None:
        return default
    override = await store.get_style_override(surface)
    if override is None:
        return default
    return Style(max_sentences=override["max_sentences"], markdown_allowed=override["markdown_allowed"])
```

Add the routes (near the other `/memory` routes):

```python
    @app.get("/settings/style/{surface}", dependencies=[Depends(require_login)])
    async def get_style(surface: str) -> dict:
        style = await _resolve_style(app.state.store, surface)
        return {"surface": surface, "max_sentences": style.max_sentences, "markdown_allowed": style.markdown_allowed}

    @app.put("/settings/style/{surface}", dependencies=[Depends(require_login)])
    async def put_style(surface: str, body: StyleIn) -> dict:
        await app.state.store.set_style_override(surface, body.max_sentences, body.markdown_allowed)
        return {"status": "ok"}
```

Update `ws_endpoint` to resolve and pass the style:

```python
        device_id = websocket.query_params.get("device", "default")
        style = await _resolve_style(app.state.store, surface)
        session = Session(
            transport=WebSocketTransport(websocket),
            stt=app.state.stt,
            llm=app.state.llm,
            tts=app.state.tts,
            settings=settings,
            store=app.state.store,
            device_id=device_id,
            embedder=app.state.embedder,
            style=style,
        )
```

- [ ] **Step 9: Run the full test suite to verify everything passes**

Run: `uv run pytest -q`
Expected: all pass.

- [ ] **Step 10: Commit**

```bash
git add server/memory/store.py server/main.py tests/fakes.py tests/test_store.py tests/test_main.py
git commit -m "Persist per-surface style overrides behind a login-gated settings API"
```

---

## Task 6: `Session.on_text` — answer a typed question without STT or TTS

**Files:**
- Modify: `server/session.py`
- Modify: `tests/test_session.py`

**Interfaces:**
- Produces: `Session.on_text(text: str) -> None` (public, mirrors
  `on_start`/`on_end`/`on_cancel`). A typed reply arrives to the transport as
  `{"type": "reply", "value": "<sentence>"}` frames, never as binary audio.
  `_respond` and `on_text` now share one implementation, `_answer`, so
  neither can drift from the other.

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_session.py`:

```python
async def test_on_text_skips_stt_entirely():
    stt = FakeSTT()
    session, _ = build(stt=stt)
    await session.on_text("Привіт!")
    await session.wait_for_reply()
    assert stt.received == []


async def test_on_text_produces_a_written_reply_not_audio():
    session, transport = build(llm=FakeLLM("Добре."))
    await session.on_text("Привіт!")
    await session.wait_for_reply()
    assert transport.binary == []
    replies = [m["value"] for m in transport.json if m.get("type") == "reply"]
    assert replies == ["Добре."]


async def test_on_text_state_sequence_has_no_listening_phase():
    session, transport = build()
    await session.on_text("Привіт!")
    await session.wait_for_reply()
    assert transport.states == ["thinking", "speaking", "idle"]


async def test_on_text_interrupts_an_in_progress_reply():
    tts = SlowTTS(chunks=50, delay=0.01)
    session, _ = build(tts=tts)
    await session.on_start()
    await session.on_audio(b"\x00\x01" * 32000)
    await session.on_end()
    await asyncio.sleep(0.05)

    await session.on_text("Ще одне питання")
    assert session.state is State.THINKING


async def test_on_text_does_not_count_tts_chars_or_audio_seconds():
    session, _ = build(llm=FakeLLM("Добре."))
    await session.on_text("Привіт!")
    await session.wait_for_reply()
    assert session.usage.audio_seconds == 0
    assert session.usage.tts_chars == 0
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `uv run pytest tests/test_session.py -k on_text -v`
Expected: FAIL — `AttributeError: 'Session' object has no attribute 'on_text'`

- [ ] **Step 3: Split `_respond` into `_respond` + `_answer`, add `on_text`**

In `server/session.py`, replace the `_run_reply` method:

```python
    async def _run_reply(self, pcm: bytes) -> None:
        try:
            await self._respond(pcm)
        except asyncio.CancelledError:
            log.info("reply cancelled by the device")
            raise
        except Exception:
            log.exception("pipeline failed")
        finally:
            # The device has already stopped playing when it cancels, but it
            # still needs "done" to re-arm, and the socket may be gone.
            with contextlib.suppress(Exception):
                await self.transport.send_json({"type": "done"})
            with contextlib.suppress(Exception):
                await self._set_state(State.IDLE)
```

with a version that runs any coroutine, not just `_respond(pcm)`:

```python
    async def _run_reply(self, coro) -> None:
        try:
            await coro
        except asyncio.CancelledError:
            log.info("reply cancelled by the device")
            raise
        except Exception:
            log.exception("pipeline failed")
        finally:
            # The device has already stopped playing when it cancels, but it
            # still needs "done" to re-arm, and the socket may be gone.
            with contextlib.suppress(Exception):
                await self.transport.send_json({"type": "done"})
            with contextlib.suppress(Exception):
                await self._set_state(State.IDLE)
```

Update its one existing call site, in `on_end`:

```python
        self._reply = asyncio.create_task(self._run_reply(pcm))
```

to:

```python
        self._reply = asyncio.create_task(self._run_reply(self._respond(pcm)))
```

Add `on_text` next to `on_cancel`:

```python
    async def on_text(self, text: str) -> None:
        """A typed question. No audio, no STT - it is already text."""
        if self.state in (State.THINKING, State.SPEAKING):
            await self.on_cancel()
        await self._set_state(State.THINKING)
        self._reply = asyncio.create_task(self._run_reply(self._answer(text, speak=False)))
```

Replace `_respond` with a shorter version that hands off to `_answer`, and
add `_answer` containing everything `_respond` used to do after obtaining
`text`:

```python
    async def _respond(self, pcm: bytes) -> None:
        if not pcm:
            return  # nothing was captured; don't spend an STT call on silence

        # Whisper does not return nothing for a fragment of room tone. It
        # returns its training data: "Thank you.", "Спасибо.", "Продолжение
        # следует...". The model then answers those perfectly reasonably, so a
        # brushed button produced a device that said "Пожалуйста!" to
        # everything - observed in the logs, eight times in a row.
        #
        # The device cannot catch this on its own. It knows how long the button
        # was held; only this side knows how much audio actually arrived, and
        # under a second of it cannot be a question.
        seconds = len(pcm) / 2 / self.settings.sample_rate
        if seconds < self.settings.min_utterance_s:
            log.info("utterance of %.2f s is too short to be speech, not transcribing", seconds)
            return

        started = time.perf_counter()
        transcript = await self.stt.transcribe(pcm, self.settings.sample_rate)
        text = transcript.text.strip()
        if not text:
            log.info("empty transcript, skipping LLM")
            return
        log.info("stt %.0f ms: %s", (time.perf_counter() - started) * 1000, text)

        await self._answer(text, speak=True, audio_seconds=transcript.seconds)

    async def _answer(self, text: str, *, speak: bool = True, audio_seconds: float = 0.0) -> None:
        if self.store is not None:
            query_vector = await self.embedder.embed_query(text)
            self.facts = await self.store.relevant_facts(
                self.device_id, query_vector, limit=self.settings.relevant_facts_limit
            )

        messages = build_messages(
            # persona.py takes one flat list; standing instructions come
            # first so a relevant fact never pushes a user's own rule out of
            # the prompt if both were ever truncated upstream.
            build_system_prompt(self.standing_instructions + self.facts, self.style),
            self.history,
            text,
            self.settings.max_context_tokens,
        )
        prompt_tokens = sum(estimate_tokens(m["content"]) for m in messages)

        splitter = SentenceSplitter()
        # Sits ahead of the splitter: the model's feeling arrives as a tag on
        # the very first token, and nothing downstream should ever see it.
        tag = LeadingTag()
        spoken: list[str] = []
        voice: str | None = None
        language: str | None = None
        sent_bytes = 0
        reply_started = time.monotonic()
        # Reply audio goes back compressed too when the device asked for it.
        # A spoken answer is far larger than the question - 340 KB against
        # 15 KB - so the downlink benefits more from this than the uplink did.
        encoder = AdpcmEncoder() if self._codec == "adpcm" else None

        def is_speakable(sentence: str) -> bool:
            """True if there is anything for a voice to actually say.

            edge-tts raises NoAudioReceived for input with no pronounceable
            content - a stray bullet, a lone quotation mark, an emoji. One such
            fragment would otherwise abort the whole reply mid-sentence.
            """
            return any(ch.isalnum() for ch in sentence)

        async def say(sentence: str) -> None:
            nonlocal voice, language
            # Belt and braces. The sniffer takes the tag off the head of the
            # stream; this catches one the model put anywhere else, because
            # edge-tts will pronounce "curious" without hesitation.
            sentence = strip_tags(sentence)
            if not is_speakable(sentence):
                log.debug("skipping unspeakable fragment: %r", sentence)
                return
            if voice is None:
                # Decided once, from the first sentence: the voice must not
                # change partway through a reply.
                language = detect_language(sentence)
                voice = voice_for(language, self.settings.voices)
                # The face has to be right before the first word arrives, so
                # the emotion goes out ahead of the speaking state. By now the
                # tag has almost always resolved; when it has not, the first
                # sentence is a better thing to guess from than nothing.
                emotion = tag.emotion or from_text(sentence)
                await self.transport.send_json({"type": "emotion", "value": emotion})
                log.info("emotion %s (%s)", emotion,
                         "tagged" if tag.emotion else "guessed")
                await self._set_state(State.SPEAKING)
            spoken.append(sentence)

            if not speak:
                # Typed in, so written back - no TTS call spent on something
                # that is already being read.
                await self.transport.send_json({"type": "reply", "value": sentence})
                return

            async def render(with_voice: str) -> None:
                nonlocal sent_bytes
                loop = asyncio.get_running_loop()
                # Start on the short budget; relax it the moment audio arrives.
                async with asyncio.timeout(self.settings.tts_first_chunk_s) as limit:
                    started = False
                    async for pcm_chunk in self.tts.synthesise(sentence, with_voice):
                        if not started:
                            started = True
                            limit.reschedule(loop.time() + self.settings.tts_timeout_s)
                        # Pacing counts audio, not bytes on the wire, so the
                        # figure has to be taken before compression.
                        sent_bytes += len(pcm_chunk)
                        if encoder is not None:
                            pcm_chunk = encoder.feed(pcm_chunk)
                            if not pcm_chunk:
                                continue
                        await self.transport.send_bytes(pcm_chunk)

                        # Stay at most playback_lead_s ahead of what the device
                        # can have played by now.
                        bytes_per_second = self.settings.sample_rate * 2
                        audio_sent = sent_bytes / bytes_per_second
                        elapsed = time.monotonic() - reply_started
                        ahead = audio_sent - elapsed
                        if ahead > self.settings.playback_lead_s:
                            await asyncio.sleep(ahead - self.settings.playback_lead_s)

            try:
                await render(voice)
                return
            except Exception:
                log.warning("tts failed on %s, retrying with fallback voice", voice)

            # Second attempt with the other voice for this language. The
            # failure is per voice and per phrase, not per language, so the
            # alternate usually renders the same text without trouble.
            alt = self.settings.fallback_voices.get(language or "uk")
            if not alt or alt == voice:
                return
            try:
                await render(alt)
            except Exception:
                # One sentence the voice cannot render must not silence the
                # rest of the reply. edge-tts raises when the text does not
                # match the voice's language, which happens whenever the STT
                # misfires and the model answers in a language we did not pick
                # a voice for.
                log.warning("tts failed for %r, skipping sentence", sentence[:60])

        async for delta in self.llm.stream(messages, self.settings.max_tokens):
            for sentence in splitter.feed(tag.feed(delta)):
                await say(sentence)
        for sentence in splitter.feed(tag.flush()):
            await say(sentence)
        for sentence in splitter.flush():
            await say(sentence)

        if not spoken:
            # The model answers a garbled transcript with an empty string, and
            # an empty reply reaches the user as unexplained silence - which is
            # indistinguishable from the device being broken. Say so instead.
            # Do not trust the language of a transcript we already know is
            # garbled: "Raskarji, Karla." looks like English and is not. The
            # last reply that actually made sense is a far better guide.
            prior = next(
                (m["content"] for m in reversed(self.history) if m["role"] == "assistant"),
                "",
            )
            lang = detect_language(prior) if prior else DEFAULT
            fallback = _DIDNT_CATCH.get(lang, _DIDNT_CATCH[DEFAULT])
            log.info("empty reply for %r, asking to repeat", text[:40])
            await say(fallback)

        reply = " ".join(spoken)
        log.info(
            "reply %d chars, %d sentences, %d B audio in %.1f s: %r",
            len(reply), len(spoken), sent_bytes,
            time.monotonic() - reply_started, reply[:80],
        )
        self.history.append({"role": "user", "content": text})
        self.history.append({"role": "assistant", "content": reply})
        self.usage.add_turn(
            audio_seconds=audio_seconds,
            prompt_tokens=prompt_tokens,
            completion_tokens=estimate_tokens(reply),
            tts_chars=sum(len(s) for s in spoken) if speak else 0,
        )
```

- [ ] **Step 4: Run the full test suite to verify everything passes**

Run: `uv run pytest -q`
Expected: all pass — every existing voice-path test (`_respond` is now two
lines plus a call to `_answer`, behaviourally identical) plus the 5 new
`on_text` tests.

- [ ] **Step 5: Commit**

```bash
git add server/session.py tests/test_session.py
git commit -m "Add Session.on_text - one answer pipeline for typed and spoken questions"
```

---

## Task 7: Wire the `text` control frame into the WebSocket protocol

**Files:**
- Modify: `server/main.py`
- Modify: `tests/test_main.py`

**Interfaces:**
- Consumes: `Session.on_text(text: str)` from Task 6.
- Produces: a `{"type": "text", "value": "..."}` inbound WS frame is now a
  documented, working part of the protocol.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_main.py`:

```python
def test_text_frame_gets_a_written_reply_with_no_audio():
    with client() as c, c.websocket_connect("/ws") as ws:
        ws.send_text(json.dumps({"type": "text", "value": "Привіт!"}))
        seen, audio_frames = [], 0
        for _ in range(20):
            message = ws.receive()
            if message.get("bytes") is not None:
                audio_frames += 1
                continue
            payload = json.loads(message["text"])
            seen.append(payload)
            if payload.get("value") == "idle":
                break
    assert audio_frames == 0
    assert any(p.get("type") == "reply" for p in seen)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `uv run pytest tests/test_main.py -k text_frame -v`
Expected: FAIL — nothing responds to a `"text"` control frame today
(`log.debug("ignoring control message %r", kind)` swallows it silently, so
the test times out reading 20 messages without ever seeing `"idle"`).

- [ ] **Step 3: Dispatch `"text"` frames to `Session.on_text`**

In `server/main.py`'s `ws_endpoint`, change:

```python
                kind = control.get("type")
                if kind == "start":
                    await session.on_start(control.get("codec", "pcm16"))
                elif kind == "end":
                    await session.on_end()
                elif kind == "cancel":
                    await session.on_cancel()
                else:
                    log.debug("ignoring control message %r", kind)
```

to:

```python
                kind = control.get("type")
                if kind == "start":
                    await session.on_start(control.get("codec", "pcm16"))
                elif kind == "end":
                    await session.on_end()
                elif kind == "cancel":
                    await session.on_cancel()
                elif kind == "text":
                    await session.on_text(control.get("value", ""))
                else:
                    log.debug("ignoring control message %r", kind)
```

- [ ] **Step 4: Run the full test suite to verify everything passes**

Run: `uv run pytest -q`
Expected: all pass.

- [ ] **Step 5: Update the README's protocol table**

In `README.md`'s `## Protocol` section, add a row to the frame table (after
the existing `{"type":"end"}` row):

```markdown
| device → server | `{"type":"text","value":"..."}` | typed question, no audio to follow |
```

And after the existing paragraph about `DEVICE_TOKEN`, add:

```markdown
A typed question (`"text"`) gets a written `{"type":"reply","value":"..."}`
reply, streamed sentence by sentence like TTS is - no audio is synthesised
for it. A spoken question still gets a spoken reply, exactly as before.
```

- [ ] **Step 6: Commit**

```bash
git add server/main.py tests/test_main.py README.md
git commit -m "Wire the text control frame into the WebSocket dispatch loop"
```

---

## Manual verification (this plan's equivalent of the bench)

Automated tests use `FakeAccounts`/`FakeStore`/fake WS transports throughout
— nothing here has been run against a real browser or real cookies in a
real browser's cookie jar. Before trusting this:

1. `PROVIDER_MODE=mock uv run uvicorn server.main:app --port 8000`
2. `uv run python scripts/create_account.py --username you --password something`
3. `curl -X POST localhost:8000/login -d '{"username":"you","password":"something"}' -H 'Content-Type: application/json' -c cookies.txt`
4. `curl -b cookies.txt localhost:8000/settings/style/web` — should show the
   `WEB` defaults.
5. Open a websocket by hand (e.g. `wscat -c ws://localhost:8000/ws`, cookie
   header copied from `cookies.txt`) and send
   `{"type":"text","value":"Привіт, як справи?"}` — expect `state`,
   `emotion`, `reply` (not audio), `done`, `state:idle` frames back.
6. Confirm the ESP32 path is untouched: `uv run python scripts/fake_device.py --say "..."` still works exactly as before, unauthenticated by this session-cookie change.

## Self-review

**Spec coverage:** shared `device_id`/facts (unchanged, no new task needed —
already true) — ✓ preserved by construction (no task touches `device_id`
scoping). Configurable per-surface persona — ✓ Task 4/5. Additive login
alongside `DEVICE_TOKEN` — ✓ Task 2/3, verified by
`test_websocket_still_rejects_a_bad_bearer_with_no_session` and every
pre-existing bearer test staying green. Minimal user model (no
user→device table) — ✓ nothing in this plan adds one. Text-primary chat
reusing the existing pipeline — ✓ Task 6/7, one `_answer` implementation.
Typed replies default to text-only — ✓ `speak=False` in `on_text`.
`SESSION_SECRET_KEY` as a new required Railway var — ✓ Task 2, and flagged
again in this plan's constraints. Settings API for style — ✓ Task 5.
Retiring the old `GET /memory` page and building the React app itself are
explicitly the next plan, not this one.

**Placeholder scan:** no TBD/TODO; every step above has complete code, not
a description of code.

**Type consistency:** `Style(max_sentences: int, markdown_allowed: bool)` is
used with the same field names and order everywhere it appears (persona.py,
store.py's dict shape, main.py's `StyleIn`/`_resolve_style`). `Accounts`
methods (`create_user`, `verify_password`) are named identically in Task 1's
definition, Task 2's usage, and `FakeAccounts`'s matching interface.
`on_text`/`_answer`/`speak`/`audio_seconds` names match between their
definition in Task 6 and their only call sites (`on_text`, `_respond`, both
inside Task 6 itself — no other task calls them).
