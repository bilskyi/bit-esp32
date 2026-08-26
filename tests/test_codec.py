"""The device encodes IMA ADPCM in C; this decoder has to agree with it exactly.

Rather than trust two independent implementations of the same spec, these tests
check ours against the reference one in the standard library. `audioop` speaks
the same Intel/DVI variant the firmware implements, so agreeing with it is good
evidence the firmware will interoperate.
"""

import math
import struct

import pytest

from server.codec import adpcm_to_pcm

audioop = pytest.importorskip("audioop", reason="removed in Python 3.13")


def sine(n: int, hz: float = 440.0, rate: int = 16000, amp: int = 8000) -> bytes:
    return struct.pack(
        f"<{n}h", *[int(amp * math.sin(2 * math.pi * hz * i / rate)) for i in range(n)]
    )


def test_matches_reference_decoder_on_a_tone():
    pcm = sine(4000)
    encoded, _ = audioop.lin2adpcm(pcm, 2, None)
    expected, _ = audioop.adpcm2lin(encoded, 2, None)
    assert adpcm_to_pcm(encoded) == expected


def test_matches_reference_decoder_on_silence():
    encoded, _ = audioop.lin2adpcm(b"\x00\x00" * 1000, 2, None)
    expected, _ = audioop.adpcm2lin(encoded, 2, None)
    assert adpcm_to_pcm(encoded) == expected


def test_matches_reference_decoder_on_full_scale_swings():
    pcm = struct.pack("<2000h", *([32767, -32768] * 1000))
    encoded, _ = audioop.lin2adpcm(pcm, 2, None)
    expected, _ = audioop.adpcm2lin(encoded, 2, None)
    assert adpcm_to_pcm(encoded) == expected


def test_output_is_four_times_the_input():
    encoded, _ = audioop.lin2adpcm(sine(2000), 2, None)
    assert len(adpcm_to_pcm(encoded)) == len(encoded) * 4


def test_empty_input_gives_empty_output():
    assert adpcm_to_pcm(b"") == b""


def test_reconstruction_stays_close_to_the_original():
    pcm = sine(8000)
    encoded, _ = audioop.lin2adpcm(pcm, 2, None)
    out = adpcm_to_pcm(encoded)
    original = struct.unpack(f"<{len(pcm) // 2}h", pcm)
    decoded = struct.unpack(f"<{len(out) // 2}h", out)
    err = math.sqrt(
        sum((a - b) ** 2 for a, b in zip(original, decoded)) / len(original)
    )
    # ADPCM is lossy, but a 4:1 coder should stay well inside a few percent of
    # full scale on a tone this simple.
    assert err < 0.03 * 32768
