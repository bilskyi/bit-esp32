"""Password accounts for the web login.

A separate module from server/memory/store.py on purpose: Store holds facts
and usage, a different bounded concern from who is allowed to log in. Same
SQLite file, same Store.__init__(url) pattern, its own table.
"""

import asyncio
from concurrent.futures import ThreadPoolExecutor

import bcrypt
from sqlalchemy import Integer, String, event, select
from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker, create_async_engine
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column


def _set_sqlite_pragmas(engine) -> None:
    """WAL plus a 5 s busy timeout on every new connection.

    synchronous=NORMAL is SQLite's own documented pairing for WAL: WAL
    already makes a transaction durable against an application crash once
    it is in the WAL file, so NORMAL's one dropped guarantee - surviving an
    OS crash or power loss between that write and the next checkpoint - is
    not a real loss for what is stored here, and it is what keeps a commit a
    single fsync of the WAL file instead of two. Left at the default FULL,
    every commit down all three engines syncs twice; that difference is
    invisible on Railway's disk but shows up as this project's SQLite tests
    going from single-digit seconds to a minute or more of pure I/O wait
    under this sandbox's syscall interception.

    Duplicated verbatim in server/memory/store.py (see its docstring for the
    full reasoning) and server/roles.py: Accounts is deliberately independent
    of both, so a shared module was rejected in favour of three copies kept
    in sync by inspection.
    """
    @event.listens_for(engine.sync_engine, "connect")
    def _pragmas(dbapi_connection, connection_record) -> None:
        cursor = dbapi_connection.cursor()
        cursor.execute("PRAGMA journal_mode=WAL")
        cursor.execute("PRAGMA synchronous=NORMAL")
        cursor.execute("PRAGMA busy_timeout=5000")
        cursor.close()


_BCRYPT_MAX_BYTES = 72

# bcrypt gets its own single-thread executor instead of asyncio.to_thread,
# which would share the loop's default thread-pool executor with
# server/providers/embeddings.py's embed_query() - and embed_query runs on
# *every* device turn, before the prompt is assembled. /login is
# unauthenticated, has no rate limit, and deliberately pays a full
# gensalt()+hashpw even for an unknown username (see verify_password below),
# so a handful of requests per second would otherwise queue bcrypt work
# behind - or in front of - the retrieval step the device's
# latency-to-first-audio budget depends on. Do not "simplify" this back to
# asyncio.to_thread; that is exactly the contention this avoids.
_BCRYPT_EXECUTOR = ThreadPoolExecutor(max_workers=1)


async def _run_bcrypt(fn):
    loop = asyncio.get_running_loop()
    return await loop.run_in_executor(_BCRYPT_EXECUTOR, fn)


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
        _set_sqlite_pragmas(self._engine)
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
        password_hash = await _run_bcrypt(
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
                await _run_bcrypt(lambda: bcrypt.hashpw(_bcrypt_bytes(password), bcrypt.gensalt()))
                return False
            return await _run_bcrypt(
                lambda: bcrypt.checkpw(_bcrypt_bytes(password), user.password_hash.encode("ascii"))
            )
