from server.providers.mock import MockLLM, MockSTT


async def test_mock_stt_returns_its_canned_text():
    stt = MockSTT("Як справи?")
    assert (await stt.transcribe(b"\x00\x01" * 100, 16000)).text == "Як справи?"


async def test_mock_stt_reports_real_duration():
    stt = MockSTT()
    result = await stt.transcribe(b"\x00\x01" * 16000, 16000)
    assert result.seconds == 1.0


async def test_mock_llm_streams_in_more_than_one_piece():
    llm = MockLLM("Все добре. Дякую.")
    chunks = [c async for c in llm.stream([], 150)]
    assert len(chunks) > 1
    assert "".join(chunks).strip() == "Все добре. Дякую."


async def test_mock_llm_echoes_the_last_user_message_when_asked():
    llm = MockLLM(echo=True)
    messages = [{"role": "user", "content": "Привіт"}]
    assert "Привіт" in "".join([c async for c in llm.stream(messages, 150)])


async def test_mock_llm_complete_returns_a_json_array_for_fact_extraction():
    import json

    llm = MockLLM()
    raw = await llm.complete([{"role": "user", "content": "extract facts"}], 300)
    assert isinstance(json.loads(raw), list)
