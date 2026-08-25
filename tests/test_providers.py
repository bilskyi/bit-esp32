import json

import httpx
import pytest

from server.providers.groq_llm import GroqLLM
from server.providers.groq_stt import GroqSTT


def sse(*chunks: str) -> bytes:
    body = "".join(f"data: {c}\n\n" for c in chunks)
    return (body + "data: [DONE]\n\n").encode()


def delta(text: str) -> str:
    return json.dumps({"choices": [{"delta": {"content": text}}]})


async def noop_sleep(_seconds: float) -> None:
    return None


# --- LLM ---------------------------------------------------------------


async def test_stream_yields_text_deltas():
    def handler(request):
        return httpx.Response(200, content=sse(delta("Привіт"), delta(" світ")))

    llm = GroqLLM("k", "m", client=httpx.AsyncClient(transport=httpx.MockTransport(handler)))
    assert [c async for c in llm.stream([{"role": "user", "content": "hi"}], 150)] == [
        "Привіт",
        " світ",
    ]


async def test_stream_sends_model_and_token_cap():
    seen = {}

    def handler(request):
        seen.update(json.loads(request.content))
        return httpx.Response(200, content=sse(delta("ok")))

    llm = GroqLLM("k", "llama-x", client=httpx.AsyncClient(transport=httpx.MockTransport(handler)))
    [c async for c in llm.stream([{"role": "user", "content": "hi"}], 42)]
    assert seen["model"] == "llama-x"
    assert seen["max_tokens"] == 42
    assert seen["stream"] is True


async def test_stream_skips_malformed_sse_lines():
    def handler(request):
        return httpx.Response(200, content=sse("{not json", delta("fine")))

    llm = GroqLLM("k", "m", client=httpx.AsyncClient(transport=httpx.MockTransport(handler)))
    assert [c async for c in llm.stream([], 150)] == ["fine"]


async def test_stream_retries_on_429_then_succeeds():
    calls = {"n": 0}

    def handler(request):
        calls["n"] += 1
        if calls["n"] == 1:
            return httpx.Response(429, headers={"retry-after": "0"}, content=b"slow down")
        return httpx.Response(200, content=sse(delta("ok")))

    llm = GroqLLM(
        "k", "m", client=httpx.AsyncClient(transport=httpx.MockTransport(handler)), sleep=noop_sleep
    )
    assert [c async for c in llm.stream([], 150)] == ["ok"]
    assert calls["n"] == 2


async def test_stream_gives_up_after_max_retries():
    def handler(request):
        return httpx.Response(429, content=b"nope")

    llm = GroqLLM(
        "k",
        "m",
        client=httpx.AsyncClient(transport=httpx.MockTransport(handler)),
        max_retries=2,
        sleep=noop_sleep,
    )
    with pytest.raises(httpx.HTTPStatusError):
        [c async for c in llm.stream([], 150)]


async def test_complete_returns_whole_message():
    def handler(request):
        return httpx.Response(200, json={"choices": [{"message": {"content": "done"}}]})

    llm = GroqLLM("k", "m", client=httpx.AsyncClient(transport=httpx.MockTransport(handler)))
    assert await llm.complete([], 150) == "done"


# --- STT ---------------------------------------------------------------


async def test_transcribe_returns_text():
    def handler(request):
        return httpx.Response(200, json={"text": "  Як справи?  "})

    stt = GroqSTT("k", "whisper", client=httpx.AsyncClient(transport=httpx.MockTransport(handler)))
    result = await stt.transcribe(b"\x00\x01" * 1600, 16000)
    assert result.text == "Як справи?"


async def test_transcribe_reports_audio_duration():
    def handler(request):
        return httpx.Response(200, json={"text": "x"})

    stt = GroqSTT("k", "whisper", client=httpx.AsyncClient(transport=httpx.MockTransport(handler)))
    result = await stt.transcribe(b"\x00\x01" * 16000, 16000)  # 32000 bytes == 1 s
    assert result.seconds == pytest.approx(1.0)


async def test_transcribe_never_pins_a_language():
    """Whisper must auto-detect: the user code-switches between utterances."""
    seen = {"body": b""}

    def handler(request):
        seen["body"] = request.content
        return httpx.Response(200, json={"text": "x"})

    stt = GroqSTT("k", "whisper", client=httpx.AsyncClient(transport=httpx.MockTransport(handler)))
    await stt.transcribe(b"\x00\x01" * 100, 16000)
    assert b'name="language"' not in seen["body"]


async def test_transcribe_uploads_a_wav_container():
    seen = {"body": b""}

    def handler(request):
        seen["body"] = request.content
        return httpx.Response(200, json={"text": "x"})

    stt = GroqSTT("k", "whisper", client=httpx.AsyncClient(transport=httpx.MockTransport(handler)))
    await stt.transcribe(b"\x00\x01" * 100, 16000)
    assert b"RIFF" in seen["body"] and b"WAVE" in seen["body"]


async def test_transcribe_retries_on_429():
    calls = {"n": 0}

    def handler(request):
        calls["n"] += 1
        if calls["n"] == 1:
            return httpx.Response(429, content=b"slow")
        return httpx.Response(200, json={"text": "ok"})

    stt = GroqSTT(
        "k", "whisper", client=httpx.AsyncClient(transport=httpx.MockTransport(handler)), sleep=noop_sleep
    )
    assert (await stt.transcribe(b"\x00\x01" * 100, 16000)).text == "ok"
    assert calls["n"] == 2
