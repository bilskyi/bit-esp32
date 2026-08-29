import pytest

from server.accounts import Accounts


@pytest.fixture
async def accounts(tmp_path):
    a = Accounts(f"sqlite+aiosqlite:///{tmp_path}/test.db")
    await a.init()
    yield a
    await a.close()


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


async def test_password_is_not_stored_in_plain_text(accounts):
    from sqlalchemy import select

    from server.accounts import User

    await accounts.create_user("oleksandr", "correct-horse")
    async with accounts._session() as s:
        user = (await s.scalars(select(User).where(User.username == "oleksandr"))).first()
    assert "correct-horse" not in user.password_hash
