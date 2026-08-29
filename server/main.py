"""FastAPI app: one WebSocket endpoint, one session per connection.

Binary frames are audio in both directions; text frames are JSON control
messages. See the protocol section of the README.
"""

import json
import logging
from contextlib import asynccontextmanager

from fastapi import Depends, FastAPI, Header, HTTPException, WebSocket, WebSocketDisconnect
from pydantic import BaseModel

from server.config import Settings
from server.memory.store import Store
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


def _authorised(ws: WebSocket, settings: Settings) -> bool:
    return _token_ok(ws.headers.get("authorization", ""), settings)


class MemoryIn(BaseModel):
    text: str


def create_app(settings=None, stt=None, llm=None, tts=None, store=None, embedder=None) -> FastAPI:
    """Build the app. Providers are injectable so tests need no network."""
    settings = settings or Settings()
    logging.basicConfig(level=settings.log_level.upper())

    injected = store is not None

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        app.state.store = store
        if not injected:
            app.state.store = Store(f"sqlite+aiosqlite:///{settings.db_path}")
            await app.state.store.init()
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

    app = FastAPI(title="voice-companion", lifespan=lifespan)
    app.state.settings = settings

    async def require_token(authorization: str = Header(default="")) -> None:
        if not _token_ok(authorization, settings):
            raise HTTPException(status_code=401, detail="unauthorized")

    @app.get("/healthz")
    async def healthz() -> dict:
        return {"status": "ok", "auth": settings.auth_required}

    @app.get("/memory/{device_id}", dependencies=[Depends(require_token)])
    async def list_memory(device_id: str) -> list[dict]:
        return await app.state.store.list_memory(device_id)

    @app.post("/memory/{device_id}", dependencies=[Depends(require_token)])
    async def add_memory(device_id: str, body: MemoryIn) -> dict:
        vector = await app.state.embedder.embed_documents([body.text])
        fact_id = await app.state.store.add_user_fact(device_id, body.text, vector[0])
        return {"id": fact_id}

    @app.delete("/memory/{device_id}/{fact_id}", dependencies=[Depends(require_token)])
    async def delete_memory_item(device_id: str, fact_id: int) -> dict:
        if not await app.state.store.delete_fact(device_id, fact_id):
            raise HTTPException(status_code=404, detail="not found")
        return {"status": "ok"}

    @app.delete("/memory/{device_id}", dependencies=[Depends(require_token)])
    async def clear_memory(device_id: str) -> dict:
        await app.state.store.forget(device_id)
        return {"status": "ok"}

    @app.websocket("/ws")
    async def ws_endpoint(websocket: WebSocket) -> None:
        if not _authorised(websocket, settings):
            log.warning("rejected unauthorised device")
            await websocket.close(code=4401)
            return
        await websocket.accept()

        device_id = websocket.query_params.get("device", "default")
        session = Session(
            transport=WebSocketTransport(websocket),
            stt=app.state.stt,
            llm=app.state.llm,
            tts=app.state.tts,
            settings=settings,
            store=app.state.store,
            device_id=device_id,
            embedder=app.state.embedder,
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
