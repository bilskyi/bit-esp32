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
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field
from starlette.middleware.gzip import GZipMiddleware
from starlette.middleware.sessions import SessionMiddleware

from server.accounts import Accounts
from server.config import Settings
from server.memory.store import Store
from server.providers.edge_tts import EdgeTTS
from server.providers.embeddings import FastEmbedEmbedder
from server.providers.groq_llm import GroqLLM
from server.providers.groq_stt import GroqSTT
from server.providers.mock import MockEmbedder, MockLLM, MockSTT
from server.roles import SPEAKABLE_LANGUAGES, NameTaken, Roles
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


class LoginIn(BaseModel):
    username: str
    password: str


class MemoryIn(BaseModel):
    text: str


class AppSettingsIn(BaseModel):
    store_conversations: bool | None = None
    retention_days: int | None = None


class RoleIn(BaseModel):
    name: str
    # build_messages (server/context.py) trims history to fit the token
    # budget but never trims the system prompt, so an unbounded persona
    # would be sent in full on every device turn against MAX_CONTEXT_TOKENS.
    # 2000 characters is ample for a persona section; SQLite itself does not
    # enforce RoleRow.prompt's String(4000), so this is the only real limit.
    prompt: str | None = Field(default=None, max_length=2000)
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
    prompt: str | None = Field(default=None, max_length=2000)
    max_sentences: int | None = None
    markdown_allowed: bool | None = None
    languages: list[str] | None = None
    pinned_mood: str | None = None


class ActiveRoleIn(BaseModel):
    role_id: int


# The only two clients that exist. server/roles.py's own default map has the
# same two keys; this copy stays local because main.py is what answers 404
# for the rest, and importing a private mapping just to get its keys would
# be more coupling than the two literals are worth.
_SURFACES = ("esp32", "web")


def _role_json(role) -> dict:
    return {
        "id": role.id,
        "name": role.name,
        "prompt": role.prompt,
        "max_sentences": role.max_sentences,
        "markdown_allowed": role.markdown_allowed,
        "languages": list(role.languages),
        "pinned_mood": role.pinned_mood,
        "built_in": role.built_in,
    }


def create_app(
    settings=None, stt=None, llm=None, tts=None, store=None, embedder=None, accounts=None,
    roles=None,
) -> FastAPI:
    """Build the app. Providers are injectable so tests need no network."""
    settings = settings or Settings()
    logging.basicConfig(level=settings.log_level.upper())

    injected = store is not None
    accounts_injected = accounts is not None
    roles_injected = roles is not None

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
        app.state.roles = roles
        if not roles_injected:
            app.state.roles = Roles(f"sqlite+aiosqlite:///{settings.db_path}")
        await app.state.roles.init()
        await app.state.roles.ensure_defaults()
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
            purged = await app.state.store.purge_expired()
            if purged:
                log.info("purged %d rows past the retention window", purged)
        yield
        if not injected and app.state.store is not None:
            await app.state.store.close()
        if not accounts_injected and app.state.accounts is not None:
            await app.state.accounts.close()
        if not roles_injected and app.state.roles is not None:
            await app.state.roles.close()

    app = FastAPI(title="voice-companion", lifespan=lifespan)
    app.state.settings = settings
    # Nothing was compressed before this. Starlette's FileResponse and
    # StaticFiles never compress, so the face frames went over the wire as
    # 1.2 MB of base64 - which quietly voided the reason base64 was kept in
    # the first place ("gzipped they are 42 KB, and the wire cost is what
    # matters"). It was only true if something actually gzipped them.
    app.add_middleware(GZipMiddleware, minimum_size=1000)
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

    @app.get("/me")
    async def me(request: Request) -> dict:
        user = request.session.get("user")
        if not user:
            raise HTTPException(status_code=401, detail="unauthorized")
        return {"username": user}

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

    @app.delete("/conversations/{device_id}", dependencies=[Depends(require_login)])
    async def clear_conversations(device_id: str) -> dict:
        await app.state.store.delete_conversations(device_id)
        return {"status": "ok"}

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
        except NameTaken as exc:
            raise HTTPException(status_code=409, detail=str(exc)) from exc
        except ValueError as exc:
            # A language with no voice or a mood that is not a face is an
            # unprocessable value. NameTaken (a conflict) is caught above -
            # it must be, since _validate's messages interpolate the
            # client's own values and could otherwise be mistaken for one.
            raise HTTPException(status_code=422, detail=str(exc)) from exc
        return {"id": role.id}

    @app.put("/roles/{role_id}", dependencies=[Depends(require_login)])
    async def update_role(role_id: int, body: RolePatch) -> dict:
        fields = {}
        for name in body.model_fields_set:
            value = getattr(body, name)
            fields[name] = tuple(value) if name == "languages" and value is not None else value
        try:
            role = await app.state.roles.update(role_id, **fields)
        except NameTaken as exc:
            raise HTTPException(status_code=409, detail=str(exc)) from exc
        except ValueError as exc:
            raise HTTPException(status_code=422, detail=str(exc)) from exc
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
            for surface in _SURFACES
        }

    @app.put("/settings/surfaces/{surface}", dependencies=[Depends(require_login)])
    async def set_surface_role(surface: str, body: ActiveRoleIn) -> dict:
        # Without this, an unknown surface (a typo, a future third client
        # named wrong) still writes a row into surface_roles - it just never
        # shows up in GET /settings/surfaces, which only ever asks about
        # esp32 and web.
        if surface not in _SURFACES:
            raise HTTPException(status_code=404, detail="not found")
        if not await app.state.roles.set_active(surface, body.role_id):
            raise HTTPException(status_code=404, detail="not found")
        return {"status": "ok"}

    @app.get("/settings/app", dependencies=[Depends(require_login)])
    async def get_app_settings() -> dict:
        return await app.state.store.app_settings()

    @app.put("/settings/app", dependencies=[Depends(require_login)])
    async def put_app_settings(body: AppSettingsIn) -> dict:
        return await app.state.store.set_app_settings(
            store_conversations=body.store_conversations,
            retention_days=body.retention_days,
        )

    @app.websocket("/ws")
    async def ws_endpoint(websocket: WebSocket) -> None:
        surface = _authorise_connection(websocket, settings)
        if surface is None:
            log.warning("rejected unauthorised connection")
            await websocket.close(code=4401)
            return
        await websocket.accept()

        device_id = websocket.query_params.get("device", "default")
        role = await app.state.roles.active_for(surface)
        session = Session(
            transport=WebSocketTransport(websocket),
            stt=app.state.stt,
            llm=app.state.llm,
            tts=app.state.tts,
            settings=settings,
            store=app.state.store,
            device_id=device_id,
            embedder=app.state.embedder,
            role=role,
            surface=surface,
            roles=app.state.roles,
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

    # Registered last, so every API route above wins. The catch-all only
    # answers GETs that look like navigation: anything else, and anything
    # under a known API prefix, must keep returning a JSON 404 rather than
    # an HTML page a fetch() would try to parse.
    _DIST = (
        Path(settings.web_dist)
        if settings.web_dist
        else Path(__file__).parent.parent / "web" / "dist"
    )
    _API_PREFIXES = (
        "login", "logout", "me", "roles", "settings", "memory",
        "conversations", "healthz", "ws",
    )
    if _DIST.is_dir():
        app.mount("/assets", StaticFiles(directory=_DIST / "assets"), name="assets")

        @app.get("/{path:path}", include_in_schema=False)
        async def spa(path: str):
            if path.split("/", 1)[0] in _API_PREFIXES:
                raise HTTPException(status_code=404, detail="not found")
            # Vite emits favicons, manifests and anything else in public/ at
            # the root of the bundle, not under assets/. Without this, the
            # catch-all answered every one of them with index.html - a
            # favicon request got a 200 of HTML.
            if path:
                candidate = (_DIST / path).resolve()
                if candidate.is_file() and candidate.is_relative_to(_DIST.resolve()):
                    return FileResponse(candidate)
            return FileResponse(_DIST / "index.html")
    else:
        log.warning("web/dist is missing - the API is up but there is no app to serve; "
                    "run `npm --prefix web run build`")

    return app



# ASGI entry point for uvicorn: `uvicorn server.main:app`
app = create_app()
