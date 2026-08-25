import pytest

from server.costs import Usage
from server.memory.store import Store


@pytest.fixture
async def store(tmp_path):
    s = Store(f"sqlite+aiosqlite:///{tmp_path}/test.db")
    await s.init()
    yield s
    await s.close()


async def test_recent_facts_is_empty_for_an_unknown_device(store):
    assert await store.recent_facts("nobody") == []


async def test_facts_round_trip(store):
    await store.add_facts("dev1", ["Lives in Kyiv", "Prefers short answers"])
    assert set(await store.recent_facts("dev1")) == {"Lives in Kyiv", "Prefers short answers"}


async def test_facts_are_scoped_per_device(store):
    await store.add_facts("dev1", ["Fact A"])
    await store.add_facts("dev2", ["Fact B"])
    assert await store.recent_facts("dev1") == ["Fact A"]
    assert await store.recent_facts("dev2") == ["Fact B"]


async def test_recent_facts_returns_newest_first(store):
    await store.add_facts("dev1", ["older"])
    await store.add_facts("dev1", ["newer"])
    assert (await store.recent_facts("dev1"))[0] == "newer"


async def test_recent_facts_respects_the_limit(store):
    await store.add_facts("dev1", [f"fact {i}" for i in range(30)])
    assert len(await store.recent_facts("dev1", limit=5)) == 5


async def test_duplicate_facts_are_not_stored_twice(store):
    await store.add_facts("dev1", ["Lives in Kyiv"])
    await store.add_facts("dev1", ["Lives in Kyiv"])
    assert await store.recent_facts("dev1") == ["Lives in Kyiv"]


async def test_usage_is_persisted(store):
    u = Usage()
    u.add_turn(audio_seconds=3.0, prompt_tokens=400, completion_tokens=60, tts_chars=90)
    await store.log_usage("dev1", u)
    rows = await store.usage_rows("dev1")
    assert len(rows) == 1
    assert rows[0]["turns"] == 1
    assert rows[0]["prompt_tokens"] == 400


async def test_init_is_safe_to_run_twice(store):
    await store.init()
    assert await store.recent_facts("dev1") == []
