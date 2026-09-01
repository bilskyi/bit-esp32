import asyncio
import time

import pytest

from server.config import Settings
from server.session import Session, State
from tests.fakes import FakeEmbedder, FakeLLM, FakeSTT, FakeTransport, FakeTTS, SlowTTS


def build(stt=None, llm=None, tts=None, **kw):
    transport = FakeTransport()
    session = Session(
        transport=transport,
        stt=stt or FakeSTT(),
        llm=llm or FakeLLM(),
        tts=tts or FakeTTS(),
        settings=Settings(_env_file=None, **kw),
        embedder=FakeEmbedder(),
    )
    return session, transport


# Two seconds at 16 kHz. It used to be 0.1 s, which was fine while nothing
# looked at the length - but the server now refuses to transcribe a fragment
# too short to be speech, and a helper called `utter` should produce something
# an utterance could plausibly be.
async def utter(session, audio=b"\x00\x01" * 32000):
    await session.on_start()
    await session.on_audio(audio)
    await session.on_end()
    # on_end detaches the reply so the socket loop can still hear "cancel";
    # a test driving the session directly has to wait for it.
    await session.wait_for_reply()


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
    await utter(session, audio=b"\x00\x01" * 32000)
    assert stt.received == [64000]


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
    await session.wait_for_reply()
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
        embedder=FakeEmbedder(),
    )
    await session.load_memory()
    await utter(session)
    assert "Lives in Kyiv" in llm.prompts[0][0]["content"]


async def test_relevant_facts_are_looked_up_per_question_not_once_at_connect():
    """Retrieval has to happen after the transcript exists - there is no
    question to rank against at connection time."""
    from tests.fakes import FakeStore

    llm = FakeLLM()
    store = FakeStore(["Owns a cat named Musya"])
    session, _ = build(llm=llm)
    session.store = store

    await session.load_memory()
    assert session.facts == [], "nothing has been retrieved before the first question"
    await utter(session)
    assert "Owns a cat named Musya" in llm.prompts[0][0]["content"]


async def test_standing_instructions_reach_every_reply_regardless_of_the_question():
    from tests.fakes import FakeStore

    llm = FakeLLM()
    session, _ = build(llm=llm)
    session.store = FakeStore(standing=["Always answer informally"])

    await session.load_memory()
    await utter(session)
    assert "Always answer informally" in llm.prompts[0][0]["content"]


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
    await utter(session, audio=b"\x00\x01" * 16000)  # exactly 1 second, the floor itself
    assert session.usage.turns == 1
    assert session.usage.audio_seconds == pytest.approx(1.0)
    assert session.usage.tts_chars == len("Добре.")


async def test_cancel_stops_a_reply_part_way_through():
    """The user pressed the button while it was talking: stop talking."""
    tts = SlowTTS(chunks=50, delay=0.01)
    session, transport = build(tts=tts)
    await session.on_start()
    await session.on_audio(b"\x00\x01" * 32000)
    await session.on_end()

    await asyncio.sleep(0.05)  # let a little audio out
    await session.on_cancel()

    emitted_at_cancel = tts.emitted
    await asyncio.sleep(0.1)  # nothing more should appear afterwards
    assert tts.emitted == emitted_at_cancel
    assert tts.finished == 0, "synthesis should not have run to completion"


async def test_cancel_returns_the_session_to_idle():
    tts = SlowTTS(chunks=50, delay=0.01)
    session, _ = build(tts=tts)
    await session.on_start()
    await session.on_audio(b"\x00\x01" * 32000)
    await session.on_end()
    await asyncio.sleep(0.05)

    await session.on_cancel()
    assert session.state is State.IDLE


async def test_a_press_during_a_reply_interrupts_and_listens_again():
    tts = SlowTTS(chunks=50, delay=0.01)
    session, _ = build(tts=tts)
    await session.on_start()
    await session.on_audio(b"\x00\x01" * 32000)
    await session.on_end()
    await asyncio.sleep(0.05)

    # No explicit cancel: starting again while speaking must interrupt.
    await session.on_start()
    assert session.state is State.LISTENING
    assert tts.finished == 0


# -------------------------------------------- a start with no end before it
#
# The device can leave LISTENING without ever sending "end". Its playback task
# rescues a reply that produced no audio by forcing ST_IDLE, and until 28 Aug
# it did that on any "done" at all - including the one closing a reply the user
# had just interrupted, which lands a moment after the mic has already started
# on the next question. The recording is then made in a state that never sends
# "end", and the next thing the device does is press again and send a second
# "start".
#
# Measured on 28 Aug, from the server's own log: a start accepted 0.19 s after
# a cancel, 0.22 s of audio buffered, no "end", and sixty seconds later
# "utterance timed out after 60.0s". The device half is fixed in voice_main.c;
# these cover this side of it, because a device that does this must not be able
# to wedge the session.


async def test_a_second_start_while_listening_begins_a_fresh_utterance():
    stt = FakeSTT()
    session, transport = build(stt=stt)
    await session.on_start()
    await session.on_audio(b"\x7f\x7f" * 3520)  # the abandoned 0.22 s

    await session.on_start()  # the user pressed again
    assert session.state is State.LISTENING
    assert transport.states == ["listening", "listening"]

    await session.on_audio(b"\x00\x01" * 32000)
    await session.on_end()
    await session.wait_for_reply()
    assert stt.received == [2 * 32000], "the abandoned fragment was prepended"


async def test_a_second_start_rearms_the_utterance_watchdog():
    """Or the new question inherits what is left of the old one's sixty
    seconds, and a long answer is cut off part-way through."""
    session, _ = build(session_timeout_s=0.3)
    await session.on_start()
    await asyncio.sleep(0.2)
    await session.on_start()

    await asyncio.sleep(0.2)  # 0.4 s since the first start, 0.2 s since the second
    assert session.state is State.LISTENING


async def test_cancel_when_nothing_is_playing_is_harmless():
    session, _ = build()
    await session.on_cancel()
    assert session.state is State.IDLE


async def test_the_socket_loop_is_not_blocked_while_a_reply_plays():
    """on_end must return promptly, or "cancel" could never be read."""
    tts = SlowTTS(chunks=100, delay=0.01)  # a full second of synthesis
    session, _ = build(tts=tts)
    await session.on_start()
    await session.on_audio(b"\x00\x01" * 32000)

    started = time.monotonic()
    await session.on_end()
    assert time.monotonic() - started < 0.2

    await session.on_cancel()


# ---------------------------------------------------------------- the emotion

def emotions(transport):
    return [m["value"] for m in transport.json if m.get("type") == "emotion"]


def frame_types(transport):
    return [m["type"] for m in transport.json]


async def test_the_emotion_reaches_the_device_before_the_speaking_state():
    """The face has to be right when the first word arrives, not a beat after
    it."""
    session, transport = build(llm=FakeLLM("[curious] А в тебе як?"))
    await utter(session)

    types = frame_types(transport)
    assert "emotion" in types
    speaking = next(
        i for i, m in enumerate(transport.json)
        if m.get("type") == "state" and m["value"] == "speaking"
    )
    assert types.index("emotion") < speaking
    assert emotions(transport) == ["curious"]


async def test_the_tag_never_reaches_the_voice():
    """edge-tts pronounces "happy" perfectly happily."""
    tts = FakeTTS()
    session, _ = build(llm=FakeLLM("[happy] Все добре. Дякую!"), tts=tts)
    await utter(session)

    assert tts.spoken
    for text, _ in tts.spoken:
        assert "[" not in text and "]" not in text
        assert "happy" not in text.lower()


async def test_a_tag_in_the_middle_of_the_reply_is_removed_too():
    tts = FakeTTS()
    session, _ = build(llm=FakeLLM("Все добре [excited] а в тебе?"), tts=tts)
    await utter(session)
    for text, _ in tts.spoken:
        assert "[" not in text


async def test_the_tag_is_kept_out_of_the_stored_history():
    """History is fed back to the model and used for language detection, so a
    stray tag would compound."""
    session, _ = build(llm=FakeLLM("[sad] Не знаю."))
    await utter(session)
    replies = [m["content"] for m in session.history if m["role"] == "assistant"]
    assert replies == ["Не знаю."]


async def test_an_untagged_reply_still_gets_an_emotion():
    """Every reply produces exactly one emotion frame, tag or no tag."""
    session, transport = build(llm=FakeLLM("Авжеж, зробимо!"))
    await utter(session)
    assert emotions(transport) == ["excited"]


async def test_an_unknown_tag_falls_back_to_the_heuristic():
    session, transport = build(llm=FakeLLM("[smug] А в тебе як?"))
    await utter(session)
    assert emotions(transport) == ["curious"]


async def test_exactly_one_emotion_frame_per_reply():
    session, transport = build(llm=FakeLLM("[happy] Перше. Друге. Третє."))
    await utter(session)
    assert emotions(transport) == ["happy"]


async def test_a_reply_the_model_could_not_give_looks_sad():
    """The "did not catch that" fallback goes through the same path."""
    session, transport = build(llm=FakeLLM(""))
    await utter(session)
    assert emotions(transport) == ["sad"]


async def test_a_fragment_too_short_to_be_speech_never_reaches_stt():
    """Whisper answers room tone with its training data.

    Measured on the real device: 0.2-0.3 s fragments came back as "Thank you."
    and "Спасибо.", the model replied "Пожалуйста!", and a brushed button made
    it say that to everything, eight times in a row in the logs. The device
    cannot catch this - it knows how long the button was held, not how much
    audio arrived.
    """
    stt = FakeSTT()
    session, _ = build(stt=stt)

    half_a_second = b"\x00\x00" * (session.settings.sample_rate // 2)
    await session._respond(half_a_second)
    assert stt.received == [], "spent an STT call on a fragment that cannot be speech"


async def test_an_utterance_long_enough_to_be_speech_still_gets_through():
    """The guard above must not become a reason ordinary questions vanish."""
    stt = FakeSTT()
    session, _ = build(stt=stt)

    two_seconds = b"\x00\x00" * (session.settings.sample_rate * 2)
    await session._respond(two_seconds)
    assert stt.received == [len(two_seconds)]


async def test_default_role_is_the_device_default():
    from server.roles import DEVICE_DEFAULT

    session, _ = build()
    assert session.role is DEVICE_DEFAULT
    assert session.surface == "esp32"


async def test_a_typed_question_gets_the_screen_wording():
    from server.roles import WEB_DEFAULT

    llm = FakeLLM()
    session = Session(
        transport=FakeTransport(), stt=FakeSTT(), llm=llm, tts=FakeTTS(),
        settings=Settings(_env_file=None), embedder=FakeEmbedder(),
        role=WEB_DEFAULT, surface="web",
    )
    await session.on_text("Як справи?")
    await session.wait_for_reply()
    assert "markdown, lists and headings are fine" in llm.prompts[0][0]["content"].lower()


async def test_a_spoken_question_never_gets_markdown_permission():
    """Same role, spoken instead of typed: TTS would read the asterisks."""
    from server.roles import WEB_DEFAULT

    llm = FakeLLM()
    session = Session(
        transport=FakeTransport(), stt=FakeSTT(), llm=llm, tts=FakeTTS(),
        settings=Settings(_env_file=None), embedder=FakeEmbedder(),
        role=WEB_DEFAULT, surface="web",
    )
    await utter(session)
    prompt = llm.prompts[0][0]["content"].lower()
    assert "no lists, no headings, no markdown" in prompt
    assert "markdown, lists and headings are fine" not in prompt


async def test_a_role_shaped_like_the_device_default_gets_the_measured_prompt():
    """The old identity check made a freshly-built lookalike get the *web*
    prompt, which was surprising enough to need a test explaining it. Roles
    are compared by value, so a lookalike now gets exactly BASE - the
    surprise is gone rather than documented."""
    from server.persona import BASE
    from server.roles import Role, SPEAKABLE_LANGUAGES

    lookalike = Role(id=7, name="Copy", prompt=None, max_sentences=2,
                     markdown_allowed=False, languages=SPEAKABLE_LANGUAGES,
                     pinned_mood=None)
    llm = FakeLLM()
    session = Session(
        transport=FakeTransport(), stt=FakeSTT(), llm=llm, tts=FakeTTS(),
        settings=Settings(_env_file=None), embedder=FakeEmbedder(), role=lookalike,
    )
    await utter(session)
    assert llm.prompts[0][0]["content"] == BASE


async def test_a_pinned_mood_overrides_the_tag_the_model_chose():
    from server.roles import Role, SPEAKABLE_LANGUAGES

    sleepy = Role(id=8, name="Sleepy", prompt=None, max_sentences=2,
                  markdown_allowed=False, languages=SPEAKABLE_LANGUAGES,
                  pinned_mood="sleepy")
    session = Session(
        transport=(transport := FakeTransport()), stt=FakeSTT(),
        llm=FakeLLM("[happy] Все добре."), tts=FakeTTS(),
        settings=Settings(_env_file=None), embedder=FakeEmbedder(), role=sleepy,
    )
    await utter(session)
    emotions = [f["value"] for f in transport.json if f.get("type") == "emotion"]
    assert emotions == ["sleepy"]


async def test_no_pinned_mood_leaves_the_models_tag_alone():
    session = Session(
        transport=(transport := FakeTransport()), stt=FakeSTT(),
        llm=FakeLLM("[happy] Все добре."), tts=FakeTTS(),
        settings=Settings(_env_file=None), embedder=FakeEmbedder(),
    )
    await utter(session)
    emotions = [f["value"] for f in transport.json if f.get("type") == "emotion"]
    assert emotions == ["happy"]


# ------------------------------------------------------------ typed questions

async def test_on_text_skips_stt_entirely():
    stt = FakeSTT()
    session, _ = build(stt=stt)
    await session.on_text("Привіт!")
    await session.wait_for_reply()
    assert stt.received == []


async def test_on_text_produces_a_written_reply_not_audio():
    session, transport = build(llm=FakeLLM("Добре."))
    await session.on_text("Привіт!")
    await session.wait_for_reply()
    assert transport.binary == []
    replies = [m["value"] for m in transport.json if m.get("type") == "reply"]
    assert replies == ["Добре."]


async def test_on_text_state_sequence_has_no_listening_phase():
    session, transport = build()
    await session.on_text("Привіт!")
    await session.wait_for_reply()
    assert transport.states == ["thinking", "speaking", "idle"]


async def test_on_text_interrupts_an_in_progress_reply():
    tts = SlowTTS(chunks=50, delay=0.01)
    session, _ = build(tts=tts)
    await session.on_start()
    await session.on_audio(b"\x00\x01" * 32000)
    await session.on_end()
    await asyncio.sleep(0.05)

    await session.on_text("Ще одне питання")
    assert session.state is State.THINKING


async def test_on_text_does_not_count_tts_chars_or_audio_seconds():
    session, _ = build(llm=FakeLLM("Добре."))
    await session.on_text("Привіт!")
    await session.wait_for_reply()
    assert session.usage.audio_seconds == 0
    assert session.usage.tts_chars == 0


async def test_a_web_session_gets_a_trace_frame():
    from server.roles import WEB_DEFAULT

    session = Session(
        transport=(transport := FakeTransport()), stt=FakeSTT(),
        llm=FakeLLM("[happy] Все добре."), tts=FakeTTS(),
        settings=Settings(_env_file=None), embedder=FakeEmbedder(),
        role=WEB_DEFAULT, surface="web",
    )
    await session.on_text("Як справи?")
    await session.wait_for_reply()

    traces = [f["value"] for f in transport.json if f.get("type") == "trace"]
    assert len(traces) == 1
    trace = traces[0]
    assert trace["role"] == "Web default"
    assert trace["surface"] == "web"
    assert trace["emotion"] == "happy"
    assert trace["spoken"] is False
    assert trace["prompt_tokens"] > 0
    assert "Rules:" in trace["prompt"]


async def test_the_device_never_gets_a_trace_frame():
    session = Session(
        transport=(transport := FakeTransport()), stt=FakeSTT(), llm=FakeLLM(),
        tts=FakeTTS(), settings=Settings(_env_file=None), embedder=FakeEmbedder(),
    )
    await utter(session)
    assert "trace" not in transport.types


async def test_the_trace_carries_the_retrieved_facts_and_their_scores():
    from tests.fakes import FakeStore

    session = Session(
        transport=(transport := FakeTransport()), stt=FakeSTT(), llm=FakeLLM(),
        tts=FakeTTS(), settings=Settings(_env_file=None), embedder=FakeEmbedder(),
        store=FakeStore(facts=["Lives in Chernivtsi"]), surface="web",
    )
    await session.on_text("Де я живу?")
    await session.wait_for_reply()

    trace = next(f["value"] for f in transport.json if f.get("type") == "trace")
    assert trace["facts"] == [{"text": "Lives in Chernivtsi", "score": 1.0}]


async def test_a_broken_trace_does_not_lose_the_reply(monkeypatch):
    """The frame is a debugging aid; the answer is the product."""
    from server.roles import WEB_DEFAULT

    session = Session(
        transport=(transport := FakeTransport()), stt=FakeSTT(),
        llm=FakeLLM("Все добре."), tts=FakeTTS(),
        settings=Settings(_env_file=None), embedder=FakeEmbedder(),
        role=WEB_DEFAULT, surface="web",
    )
    monkeypatch.setattr(session, "_trace", lambda **kw: (_ for _ in ()).throw(RuntimeError("boom")))
    await session.on_text("Як справи?")
    await session.wait_for_reply()
    assert "reply" in transport.types
    assert "trace" not in transport.types


async def test_on_text_while_listening_discards_the_recording_and_logs_it(caplog):
    session, transport = build()
    await session.on_start()
    await session.on_audio(b"\x00\x01" * 16000)
    assert len(session._buf) > 0

    with caplog.at_level("INFO"):
        await session.on_text("typed instead")
    await session.wait_for_reply()

    assert len(session._buf) == 0
    assert session._watchdog is None
    assert any("dropping" in r.message for r in caplog.records)
