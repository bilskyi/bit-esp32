"""SQLite persistence for durable facts and per-session usage.

Facts are injected into the system prompt at the start of the next session.
Vector search would be premature below a couple hundred facts; newest-first
with a limit is enough and costs no RAM.
"""

from datetime import datetime, timezone

from sqlalchemy import Integer, Float, String, DateTime, select, delete
from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker, create_async_engine
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column

from server.costs import Usage


def _now() -> datetime:
    return datetime.now(timezone.utc)


class Base(DeclarativeBase):
    pass


class Fact(Base):
    __tablename__ = "facts"
    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    device_id: Mapped[str] = mapped_column(String(64), index=True)
    text: Mapped[str] = mapped_column(String(500))
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


class Store:
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

    async def add_facts(self, device_id: str, facts: list[str]) -> None:
        """Store new facts, skipping ones already known for this device."""
        clean = [f.strip() for f in facts if f and f.strip()]
        if not clean:
            return
        async with self._session() as s:
            known = set(
                (
                    await s.scalars(select(Fact.text).where(Fact.device_id == device_id))
                ).all()
            )
            s.add_all(
                [Fact(device_id=device_id, text=f) for f in clean if f not in known]
            )
            await s.commit()

    async def recent_facts(self, device_id: str, limit: int = 20) -> list[str]:
        async with self._session() as s:
            rows = await s.scalars(
                select(Fact.text)
                .where(Fact.device_id == device_id)
                .order_by(Fact.id.desc())
                .limit(limit)
            )
            return list(rows.all())

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
