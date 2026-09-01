import asyncio

import pytest

from server.accounts import Accounts


@pytest.fixture
async def accounts(tmp_path):
    a = Accounts(f"sqlite+aiosqlite:///{tmp_path}/test.db")
    await a.init()
    yield a
    await a.close()


async def test_wal_and_a_busy_timeout_are_set_on_every_connection(accounts):
    from sqlalchemy import text

    async with accounts._engine.connect() as conn:
        mode = (await conn.execute(text("PRAGMA journal_mode"))).scalar()
        timeout = (await conn.execute(text("PRAGMA busy_timeout"))).scalar()
    assert mode == "wal"
    assert timeout == 5000


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


async def test_a_password_over_72_bytes_does_not_crash_hashing_or_checking(accounts):
    long_pw = "п" * 40  # 80 bytes in UTF-8, over bcrypt's 72-byte limit
    await accounts.create_user("oleksandr", long_pw)
    assert await accounts.verify_password("oleksandr", long_pw) is True


async def test_a_wrong_password_over_72_bytes_is_still_rejected(accounts):
    await accounts.create_user("oleksandr", "п" * 40)
    assert await accounts.verify_password("oleksandr", "х" * 40) is False


async def test_password_is_not_stored_in_plain_text(accounts):
    from sqlalchemy import select

    from server.accounts import User

    await accounts.create_user("oleksandr", "correct-horse")
    async with accounts._session() as s:
        user = (await s.scalars(select(User).where(User.username == "oleksandr"))).first()
    assert "correct-horse" not in user.password_hash


async def test_bcrypt_runs_on_its_own_dedicated_executor_not_the_default_pool(
    accounts, monkeypatch,
):
    """/login is unauthenticated and unrated, and an unknown username pays the
    full gensalt()+hashpw cost on purpose (see the timing-indistinguishability
    comment in verify_password). If bcrypt shared the default thread-pool
    executor, a burst of /login requests would queue behind - or block - the
    embed_query() call that runs on every device turn (server/providers/
    embeddings.py), delaying retrieval on the path the device's
    latency-to-first-audio budget depends on. Both correct-password and
    wrong-password verification, and the unknown-username branch, must all
    run on the dedicated executor instead."""
    from server import accounts as accounts_module

    await accounts.create_user("oleksandr", "correct-horse")

    loop = asyncio.get_running_loop()
    seen_executors = []
    original_run_in_executor = loop.run_in_executor

    def spy(executor, fn, *args):
        seen_executors.append(executor)
        return original_run_in_executor(executor, fn, *args)

    monkeypatch.setattr(loop, "run_in_executor", spy)

    assert await accounts.verify_password("oleksandr", "correct-horse") is True
    assert await accounts.verify_password("oleksandr", "wrong") is False
    assert await accounts.verify_password("nobody", "whatever") is False

    assert seen_executors, "bcrypt never went through run_in_executor"
    assert all(e is accounts_module._BCRYPT_EXECUTOR for e in seen_executors)
    assert accounts_module._BCRYPT_EXECUTOR is not None
    assert accounts_module._BCRYPT_EXECUTOR._max_workers == 1
