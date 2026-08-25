#!/usr/bin/env python3
"""Stand in for the ESP32 so the server can be exercised from a laptop.

It speaks the same protocol the firmware will: a JSON `start`, PCM binary
frames sent *while the button is held*, a JSON `end`, then it collects the
reply audio and reports where the latency went.

    uv run python scripts/fake_device.py --say "Привіт, як справи?"
    uv run python scripts/fake_device.py --file question.wav
    uv run python scripts/fake_device.py --silence 2.0

The reply is written to reply.wav so you can listen to it.
"""

import argparse
import asyncio
import json
import math
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import websockets

from server.audio import Mp3ToPcm, pcm_to_wav

SAMPLE_RATE = 16000
CHUNK_SAMPLES = 1024  # matches the firmware's I2S read size
CHUNK_BYTES = CHUNK_SAMPLES * 2


async def pcm_from_text(text: str, voice: str) -> bytes:
    """Speak `text` with edge-tts and return it as mic-shaped PCM."""
    import edge_tts

    decoder = Mp3ToPcm(SAMPLE_RATE)
    out = bytearray()
    async for event in edge_tts.Communicate(text, voice).stream():
        if event.get("type") == "audio":
            out += decoder.feed(event["data"])
    out += decoder.flush()
    return bytes(out)


def pcm_from_wav(path: Path) -> bytes:
    """Read any audio file and convert it to 16 kHz mono s16le."""
    import av

    container = av.open(str(path))
    resampler = av.audio.resampler.AudioResampler(format="s16", layout="mono", rate=SAMPLE_RATE)
    out = bytearray()
    for frame in container.decode(audio=0):
        for resampled in resampler.resample(frame):
            out += bytes(resampled.planes[0])[: resampled.samples * 2]
    for resampled in resampler.resample(None):
        out += bytes(resampled.planes[0])[: resampled.samples * 2]
    return bytes(out)


def silence(seconds: float) -> bytes:
    n = int(SAMPLE_RATE * seconds)
    return struct.pack("<%dh" % n, *[int(200 * math.sin(2 * math.pi * 220 * i / SAMPLE_RATE)) for i in range(n)])


async def run(args: argparse.Namespace) -> int:
    if args.say:
        print(f"synthesising the question with {args.voice} ...")
        pcm = await pcm_from_text(args.say, args.voice)
    elif args.file:
        pcm = pcm_from_wav(Path(args.file))
    else:
        pcm = silence(args.silence)
    print(f"question: {len(pcm) / 2 / SAMPLE_RATE:.2f} s of audio, {len(pcm)} bytes")

    headers = {"Authorization": f"Bearer {args.token}"} if args.token else {}
    async with websockets.connect(args.url, additional_headers=headers, max_size=None) as ws:
        await ws.send(json.dumps({"type": "start"}))

        # Stream while "holding the button": real time pacing, so the server
        # sees the same arrival pattern the firmware will produce.
        send_started = time.perf_counter()
        for offset in range(0, len(pcm), CHUNK_BYTES):
            await ws.send(pcm[offset : offset + CHUNK_BYTES])
            if args.realtime:
                await asyncio.sleep(CHUNK_SAMPLES / SAMPLE_RATE)
        released = time.perf_counter()
        await ws.send(json.dumps({"type": "end"}))
        print(f"upload took {(released - send_started) * 1000:.0f} ms, button released")

        reply = bytearray()
        first_audio = None
        marks: dict[str, float] = {}
        while True:
            try:
                message = await asyncio.wait_for(ws.recv(), timeout=args.timeout)
            except asyncio.TimeoutError:
                print("timed out waiting for the server")
                return 1
            if isinstance(message, bytes):
                if first_audio is None:
                    first_audio = time.perf_counter()
                    print(f"  first audio      +{(first_audio - released) * 1000:7.0f} ms  <-- the number that matters")
                reply += message
                continue
            event = json.loads(message)
            now = time.perf_counter()
            if event.get("type") == "state":
                marks[event["value"]] = now
                print(f"  state {event['value']:<10} +{(now - released) * 1000:7.0f} ms")
            elif event.get("type") == "done":
                print(f"  done             +{(now - released) * 1000:7.0f} ms")
                break

    if not reply:
        print("no audio came back")
        return 1
    Path(args.out).write_bytes(pcm_to_wav(bytes(reply), SAMPLE_RATE))
    seconds = len(reply) / 2 / SAMPLE_RATE
    print(f"\nreply: {seconds:.2f} s of audio -> {args.out}")
    if first_audio:
        budget = (first_audio - released) * 1000
        verdict = "within" if budget <= 1500 else "OVER"
        print(f"button release -> first audible sound: {budget:.0f} ms ({verdict} the 1500 ms target)")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--url", default="ws://localhost:8000/ws")
    p.add_argument("--token", default="", help="device token, if the server requires one")
    source = p.add_mutually_exclusive_group()
    source.add_argument("--say", help="synthesise this question and send it as mic audio")
    source.add_argument("--file", help="send this audio file as mic audio")
    source.add_argument("--silence", type=float, default=2.0, help="send N seconds of quiet tone")
    p.add_argument("--voice", default="uk-UA-PolinaNeural", help="voice used for --say")
    p.add_argument("--out", default="reply.wav")
    p.add_argument("--timeout", type=float, default=30.0)
    p.add_argument(
        "--realtime",
        action="store_true",
        help="pace the upload like a real mic instead of blasting it",
    )
    return asyncio.run(run(p.parse_args()))


if __name__ == "__main__":
    raise SystemExit(main())
