import io
import math
import struct

import av
import pytest


def _make_mp3(seconds: float = 1.0, rate: int = 24000, freq: int = 440) -> bytes:
    """Encode a sine tone to MP3 the way edge-tts delivers it: 24 kHz mono."""
    n = int(rate * seconds)
    pcm = struct.pack("<%dh" % n, *[int(9000 * math.sin(2 * math.pi * freq * i / rate)) for i in range(n)])
    src = av.AudioFrame(format="s16", layout="mono", samples=n)
    src.sample_rate = rate
    src.pts = 0
    src.planes[0].update(pcm)
    to_fltp = av.audio.resampler.AudioResampler(format="fltp", layout="mono", rate=rate)

    buf = io.BytesIO()
    container = av.open(buf, mode="w", format="mp3")
    stream = container.add_stream("libmp3lame", rate=rate, layout="mono")
    for frame in to_fltp.resample(src):
        for packet in stream.encode(frame):
            container.mux(packet)
    for packet in stream.encode(None):
        container.mux(packet)
    container.close()
    return buf.getvalue()


@pytest.fixture(scope="session")
def mp3_one_second() -> bytes:
    return _make_mp3(1.0)
