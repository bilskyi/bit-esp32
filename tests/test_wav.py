import struct

from server.audio import pcm_to_wav


def test_starts_with_riff_wave_magic():
    w = pcm_to_wav(b"\x00\x00" * 100, 16000)
    assert w[:4] == b"RIFF"
    assert w[8:12] == b"WAVE"


def test_declares_mono_16bit_at_the_given_rate():
    w = pcm_to_wav(b"\x00\x00" * 100, 16000)
    channels, rate, _, _, bits = struct.unpack("<HIIHH", w[22:36])
    assert channels == 1
    assert rate == 16000
    assert bits == 16


def test_riff_size_field_matches_actual_length():
    w = pcm_to_wav(b"\x01\x02" * 500, 16000)
    assert struct.unpack("<I", w[4:8])[0] == len(w) - 8


def test_data_chunk_size_matches_payload():
    pcm = b"\x01\x02" * 500
    w = pcm_to_wav(pcm, 16000)
    assert struct.unpack("<I", w[40:44])[0] == len(pcm)


def test_payload_is_preserved_verbatim():
    pcm = bytes(range(256)) * 4
    assert pcm_to_wav(pcm, 16000)[44:] == pcm
