"""Password accounts for the web login.

A separate module from server/memory/store.py on purpose: Store holds facts
and usage, a different bounded concern from who is allowed to log in. Same
SQLite file, same Store.__init__(url) pattern, its own table.
"""

import asyncio

import bcrypt
from sqlalchemy import Integer, String, select
from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker, create_async_engine
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column


_BCRYPT_MAX_BYTES = 72


def _bcrypt_bytes(password: str) -> bytes:
    """bcrypt only ever looks at the first 72 bytes of input. Truncating
    explicitly here means hashing and checking always agree, instead of
    depending on whether the installed bcrypt version truncates silently or
    raises - and a password over the limit is common in this project's own
    languages: 37 Cyrillic characters is already 74 bytes.
    """
    return password.encode("utf-8")[:_BCRYPT_MAX_BYTES]


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
        password_hash = await asyncio.to_thread(
            lambda: bcrypt.hashpw(_bcrypt_bytes(password), bcrypt.gensalt()).decode("ascii")
        )
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
                await asyncio.to_thread(lambda: bcrypt.hashpw(_bcrypt_bytes(password), bcrypt.gensalt()))
                return False
            return await asyncio.to_thread(
                lambda: bcrypt.checkpw(_bcrypt_bytes(password), user.password_hash.encode("ascii"))
            )
