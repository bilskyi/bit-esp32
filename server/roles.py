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
