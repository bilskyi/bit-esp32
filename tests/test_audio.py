from server.audio import Mp3ToPcm


def test_fixture_carries_an_id3_header(mp3_one_second):
    # Guards the assumption the decoder is built around.
    assert mp3_one_second[:3] == b"ID3"


def test_decodes_to_roughly_one_second_at_16k(mp3_one_second):
    dec = Mp3ToPcm()
    pcm = dec.feed(mp3_one_second) + dec.flush()
    samples = len(pcm) // 2
    # LAME adds encoder delay/padding, so allow slack — but 24 kHz output
    # (~24000+ samples) must not slip through.
    assert 15000 <= samples <= 21000, samples


def test_output_is_int16_aligned(mp3_one_second):
    dec = Mp3ToPcm()
    pcm = dec.feed(mp3_one_second) + dec.flush()
    assert len(pcm) % 2 == 0


def test_emits_pcm_before_all_input_is_consumed(mp3_one_second):
    """The latency budget depends on decoding as bytes arrive."""
    dec = Mp3ToPcm()
    fed = 0
    for i in range(0, len(mp3_one_second), 512):
        fed = i + 512
        if dec.feed(mp3_one_second[i : i + 512]):
            break
    assert fed < len(mp3_one_second) / 2


def test_chunked_and_whole_input_agree(mp3_one_second):
    whole = Mp3ToPcm()
    a = whole.feed(mp3_one_second) + whole.flush()

    chunked = Mp3ToPcm()
    b = b"".join(chunked.feed(mp3_one_second[i : i + 300]) for i in range(0, len(mp3_one_second), 300))
    b += chunked.flush()

    assert a == b


def test_decodes_audio_not_silence(mp3_one_second):
    import struct

    dec = Mp3ToPcm()
    pcm = dec.feed(mp3_one_second) + dec.flush()
    peak = max(abs(v) for v in struct.unpack("<%dh" % (len(pcm) // 2), pcm))
    assert peak > 1000, peak


def test_empty_input_yields_no_output():
    dec = Mp3ToPcm()
    assert dec.feed(b"") == b""
    assert dec.flush() == b""
