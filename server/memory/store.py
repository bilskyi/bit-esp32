"""SQLite persistence for durable facts and per-session usage.

Facts are injected into the system prompt at the start of the next session.
Two kinds live in the same table, told apart by `source`:

- "auto" facts come from end-of-session extraction and are ranked by
  embedding similarity to the live question (`relevant_facts`) - real vector
  search was premature below a couple hundred of these, but the user asked
  for it now as the base a bigger memory can grow into later.
- "user" facts are standing instructions typed through the HTTP API
  (main.py) - "always answer informally" is not a fact to rank by
  relevance, so these are returned in full, every turn (`user_facts`).

Embedding is intentionally not this module's job: Store stays dependency-free
(SQLAlchemy only) and only stores and ranks vectors it is handed. Whatever
calls this embeds elsewhere (server/session.py, via an injected Embedder).
"""

import math
import struct
from datetime import datetime, timezone
from typing import Awaitable, Callable

from sqlalchemy import Integer, Float, LargeBinary, String, DateTime, Boolean, select, delete
from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker, create_async_engine
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column

from server.costs import Usage


def _now() -> datetime:
    return datetime.now(timezone.utc)


def _pack(vector: list[float]) -> bytes:
    return struct.pack(f"<{len(vector)}f", *vector)


def _unpack(blob: bytes) -> list[float]:
    count = len(blob) // 4
    return list(struct.unpack(f"<{count}f", blob))


def _cosine(a: list[float], b: list[float]) -> float:
    dot = sum(x * y for x, y in zip(a, b))
    norm_a = math.sqrt(sum(x * x for x in a))
    norm_b = math.sqrt(sum(x * x for x in b))
    if not norm_a or not norm_b:
        return 0.0
    return dot / (norm_a * norm_b)


class Base(DeclarativeBase):
    pass


class Fact(Base):
    __tablename__ = "facts"
    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    device_id: Mapped[str] = mapped_column(String(64), index=True)
    text: Mapped[str] = mapped_column(String(500))
    source: Mapped[str] = mapped_column(String(16), default="auto")
    embedding: Mapped[bytes | None] = mapped_column(LargeBinary, nullable=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=_now)


class SessionUsage(Base):
    __tablename__ = "session_usage"
    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    device_id: Mapped[str] = mapped_column(String(64), index=True)
    turns: Mapped[int] = mapped_column(Integer, default=0)
    audio_seconds: Mapped[float] = mapped_column(Float, default=0.0)
    prompt_tokens: Mapped[int] = mapped_column(Integer, default=0)
    completion_tokens: Mapped[int] = mapped_column(Integer, default=0)
    tts_chars: Mapped[int] = mapped_column(Integer, default=0)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=_now)


class StyleOverride(Base):
    __tablename__ = "style_overrides"
    surface: Mapped[str] = mapped_column(String(16), primary_key=True)
    max_sentences: Mapped[int] = mapped_column(Integer)
    markdown_allowed: Mapped[bool] = mapped_column(Boolean)


class Store:
    def __init__(self, url: str) -> None:
        self._engine = create_async_engine(url, future=True)
        self._session: async_sessionmaker[AsyncSession] = async_sessionmaker(
            self._engine, expire_on_commit=False
        )

    async def init(self) -> None:
        async with self._engine.begin() as conn:
            await conn.run_sync(Base.metadata.create_all)
            # SQLite has no "ADD COLUMN IF NOT EXISTS"; a facts table from
            # before `source`/`embedding` existed needs these added once.
            # Broad except is deliberate: the only failure mode on a second
            # run is "duplicate column", and SQLite's wording for that is not
            # worth pattern-matching for a two-statement migration.
            for stmt in (
                "ALTER TABLE facts ADD COLUMN source VARCHAR(16) DEFAULT 'auto'",
                "ALTER TABLE facts ADD COLUMN embedding BLOB",
            ):
                try:
                    await conn.exec_driver_sql(stmt)
                except Exception:
                    pass

    async def close(self) -> None:
        await self._engine.dispose()

    async def add_facts(
        self,
        device_id: str,
        facts: list[str],
        embeddings: list[list[float]] | None = None,
    ) -> None:
        """Store new auto-extracted facts, skipping ones already known.

        `embeddings`, if given, must line up 1:1 with `facts` before
        deduplication - both are filtered together so a fact's vector never
        drifts from its text.
        """
        pairs = list(zip(facts, embeddings)) if embeddings is not None else [(f, None) for f in facts]
        pairs = [(f.strip(), e) for f, e in pairs if f and f.strip()]
        if not pairs:
            return
        async with self._session() as s:
            known = set(
                (
                    await s.scalars(select(Fact.text).where(Fact.device_id == device_id))
                ).all()
            )
            s.add_all(
                [
                    Fact(
                        device_id=device_id,
                        text=f,
                        source="auto",
                        embedding=_pack(e) if e is not None else None,
                    )
                    for f, e in pairs
                    if f not in known
                ]
            )
            await s.commit()

    async def add_user_fact(
        self, device_id: str, text: str, embedding: list[float] | None = None
    ) -> int:
        """Store one standing instruction, typed by the user, not extracted."""
        async with self._session() as s:
            fact = Fact(
                device_id=device_id,
                text=text.strip(),
                source="user",
                embedding=_pack(embedding) if embedding is not None else None,
            )
            s.add(fact)
            await s.commit()
            await s.refresh(fact)
            return fact.id

    async def user_facts(self, device_id: str) -> list[str]:
        """Standing instructions, all of them, in the order they were added.

        There is no ranking here: "always answer informally" is a rule, not
        a fact to judge for relevance against the current question.
        """
        async with self._session() as s:
            rows = await s.scalars(
                select(Fact.text)
                .where(Fact.device_id == device_id, Fact.source == "user")
                .order_by(Fact.id.asc())
            )
            return list(rows.all())

    async def recent_facts(self, device_id: str, limit: int = 20) -> list[str]:
        async with self._session() as s:
            rows = await s.scalars(
                select(Fact.text)
                .where(Fact.device_id == device_id)
                .order_by(Fact.id.desc())
                .limit(limit)
            )
            return list(rows.all())

    async def relevant_facts(
        self, device_id: str, query_embedding: list[float] | None, limit: int = 6
    ) -> list[str]:
        """Auto facts ranked by cosine similarity to `query_embedding`.

        Brute-force, in Python: at the row counts a single device's memory
        will ever reach, this is far cheaper than standing up a vector index
        would be, and it needs no extra dependency to do it.
        """
        if not query_embedding:
            return []
        async with self._session() as s:
            rows = await s.execute(
                select(Fact.text, Fact.embedding).where(
                    Fact.device_id == device_id,
                    Fact.source == "auto",
                    Fact.embedding.is_not(None),
                )
            )
            scored = [
                (text, _cosine(query_embedding, _unpack(blob)))
                for text, blob in rows.all()
            ]
        scored.sort(key=lambda pair: pair[1], reverse=True)
        return [text for text, _ in scored[:limit]]

    async def list_memory(self, device_id: str) -> list[dict]:
        """Everything remembered for a device, for the customization API."""
        async with self._session() as s:
            rows = await s.scalars(
                select(Fact)
                .where(Fact.device_id == device_id)
                .order_by(Fact.id.asc())
            )
            return [
                {
                    "id": f.id,
                    "text": f.text,
                    "source": f.source,
                    "created_at": f.created_at,
                }
                for f in rows.all()
            ]

    async def delete_fact(self, device_id: str, fact_id: int) -> bool:
        async with self._session() as s:
            result = await s.execute(
                delete(Fact).where(Fact.device_id == device_id, Fact.id == fact_id)
            )
            await s.commit()
            return result.rowcount > 0

    async def backfill_embeddings(
        self, embed_fn: Callable[[list[str]], Awaitable[list[list[float]]]]
    ) -> int:
        """Embed any row left over from before `embedding` existed.

        Runs once at startup (see main.py's lifespan). Without this, a fact
        stored before this feature shipped - the one already living on the
        Railway volume - would be permanently invisible to `relevant_facts`.
        """
        async with self._session() as s:
            rows = (
                await s.execute(
                    select(Fact.id, Fact.text).where(Fact.embedding.is_(None))
                )
            ).all()
            if not rows:
                return 0
            vectors = await embed_fn([text for _, text in rows])
            for (fact_id, _), vector in zip(rows, vectors):
                await s.execute(
                    Fact.__table__.update()
                    .where(Fact.id == fact_id)
                    .values(embedding=_pack(vector))
                )
            await s.commit()
            return len(rows)

    async def forget(self, device_id: str) -> None:
        async with self._session() as s:
            await s.execute(delete(Fact).where(Fact.device_id == device_id))
            await s.commit()

    async def log_usage(self, device_id: str, usage: Usage) -> None:
        async with self._session() as s:
            s.add(
                SessionUsage(
                    device_id=device_id,
                    turns=usage.turns,
                    audio_seconds=usage.audio_seconds,
                    prompt_tokens=usage.prompt_tokens,
                    completion_tokens=usage.completion_tokens,
                    tts_chars=usage.tts_chars,
                )
            )
            await s.commit()

    async def usage_rows(self, device_id: str, limit: int = 50) -> list[dict]:
        async with self._session() as s:
            rows = await s.scalars(
                select(SessionUsage)
                .where(SessionUsage.device_id == device_id)
                .order_by(SessionUsage.id.desc())
                .limit(limit)
            )
            return [
                {
                    "turns": r.turns,
                    "audio_seconds": r.audio_seconds,
                    "prompt_tokens": r.prompt_tokens,
                    "completion_tokens": r.completion_tokens,
                    "tts_chars": r.tts_chars,
                    "created_at": r.created_at,
                }
                for r in rows.all()
            ]

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
