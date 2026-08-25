"""Capture microphone audio from the device over USB and transcribe it.

A bench tool for the gap between step 2 and step 4. The device has no WiFi
yet, so this stands in for the WebSocket: it reads the framed PCM stream the
`stream` sketch emits, saves a WAV, and posts it to the same Groq provider the
server uses. That answers "is this microphone good enough for Whisper?" before
any networking exists to obscure the answer.

    uv run python scripts/mic_to_stt.py --seconds 8

Add --no-stt to record a WAV without spending an API call.
"""

import argparse
import asyncio
import statistics
import struct
import sys
import time
import wave

import serial

sys.path.insert(0, ".")
from server.config import Settings  # noqa: E402
from server.providers.groq_stt import GroqSTT  # noqa: E402

MAGIC = b"\xa5\x5a\xa5\x5a"
BLOCK_SAMPLES = 1024
BLOCK_BYTES = BLOCK_SAMPLES * 2
SAMPLE_RATE = 16000


def _read_exact(ser: serial.Serial, n: int, deadline: float) -> bytes:
    out = bytearray()
    while len(out) < n:
        if time.time() > deadline:
            raise TimeoutError(f"got {len(out)} of {n} bytes before timeout")
        chunk = ser.read(n - len(out))
        if chunk:
            out += chunk
    return bytes(out)


def _sync(ser: serial.Serial, deadline: float) -> None:
    """Scan forward until the frame magic is found.

    The device streams continuously, so the host always joins mid-stream. A
    one-byte misalignment would swap the high and low halves of every sample
    and turn speech into noise, which is why this is worth doing properly.
    """
    window = b""
    while True:
        if time.time() > deadline:
            raise TimeoutError("never saw frame magic - is the 'stream' sketch flashed?")
        b = ser.read(1)
        if not b:
            continue
        window = (window + b)[-4:]
        if window == MAGIC:
            return


def _rms(pcm: bytes) -> float:
    n = len(pcm) // 2
    if n == 0:
        return 0.0
    vals = struct.unpack(f"<{n}h", pcm[: n * 2])
    return (sum(v * v for v in vals) / n) ** 0.5


def capture(port: str, seconds: float) -> bytes:
    want_blocks = int(seconds * SAMPLE_RATE / BLOCK_SAMPLES)
    with serial.Serial(port, 115200, timeout=1) as ser:
        ser.reset_input_buffer()
        deadline = time.time() + seconds + 15
        _sync(ser, deadline)

        blocks, levels, resyncs = [], [], 0
        for i in range(want_blocks):
            pcm = _read_exact(ser, BLOCK_BYTES, deadline)
            blocks.append(pcm)
            levels.append(_rms(pcm))

            tag = _read_exact(ser, 4, deadline)
            if tag != MAGIC:
                # Lost framing; find it again rather than emitting garbage.
                resyncs += 1
                _sync(ser, deadline)

            if i and i % 8 == 0:
                bar = "#" * min(40, int(levels[-1] / 20))
                print(f"  {i * BLOCK_SAMPLES / SAMPLE_RATE:4.1f}s  rms {levels[-1]:6.0f} {bar}")

    if resyncs:
        print(f"  note: re-synced {resyncs}x (dropped audio at those points)")
    print(
        f"  levels: median rms {statistics.median(levels):.0f}, "
        f"peak {max(levels):.0f}"
    )
    return b"".join(blocks)


def write_wav(path: str, pcm: bytes) -> None:
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(pcm)


async def transcribe(pcm: bytes) -> None:
    s = Settings()
    if not s.groq_api_key:
        print("GROQ_API_KEY is empty; skipping transcription.")
        return
    stt = GroqSTT(api_key=s.groq_api_key, model=s.stt_model)
    t0 = time.time()
    result = await stt.transcribe(pcm, SAMPLE_RATE)
    print(f"\n  model    : {s.stt_model}")
    print(f"  language : {result.language}")
    print(f"  audio    : {result.seconds:.1f}s, round trip {time.time() - t0:.2f}s")
    print(f"\n  >>> {result.text!r}\n")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbmodem1101")
    ap.add_argument("--seconds", type=float, default=8.0)
    ap.add_argument("--out", default="mic.wav")
    ap.add_argument("--no-stt", action="store_true")
    a = ap.parse_args()

    print(f"capturing {a.seconds:.0f}s from {a.port} - speak now")
    pcm = capture(a.port, a.seconds)
    write_wav(a.out, pcm)
    print(f"  wrote {a.out} ({len(pcm)} bytes, {len(pcm) / 2 / SAMPLE_RATE:.1f}s)")

    if not a.no_stt:
        asyncio.run(transcribe(pcm))


if __name__ == "__main__":
    main()
