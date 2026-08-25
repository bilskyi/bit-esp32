from server.providers.edge_tts import EdgeTTS


class FakeCommunicate:
    """Stands in for edge_tts.Communicate: yields mp3 in small chunks."""

    last_voice = None
    last_text = None
    mp3 = b""

    def __init__(self, text, voice, **kw):
        FakeCommunicate.last_text = text
        FakeCommunicate.last_voice = voice

    async def stream(self):
        for i in range(0, len(FakeCommunicate.mp3), 400):
            yield {"type": "audio", "data": FakeCommunicate.mp3[i : i + 400]}
        yield {"type": "WordBoundary", "offset": 0}


async def test_yields_pcm_not_mp3(mp3_one_second):
    FakeCommunicate.mp3 = mp3_one_second
    tts = EdgeTTS(communicate=FakeCommunicate)
    pcm = b"".join([c async for c in tts.synthesise("привіт", "uk-UA-OstapNeural")])
    assert pcm[:3] != b"ID3"
    assert len(pcm) > 20000


async def test_output_is_16k_mono_s16(mp3_one_second):
    FakeCommunicate.mp3 = mp3_one_second
    tts = EdgeTTS(communicate=FakeCommunicate)
    pcm = b"".join([c async for c in tts.synthesise("привіт", "uk-UA-OstapNeural")])
    samples = len(pcm) // 2
    assert 15000 <= samples <= 21000, samples


async def test_passes_the_requested_voice_through(mp3_one_second):
    FakeCommunicate.mp3 = mp3_one_second
    tts = EdgeTTS(communicate=FakeCommunicate)
    [c async for c in tts.synthesise("hello", "en-US-AndrewNeural")]
    assert FakeCommunicate.last_voice == "en-US-AndrewNeural"
    assert FakeCommunicate.last_text == "hello"


async def test_ignores_non_audio_events(mp3_one_second):
    FakeCommunicate.mp3 = b""
    tts = EdgeTTS(communicate=FakeCommunicate)
    assert b"".join([c async for c in tts.synthesise("x", "v")]) == b""


async def test_emits_before_the_stream_ends(mp3_one_second):
    """First PCM must not wait for the last mp3 byte."""
    FakeCommunicate.mp3 = mp3_one_second
    tts = EdgeTTS(communicate=FakeCommunicate)
    chunks = [c async for c in tts.synthesise("привіт", "uk-UA-OstapNeural")]
    assert len(chunks) > 1
