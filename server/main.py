"""FastAPI app: one WebSocket endpoint, one session per connection.

Binary frames are audio in both directions; text frames are JSON control
messages. See the protocol section of the README.
"""

import json
import logging
import secrets
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import Depends, FastAPI, Header, HTTPException, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse
from pydantic import BaseModel
from starlette.middleware.sessions import SessionMiddleware

from server.accounts import Accounts
from server.config import Settings
from server.memory.store import Store
from server.persona import ESP32, WEB, Style
from server.providers.edge_tts import EdgeTTS
from server.providers.embeddings import FastEmbedEmbedder
from server.providers.groq_llm import GroqLLM
from server.providers.groq_stt import GroqSTT
from server.providers.mock import MockEmbedder, MockLLM, MockSTT
from server.session import Session

log = logging.getLogger(__name__)


class WebSocketTransport:
    """Adapts a Starlette WebSocket to what Session needs."""

    def __init__(self, ws: WebSocket) -> None:
        self._ws = ws

    async def send_json(self, obj: dict) -> None:
        await self._ws.send_text(json.dumps(obj, ensure_ascii=False))

    async def send_bytes(self, data: bytes) -> None:
        await self._ws.send_bytes(data)


def _token_ok(header: str, settings: Settings) -> bool:
    if not settings.auth_required:
        return True
    scheme, _, token = header.partition(" ")
    return scheme.lower() == "bearer" and token == settings.device_token


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


async def _resolve_style(store, surface: str) -> Style:
    if surface == "esp32":
        # The ESP32's prompt is BASE, measured and protected (see RESUME.md) -
        # never overridable, and never even looked up, so a stray override
        # row (however it got there) can never affect it either.
        return ESP32
    if store is None:
        return WEB
    override = await store.get_style_override(surface)
    if override is None:
        return WEB
    return Style(max_sentences=override["max_sentences"], markdown_allowed=override["markdown_allowed"])


class LoginIn(BaseModel):
    username: str
    password: str


class StyleIn(BaseModel):
    max_sentences: int
    markdown_allowed: bool


class MemoryIn(BaseModel):
    text: str


# Read once at import, not per request - it's a static file, not a template.
_MEMORY_UI_HTML = (Path(__file__).parent / "static" / "memory.html").read_text()


def create_app(
    settings=None, stt=None, llm=None, tts=None, store=None, embedder=None, accounts=None
) -> FastAPI:
    """Build the app. Providers are injectable so tests need no network."""
    settings = settings or Settings()
    logging.basicConfig(level=settings.log_level.upper())

    injected = store is not None
    accounts_injected = accounts is not None

    # A fixed fallback key would mean anyone who has read this file can forge
    # a session cookie the moment a real deploy forgets to set the real one.
    # A random key generated once per process start closes that off entirely
    # - the cost is every restart invalidates existing logins, which is an
    # inconvenience, not a vulnerability.
    session_secret_key = settings.session_secret_key
    if not session_secret_key:
        session_secret_key = secrets.token_hex(32)
        log.warning("SESSION_SECRET_KEY is not set - using a random key for this process; "
                    "existing sessions will not survive a restart")

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
    app.add_middleware(
        SessionMiddleware,
        secret_key=session_secret_key,
        https_only=settings.session_cookie_secure,
        same_site="lax",
    )

    async def require_token(authorization: str = Header(default="")) -> None:
        if not _token_ok(authorization, settings):
            raise HTTPException(status_code=401, detail="unauthorized")

    async def require_login(request: Request) -> None:
        if not request.session.get("user"):
            raise HTTPException(status_code=401, detail="unauthorized")

    async def require_token_or_login(request: Request, authorization: str = Header(default="")) -> None:
        if _token_ok(authorization, settings):
            return
        if request.session.get("user"):
            return
        raise HTTPException(status_code=401, detail="unauthorized")

    @app.get("/healthz")
    async def healthz() -> dict:
        return {"status": "ok", "auth": settings.auth_required}

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

    @app.get("/memory", response_class=HTMLResponse)
    async def memory_ui() -> str:
        # The page itself carries no data - it only reveals anything once its
        # own fetch calls hit the endpoints below, which do check the token.
        return _MEMORY_UI_HTML

    @app.get("/memory/{device_id}", dependencies=[Depends(require_token_or_login)])
    async def list_memory(device_id: str) -> list[dict]:
        return await app.state.store.list_memory(device_id)

    @app.post("/memory/{device_id}", dependencies=[Depends(require_token_or_login)])
    async def add_memory(device_id: str, body: MemoryIn) -> dict:
        vector = await app.state.embedder.embed_documents([body.text])
        fact_id = await app.state.store.add_user_fact(device_id, body.text, vector[0])
        return {"id": fact_id}

    @app.delete("/memory/{device_id}/{fact_id}", dependencies=[Depends(require_token_or_login)])
    async def delete_memory_item(device_id: str, fact_id: int) -> dict:
        if not await app.state.store.delete_fact(device_id, fact_id):
            raise HTTPException(status_code=404, detail="not found")
        return {"status": "ok"}

    @app.delete("/memory/{device_id}", dependencies=[Depends(require_token_or_login)])
    async def clear_memory(device_id: str) -> dict:
        await app.state.store.forget(device_id)
        return {"status": "ok"}

    @app.get("/settings/style/{surface}", dependencies=[Depends(require_login)])
    async def get_style(surface: str) -> dict:
        style = await _resolve_style(app.state.store, surface)
        return {"surface": surface, "max_sentences": style.max_sentences, "markdown_allowed": style.markdown_allowed}

    @app.put("/settings/style/{surface}", dependencies=[Depends(require_login)])
    async def put_style(surface: str, body: StyleIn) -> dict:
        if surface == "esp32":
            raise HTTPException(status_code=400, detail="esp32's prompt is fixed and cannot be overridden")
        await app.state.store.set_style_override(surface, body.max_sentences, body.markdown_allowed)
        return {"status": "ok"}

    @app.websocket("/ws")
    async def ws_endpoint(websocket: WebSocket) -> None:
        surface = _authorise_connection(websocket, settings)
        if surface is None:
            log.warning("rejected unauthorised connection")
            await websocket.close(code=4401)
            return
        await websocket.accept()

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
        await session.load_memory()

        try:
            while True:
                message = await websocket.receive()
                if message["type"] == "websocket.disconnect":
                    break
                if message.get("bytes") is not None:
                    await session.on_audio(message["bytes"])
                    continue
                text = message.get("text")
                if text is None:
                    continue
                try:
                    control = json.loads(text)
                except json.JSONDecodeError:
                    log.warning("ignoring malformed control frame")
                    continue
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
        except WebSocketDisconnect:
            pass
        finally:
            try:
                await session.finish()
            except Exception:
                log.exception("session teardown failed")

    return app



# ASGI entry point for uvicorn: `uvicorn server.main:app`
app = create_app()
