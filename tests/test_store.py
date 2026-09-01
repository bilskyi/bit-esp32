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


# --------------------------------------------------------- ranked retrieval

async def test_relevant_facts_ranks_by_similarity_not_recency(store):
    """Three facts, deliberately inserted so that the correct answer matches
    neither insertion order nor its reverse.

    With two facts, any ordering the sort produces is also produced by some
    trivial ORDER BY, so the test passed even with the sort removed. The
    middle fact is what makes the assertion about similarity rather than
    about rowid.
    """
    await store.add_facts(
        "dev1",
        ["Lives in Kyiv", "Has a cat named Musya", "Likes short answers"],
        [[0.0, 1.0], [1.0, 0.0], [0.7, 0.7]],
    )
    ranked = await store.relevant_facts("dev1", [1.0, 0.0])
    assert [text for text, _ in ranked] == [
        "Has a cat named Musya",
        "Likes short answers",
        "Lives in Kyiv",
    ]
    assert ranked[0][1] > ranked[1][1] > ranked[2][1]
    assert ranked[0][1] == pytest.approx(1.0)


async def test_relevant_facts_respects_the_limit(store):
    await store.add_facts("dev1", ["a", "b", "c"], [[1.0, 0.0], [0.9, 0.1], [0.0, 1.0]])
    assert len(await store.relevant_facts("dev1", [1.0, 0.0], limit=2)) == 2


async def test_relevant_facts_ignores_other_devices(store):
    await store.add_facts("dev1", ["mine"], [[1.0, 0.0]])
    await store.add_facts("dev2", ["theirs"], [[1.0, 0.0]])
    assert [t for t, _ in await store.relevant_facts("dev1", [1.0, 0.0])] == ["mine"]


async def test_relevant_facts_skips_rows_with_no_embedding(store):
    await store.add_facts("dev1", ["unembedded"])
    assert await store.relevant_facts("dev1", [1.0, 0.0]) == []


async def test_relevant_facts_with_no_query_is_empty(store):
    await store.add_facts("dev1", ["a"], [[1.0, 0.0]])
    assert await store.relevant_facts("dev1", None) == []


# --------------------------------------------------- user-authored standing instructions

async def test_user_facts_are_empty_by_default(store):
    assert await store.user_facts("dev1") == []


async def test_add_user_fact_makes_it_a_standing_instruction(store):
    await store.add_user_fact("dev1", "Always answer informally")
    assert await store.user_facts("dev1") == ["Always answer informally"]


async def test_user_facts_do_not_appear_in_auto_retrieval(store):
    """Standing instructions are never ranked - they always apply, so they
    must not compete with auto facts for the top-K slots."""
    await store.add_user_fact("dev1", "Always answer informally", embedding=[1.0, 0.0])
    assert [t for t, _ in await store.relevant_facts("dev1", [1.0, 0.0])] == []


async def test_list_memory_shows_both_kinds_with_their_source(store):
    await store.add_facts("dev1", ["An auto fact"], embeddings=[[1.0, 0.0]])
    await store.add_user_fact("dev1", "A user instruction")
    rows = await store.list_memory("dev1")
    sources = {r["text"]: r["source"] for r in rows}
    assert sources == {"An auto fact": "auto", "A user instruction": "user"}


async def test_delete_fact_removes_only_that_row(store):
    keep_id = await store.add_user_fact("dev1", "keep me")
    drop_id = await store.add_user_fact("dev1", "drop me")
    assert await store.delete_fact("dev1", drop_id) is True
    remaining = {r["id"] for r in await store.list_memory("dev1")}
    assert remaining == {keep_id}


async def test_delete_fact_returns_false_for_an_unknown_id(store):
    assert await store.delete_fact("dev1", 999) is False


async def test_delete_fact_is_scoped_per_device(store):
    other_id = await store.add_user_fact("dev2", "belongs to dev2")
    assert await store.delete_fact("dev1", other_id) is False
    assert await store.user_facts("dev2") == ["belongs to dev2"]


# ------------------------------------------------------------ embedding backfill

async def test_backfill_embeddings_fills_only_missing_rows(store):
    await store.add_facts("dev1", ["already embedded"], embeddings=[[1.0, 0.0]])
    await store.add_facts("dev1", ["needs embedding"])  # embeddings=None

    calls: list[list[str]] = []

    async def embed(texts: list[str]) -> list[list[float]]:
        calls.append(texts)
        return [[0.5, 0.5] for _ in texts]

    updated = await store.backfill_embeddings(embed)
    assert updated == 1
    assert calls == [["needs embedding"]]
    # Now retrievable, proving the write actually landed.
    ranked = await store.relevant_facts("dev1", [0.5, 0.5])
    assert "needs embedding" in [text for text, _ in ranked]


async def test_backfill_embeddings_is_idempotent(store):
    await store.add_facts("dev1", ["needs embedding"])

    async def embed(texts: list[str]) -> list[list[float]]:
        return [[1.0, 0.0] for _ in texts]

    assert await store.backfill_embeddings(embed) == 1
    assert await store.backfill_embeddings(embed) == 0
