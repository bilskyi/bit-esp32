import asyncio

import pytest

from server.config import Settings
from server.session import Session
from tests.fakes import FakeLLM, FakeSTT, FakeTransport, FakeTTS


def build(stt=None, llm=None, tts=None, **kw):
    transport = FakeTransport()
    session = Session(
        transport=transport,
        stt=stt or FakeSTT(),
        llm=llm or FakeLLM(),
        tts=tts or FakeTTS(),
        settings=Settings(_env_file=None, **kw),
    )
    return session, transport


async def utter(session, audio=b"\x00\x01" * 1600):
    await session.on_start()
    await session.on_audio(audio)
    await session.on_end()


async def test_start_announces_listening():
    session, transport = build()
    await session.on_start()
    assert transport.states == ["listening"]


async def test_audio_arriving_before_start_is_discarded():
    stt = FakeSTT()
    session, _ = build(stt=stt)
    await session.on_audio(b"\x00\x01" * 100)
    await session.on_start()
    await session.on_end()
    assert stt.received == []


async def test_state_sequence_covers_the_whole_turn():
    session, transport = build()
    await utter(session)
    assert transport.states == ["listening", "thinking", "speaking", "idle"]


async def test_reply_audio_is_sent_as_binary_frames():
    session, transport = build()
    await utter(session)
    assert transport.binary
    assert all(isinstance(f, bytes) for f in transport.binary)


async def test_done_is_sent_once_playback_finishes():
    session, transport = build()
    await utter(session)
    assert transport.types.count("done") == 1
    assert transport.types[-2:] == ["done", "state"]


async def test_transcribed_audio_reaches_stt():
    stt = FakeSTT()
    session, _ = build(stt=stt)
    await utter(session, audio=b"\x00\x01" * 1600)
    assert stt.received == [3200]


async def test_each_sentence_is_synthesised_separately():
    tts = FakeTTS()
    session, _ = build(llm=FakeLLM("Перше речення. Друге речення."), tts=tts)
    await utter(session)
    assert [t for t, _ in tts.spoken] == ["Перше речення.", "Друге речення."]


async def test_one_voice_is_used_for_the_whole_reply():
    """Switching voice mid-reply is audible and jarring."""
    tts = FakeTTS()
    session, _ = build(llm=FakeLLM("Привіт як справи. Another sentence here."), tts=tts)
    await utter(session)
    assert len(set(tts.voices)) == 1
    assert tts.voices[0].startswith("uk-UA-")


async def test_blank_transcript_skips_the_llm_entirely():
    llm = FakeLLM()
    session, transport = build(stt=FakeSTT(text="   "), llm=llm)
    await utter(session)
    assert llm.prompts == []
    assert transport.states == ["listening", "thinking", "idle"]
    assert "done" in transport.types


async def test_history_carries_into_the_next_turn():
    llm = FakeLLM("Добре.")
    session, _ = build(llm=llm)
    await utter(session)
    await utter(session)
    second = llm.prompts[-1]
    assert [m["role"] for m in second] == ["system", "user", "assistant", "user"]
    assert second[2]["content"] == "Добре."


async def test_utterance_is_capped_so_a_stuck_button_cannot_grow_forever():
    stt = FakeSTT()
    session, _ = build(stt=stt, session_timeout_s=1)  # 1 s == 32000 bytes
    await session.on_start()
    for _ in range(10):
        await session.on_audio(b"\x00\x01" * 5000)  # 10 KB each, 100 KB total
    await session.on_end()
    assert stt.received == [32000]


async def test_end_without_start_is_ignored():
    llm = FakeLLM()
    session, transport = build(llm=llm)
    await session.on_end()
    assert llm.prompts == []
    assert transport.json == []


async def test_reply_respects_the_configured_token_cap():
    llm = FakeLLM()
    session, _ = build(llm=llm, max_tokens=42)
    await utter(session)
    assert session.settings.max_tokens == 42


async def test_a_stuck_button_ends_the_utterance_without_a_release():
    session, transport = build(session_timeout_s=0.05)
    await session.on_start()
    await session.on_audio(b"\x00\x01" * 100)
    await asyncio.sleep(0.25)
    assert transport.states[-1] == "idle"
    assert "done" in transport.types


async def test_remembered_facts_reach_the_system_prompt():
    from tests.fakes import FakeStore

    llm = FakeLLM()
    transport = FakeTransport()
    session = Session(
        transport=transport,
        stt=FakeSTT(),
        llm=llm,
        tts=FakeTTS(),
        settings=Settings(_env_file=None),
        store=FakeStore(["Lives in Kyiv"]),
    )
    await session.load_memory()
    await utter(session)
    assert "Lives in Kyiv" in llm.prompts[0][0]["content"]


async def test_finish_extracts_and_stores_facts():
    from tests.fakes import FakeStore

    store = FakeStore()
    llm = FakeLLM("Добре.")
    llm.completions = ['["Likes short answers"]']
    session, _ = build(llm=llm)
    session.store = store
    await utter(session)
    llm.completions = ['["Likes short answers"]']
    await session.finish()
    assert store.added == [("default", ["Likes short answers"])]


async def test_finish_logs_usage():
    from tests.fakes import FakeStore

    store = FakeStore()
    llm = FakeLLM("Добре.")
    session, _ = build(llm=llm)
    session.store = store
    await utter(session)
    llm.completions = ["[]"]
    await session.finish()
    assert len(store.usage_logged) == 1
    assert store.usage_logged[0][1].turns == 1


async def test_finish_on_an_empty_session_stores_nothing():
    from tests.fakes import FakeStore

    store = FakeStore()
    session, _ = build()
    session.store = store
    await session.finish()
    assert store.added == [] and store.usage_logged == []


async def test_usage_counts_audio_seconds_and_tts_chars():
    session, _ = build(llm=FakeLLM("Добре."))
    await utter(session, audio=b"\x00\x01" * 16000)  # 1 second
    assert session.usage.turns == 1
    assert session.usage.audio_seconds == pytest.approx(1.0)
    assert session.usage.tts_chars == len("Добре.")
