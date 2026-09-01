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
from datetime import datetime, timedelta, timezone
from typing import Awaitable, Callable

from sqlalchemy import Integer, Float, LargeBinary, String, DateTime, Text, select, delete
from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker, create_async_engine
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column

from server.costs import Usage


def _now() -> datetime:
    return datetime.now(timezone.utc)


def _aware(dt: datetime | None) -> datetime | None:
    """SQLite always hands datetimes back naive, even through a column
    declared DateTime(timezone=True). conversation_rows() is the one path
    Task 7 added for the planned dashboard, and code there that treats a
    naive value as UTC will hit `TypeError: can't compare offset-naive and
    offset-aware datetimes` the moment it compares one - so make it true
    instead of leaving that landmine. The other timestamp paths (Fact,
    SessionUsage) predate this and are left alone."""
    if dt is None or dt.tzinfo is not None:
        return dt
    return dt.replace(tzinfo=timezone.utc)


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
    ) -> list[tuple[str, float]]:
        """Auto facts and their cosine similarity to `query_embedding`, best first.

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
        return scored[:limit]

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

    async def delete_conversations(self, device_id: str) -> None:
        """Delete a device's recorded conversations and their messages,
        without touching its facts.

        Its own method, not folded silently into forget(): the new
        DELETE /conversations endpoint exists precisely so transcripts can
        be dropped independently of the facts extracted from them.
        """
        async with self._session() as s:
            conversation_ids = list(
                await s.scalars(
                    select(Conversation.id).where(Conversation.device_id == device_id)
                )
            )
            for start in range(0, len(conversation_ids), self._PURGE_BATCH_SIZE):
                batch = conversation_ids[start : start + self._PURGE_BATCH_SIZE]
                await s.execute(delete(Message).where(Message.conversation_id.in_(batch)))
            await s.execute(delete(Conversation).where(Conversation.device_id == device_id))
            await s.commit()

    async def forget(self, device_id: str) -> None:
        """Forget a device entirely: its facts, and every transcript of it.

        Before this deleted only Fact rows, so a verbatim record of every
        conversation survived a "wipe this device's memory" call - the
        person doing the wiping would believe the data was gone while it
        stayed on the volume. `forget` is documented (README) as the whole
        wipe; delete_conversations is what to call instead when only the
        transcripts, not the facts, should go.
        """
        async with self._session() as s:
            await s.execute(delete(Fact).where(Fact.device_id == device_id))
            await s.commit()
        await self.delete_conversations(device_id)

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
                    "started_at": _aware(c.started_at),
                    "ended_at": _aware(c.ended_at),
                    "turns": sum(1 for m in messages if m.role == "user"),
                    "messages": [
                        {"role": m.role, "text": m.text, "emotion": m.emotion}
                        for m in messages
                    ],
                })
            return out

    # SQLite refuses a statement with more bound parameters than it was built
    # to allow - 32766 on a modern build, 999 on an older one. A single
    # `WHERE id IN (...)` over every expired conversation can blow past
    # either, and when it does the whole sweep raises before deleting
    # anything, so every later startup hits the identical wall. Chunking
    # keeps each statement's parameter count far under even the old limit.
    _PURGE_BATCH_SIZE = 500

    async def purge_expired(self) -> int:
        """Delete messages past the retention window, and any conversation
        they leave empty and already ended. Returns the total number of
        rows deleted across both tables.

        Keyed on Message.created_at, not Conversation.started_at: one
        conversation row is one WebSocket connection, and the ESP32 holds a
        single long-lived socket, so a conversation can span days or weeks.
        Keying expiry on when it started would delete a 91-day-old row
        wholesale - yesterday's turns included - while a still-open row
        would never become eligible at all. A conversation is only dropped
        once every message in it is gone *and* it has actually ended: a
        conversation with zero messages so far because it just started is
        not expired, it just hasn't been talked to yet.

        `retention_days = 0` means keep forever, which is why this reads the
        setting rather than taking a parameter: the switch and the sweep must
        agree, and there is exactly one place to set it.
        """
        settings = await self.app_settings()
        days = settings["retention_days"]
        if days <= 0:
            return 0
        cutoff = _now() - timedelta(days=days)
        deleted = 0
        async with self._session() as s:
            doomed_messages = list(
                await s.scalars(select(Message.id).where(Message.created_at < cutoff))
            )
            for start in range(0, len(doomed_messages), self._PURGE_BATCH_SIZE):
                batch = doomed_messages[start : start + self._PURGE_BATCH_SIZE]
                result = await s.execute(delete(Message).where(Message.id.in_(batch)))
                deleted += result.rowcount
            await s.commit()

            # NOT EXISTS rather than a Python-side id list: the surviving
            # side of this can be far larger than the doomed side (most
            # conversations keep most of their messages), and that list
            # would reintroduce the exact bound-parameter wall this method
            # exists to avoid.
            no_messages_left = ~(
                select(Message.id).where(Message.conversation_id == Conversation.id).exists()
            )
            empty_and_ended = list(
                await s.scalars(
                    select(Conversation.id).where(
                        Conversation.ended_at.is_not(None), no_messages_left
                    )
                )
            )
            for start in range(0, len(empty_and_ended), self._PURGE_BATCH_SIZE):
                batch = empty_and_ended[start : start + self._PURGE_BATCH_SIZE]
                result = await s.execute(delete(Conversation).where(Conversation.id.in_(batch)))
                deleted += result.rowcount
            await s.commit()
        return deleted

