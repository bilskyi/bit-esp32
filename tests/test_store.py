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


# ------------------------------------------------------- conversation history

async def test_app_settings_default_to_recording_on_for_ninety_days(store):
    assert await store.app_settings() == {"store_conversations": True, "retention_days": 90}


async def test_app_settings_round_trip(store):
    await store.set_app_settings(store_conversations=False, retention_days=7)
    assert await store.app_settings() == {"store_conversations": False, "retention_days": 7}


async def test_setting_one_app_setting_leaves_the_other(store):
    await store.set_app_settings(retention_days=30)
    settings = await store.app_settings()
    assert settings == {"store_conversations": True, "retention_days": 30}


async def test_a_recorded_turn_keeps_both_halves(store):
    cid = await store.start_conversation("dev1", "web", "Web default")
    await store.record_turn(cid, question="Де я живу?", reply="У Чернівцях.",
                            emotion="neutral", prompt_tokens=120,
                            completion_tokens=8, latency_ms=845.0)
    rows = await store.conversation_rows("dev1")
    assert len(rows) == 1
    assert rows[0]["surface"] == "web"
    assert rows[0]["role_name"] == "Web default"
    assert rows[0]["turns"] == 1
    assert rows[0]["messages"][0] == {"role": "user", "text": "Де я живу?", "emotion": None}
    assert rows[0]["messages"][1]["role"] == "assistant"
    assert rows[0]["messages"][1]["text"] == "У Чернівцях."
    assert rows[0]["messages"][1]["emotion"] == "neutral"


async def test_conversation_rows_started_at_is_timezone_aware(store):
    """SQLite hands datetimes back naive even though the column is declared
    DateTime(timezone=True). A dashboard reading conversation_rows() and
    treating started_at as UTC-aware would otherwise hit
    `TypeError: can't compare offset-naive and offset-aware datetimes`."""
    await store.start_conversation("dev1", "web", "Web default")
    rows = await store.conversation_rows("dev1")
    assert rows[0]["started_at"].tzinfo is not None


async def test_conversations_are_scoped_per_device(store):
    await store.start_conversation("dev1", "web", "Web default")
    assert await store.conversation_rows("dev2") == []


async def test_ending_a_conversation_stamps_it(store):
    cid = await store.start_conversation("dev1", "esp32", "Device default")
    await store.end_conversation(cid)
    assert (await store.conversation_rows("dev1"))[0]["ended_at"] is not None


# ------------------------------------------------------ deleting transcripts

async def test_delete_conversations_removes_everything_for_that_device(store):
    cid = await store.start_conversation("dev1", "web", "Web default")
    await store.record_turn(cid, question="q", reply="r", emotion=None,
                            prompt_tokens=1, completion_tokens=1, latency_ms=1.0)
    await store.delete_conversations("dev1")
    assert await store.conversation_rows("dev1") == []


async def test_delete_conversations_leaves_another_devices_alone(store):
    cid1 = await store.start_conversation("dev1", "web", "Web default")
    await store.record_turn(cid1, question="q", reply="r", emotion=None,
                            prompt_tokens=1, completion_tokens=1, latency_ms=1.0)
    cid2 = await store.start_conversation("dev2", "web", "Web default")
    await store.record_turn(cid2, question="q2", reply="r2", emotion=None,
                            prompt_tokens=1, completion_tokens=1, latency_ms=1.0)
    await store.delete_conversations("dev1")
    assert await store.conversation_rows("dev1") == []
    assert len(await store.conversation_rows("dev2")) == 1


async def test_delete_conversations_leaves_facts_intact(store):
    """DELETE /conversations/{device_id} exists precisely so transcripts can
    be dropped without dropping the facts extracted from them."""
    cid = await store.start_conversation("dev1", "web", "Web default")
    await store.record_turn(cid, question="q", reply="r", emotion=None,
                            prompt_tokens=1, completion_tokens=1, latency_ms=1.0)
    await store.add_facts("dev1", ["a fact"])
    await store.delete_conversations("dev1")
    assert await store.recent_facts("dev1") == ["a fact"]


async def test_forget_deletes_conversations_and_messages_too(store):
    """Store.forget is documented (README) as forgetting a device entirely.
    Before this fix it deleted only Fact rows, so a verbatim transcript of
    every conversation survived a 'wipe this device's memory' call - the
    owner would believe the data was gone when it was still on the volume."""
    cid = await store.start_conversation("dev1", "web", "Web default")
    await store.record_turn(cid, question="q", reply="r", emotion=None,
                            prompt_tokens=1, completion_tokens=1, latency_ms=1.0)
    await store.add_facts("dev1", ["a fact"])
    await store.forget("dev1")
    assert await store.conversation_rows("dev1") == []
    assert await store.recent_facts("dev1") == []


async def test_forget_leaves_another_devices_conversations_alone(store):
    cid1 = await store.start_conversation("dev1", "web", "Web default")
    await store.record_turn(cid1, question="q", reply="r", emotion=None,
                            prompt_tokens=1, completion_tokens=1, latency_ms=1.0)
    cid2 = await store.start_conversation("dev2", "web", "Web default")
    await store.record_turn(cid2, question="q2", reply="r2", emotion=None,
                            prompt_tokens=1, completion_tokens=1, latency_ms=1.0)
    await store.forget("dev1")
    assert await store.conversation_rows("dev1") == []
    assert len(await store.conversation_rows("dev2")) == 1


async def test_purge_removes_conversations_past_retention(store):
    """Keyed on Message.created_at, not Conversation.started_at: the
    conversation must also have ended before it is eligible - a message
    going stale is not the same thing as the socket having closed."""
    from datetime import datetime, timedelta, timezone

    from sqlalchemy import select

    from server.memory.store import Message

    cid = await store.start_conversation("dev1", "web", "Web default")
    await store.record_turn(cid, question="q", reply="r", emotion=None,
                            prompt_tokens=1, completion_tokens=1, latency_ms=1.0)
    await store.end_conversation(cid)
    await store.set_app_settings(retention_days=1)
    async with store._session() as s:  # noqa: SLF001 - fixture-level surgery
        old = datetime.now(timezone.utc) - timedelta(days=5)
        rows = (await s.scalars(select(Message).where(Message.conversation_id == cid))).all()
        for m in rows:
            m.created_at = old
        await s.commit()

    assert await store.purge_expired() == 3  # both messages, then the empty conversation
    assert await store.conversation_rows("dev1") == []


async def test_purge_keeps_conversations_inside_retention(store):
    cid = await store.start_conversation("dev1", "web", "Web default")
    await store.record_turn(cid, question="q", reply="r", emotion=None,
                            prompt_tokens=1, completion_tokens=1, latency_ms=1.0)
    assert await store.purge_expired() == 0
    assert len(await store.conversation_rows("dev1")) == 1


async def test_a_long_lived_conversation_loses_only_its_expired_messages(store):
    """One conversation row is one WebSocket connection, and the ESP32 holds
    a single long-lived socket - so a conversation can span days, with some
    messages older than the retention window and others still fresh. Purge
    must not delete the row wholesale just because it is old, and must not
    spare a message just because its conversation is still around."""
    from datetime import datetime, timedelta, timezone

    from sqlalchemy import select

    from server.memory.store import Message

    cid = await store.start_conversation("dev1", "web", "Web default")
    await store.record_turn(cid, question="old q", reply="old r", emotion=None,
                            prompt_tokens=1, completion_tokens=1, latency_ms=1.0)
    await store.record_turn(cid, question="recent q", reply="recent r", emotion=None,
                            prompt_tokens=1, completion_tokens=1, latency_ms=1.0)
    await store.set_app_settings(retention_days=1)
    async with store._session() as s:  # noqa: SLF001 - fixture-level surgery
        old = datetime.now(timezone.utc) - timedelta(days=5)
        old_messages = (
            await s.scalars(
                select(Message)
                .where(Message.conversation_id == cid)
                .order_by(Message.id.asc())
                .limit(2)
            )
        ).all()
        for m in old_messages:
            m.created_at = old
        await s.commit()

    deleted = await store.purge_expired()
    assert deleted == 2  # only the expired turn's two messages

    rows = await store.conversation_rows("dev1")
    assert len(rows) == 1  # the conversation itself survives
    assert {m["text"] for m in rows[0]["messages"]} == {"recent q", "recent r"}


async def test_retention_of_zero_days_never_purges(store):
    from datetime import datetime, timedelta, timezone

    from sqlalchemy import select

    from server.memory.store import Message

    cid = await store.start_conversation("dev1", "web", "Web default")
    await store.record_turn(cid, question="q", reply="r", emotion=None,
                            prompt_tokens=1, completion_tokens=1, latency_ms=1.0)
    await store.end_conversation(cid)
    await store.set_app_settings(retention_days=0)
    async with store._session() as s:  # noqa: SLF001 - fixture-level surgery
        old = datetime.now(timezone.utc) - timedelta(days=4000)
        rows = (await s.scalars(select(Message).where(Message.conversation_id == cid))).all()
        for m in rows:
            m.created_at = old
        await s.commit()
    assert await store.purge_expired() == 0


async def test_purge_batches_deletes_past_sqlites_bound_parameter_limit(store):
    """One un-chunked `WHERE id IN (...)` over every expired id blows past
    SQLite's bound-parameter ceiling (32766 in the build this test runs
    against) and raises `OperationalError`, aborting before anything is
    deleted - so every later startup hits the same wall. This seeds enough
    expired, ended conversations (one old message each) to cross that
    ceiling for real, on both the message delete and the follow-up
    conversation delete, and asserts the sweep still purges every row, in
    one call, without orphaning a single message - while a conversation
    still inside retention is left alone."""
    from datetime import datetime, timedelta, timezone

    from sqlalchemy import insert, select

    from server.memory.store import Conversation, Message

    kept_cid = await store.start_conversation("dev1", "web", "Web default")
    await store.record_turn(kept_cid, question="q", reply="r", emotion=None,
                            prompt_tokens=1, completion_tokens=1, latency_ms=1.0)

    old = datetime.now(timezone.utc) - timedelta(days=200)
    count = 33000  # comfortably past this SQLite build's 32766-variable ceiling
    async with store._session() as s:  # noqa: SLF001 - fixture-level bulk seeding
        await s.execute(
            insert(Conversation),
            [
                {"device_id": "dev1", "surface": "web", "role_name": "Web default",
                 "ended_at": old}
                for _ in range(count)
            ],
        )
        await s.commit()
        doomed_ids = (
            await s.scalars(
                select(Conversation.id).where(Conversation.id != kept_cid)
            )
        ).all()
        await s.execute(
            insert(Message),
            [{"conversation_id": i, "role": "user", "text": "q", "created_at": old}
             for i in doomed_ids],
        )
        await s.commit()

    await store.set_app_settings(retention_days=1)
    assert await store.purge_expired() == count * 2  # each doomed message, then its conversation

    async with store._session() as s:  # noqa: SLF001
        remaining_ids = set((await s.scalars(select(Conversation.id))).all())
        remaining_messages = (await s.scalars(select(Message.id))).all()
    assert remaining_ids == {kept_cid}
    assert len(remaining_messages) == 2  # the kept conversation's own turn
