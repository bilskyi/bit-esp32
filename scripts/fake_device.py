#!/usr/bin/env python3
"""Stand in for the ESP32 so the server can be exercised from a laptop.

It speaks the same protocol the firmware will: a JSON `start`, PCM binary
frames sent *while the button is held*, a JSON `end`, then it collects the
reply audio and reports where the latency went.

    uv run python scripts/fake_device.py --say "Привіт, як справи?"
    uv run python scripts/fake_device.py --file question.wav
    uv run python scripts/fake_device.py --silence 2.0

The reply is written to reply.wav so you can listen to it.

It also stands in for the device being updated, which is how the whole
browser -> server -> device path gets exercised with no board in the room:

    uv run python scripts/fake_device.py --await-ota \
        --expect firmware/build/voice_capture.bin

then push that file at POST /firmware/push from the web app or with curl.
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


async def connect(args: argparse.Namespace):
    """Open the socket, or explain why not and return None.

    Shared by both modes so a bad token reads the same either way.
    """
    headers = {"Authorization": f"Bearer {args.token}"} if args.token else {}
    try:
        return await websockets.connect(args.url, additional_headers=headers, max_size=None)
    except websockets.exceptions.InvalidStatus as exc:
        if exc.response.status_code in (401, 403):
            print("rejected by the server: bad or missing device token (pass --token)")
        else:
            print(f"rejected by the server: HTTP {exc.response.status_code}")
    except OSError as exc:
        print(f"could not reach {args.url}: {exc}")
    return None


async def say_hello(ws, version: str) -> None:
    """What the firmware sends on every connect, so the server can report a
    version on the Пристрої page instead of admitting it knows none."""
    await ws.send(json.dumps({"type": "hello", "version": version}))


async def await_ota(args: argparse.Namespace) -> int:
    """Sit there being a device that can be updated.

    Speaks the device half of the protocol exactly: ota_begin, binary frames
    into a buffer instead of a flash partition, ota_end, then ota_ready. What
    it does not do is verify the image the way esp_ota_end() would - the
    appended SHA-256 is checked by the bootloader on real hardware, and
    pretending to do it here would be theatre.
    """
    expected = Path(args.expect).read_bytes() if args.expect else None
    if expected is not None:
        print(f"expecting {len(expected)} bytes from {args.expect}")

    connection = await connect(args)
    if connection is None:
        return 1

    async with connection as ws:
        await say_hello(ws, args.version)
        print(f"connected as version {args.version}; waiting for an update ...")

        size = 0
        got = bytearray()
        started = 0.0

        while True:
            try:
                message = await asyncio.wait_for(ws.recv(), timeout=args.timeout)
            except asyncio.TimeoutError:
                print(f"nothing arrived within {args.timeout:.0f} s")
                return 1

            if isinstance(message, bytes):
                if not size:
                    print("binary frame with no ota_begin - that is reply audio, not firmware")
                    continue
                got += message
                done = len(got)
                # One line per 64 KB, so a megabyte does not scroll the
                # terminal off its own hinges.
                if done % (64 * 1024) < len(message):
                    print(f"  {done:>8} / {size} bytes  ({100 * done // size}%)")
                continue

            event = json.loads(message)
            kind = event.get("type")

            if kind == "ota_begin":
                size = int(event.get("size", 0))
                started = time.perf_counter()
                got = bytearray()
                print(f"ota_begin: {size} bytes, version {event.get('version') or '(unnamed)'}")
                continue

            if kind == "ota_abort":
                print("ota_abort - the server gave up")
                return 1

            if kind != "ota_end":
                print(f"  (ignoring {kind})")
                continue

            elapsed = time.perf_counter() - started
            print(f"ota_end after {elapsed:.2f} s ({len(got) / max(elapsed, 1e-9) / 1024:.0f} KB/s)")

            if len(got) != size:
                reason = f"got {len(got)} bytes, was promised {size}"
                print(f"REFUSING: {reason}")
                await ws.send(json.dumps({"type": "ota_failed", "reason": reason}))
                return 1

            if expected is not None and bytes(got) != expected:
                reason = "bytes do not match the expected file"
                print(f"REFUSING: {reason}")
                await ws.send(json.dumps({"type": "ota_failed", "reason": reason}))
                return 1

            if expected is not None:
                print("bytes match")
            await ws.send(json.dumps({"type": "ota_ready"}))
            print("ota_ready sent; a real device would restart into it now")
            return 0


async def run(args: argparse.Namespace) -> int:
    if args.say:
        print(f"synthesising the question with {args.voice} ...")
        pcm = await pcm_from_text(args.say, args.voice)
    elif args.file:
        pcm = pcm_from_wav(Path(args.file))
    else:
        pcm = silence(args.silence)
    print(f"question: {len(pcm) / 2 / SAMPLE_RATE:.2f} s of audio, {len(pcm)} bytes")

    connection = await connect(args)
    if connection is None:
        return 1

    async with connection as ws:
        await say_hello(ws, args.version)
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
            elif event.get("type") == "emotion":
                # Drives the face. It has to arrive before "speaking", or the
                # eyes change expression a beat after the first word.
                print(f"  emotion {event['value']:<8} +{(now - released) * 1000:7.0f} ms")
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
    p.add_argument(
        "--version",
        default="fake-0.0.0",
        help="the version this stand-in reports in its hello frame",
    )
    p.add_argument(
        "--await-ota",
        action="store_true",
        help="be a device that can be updated, instead of asking a question",
    )
    p.add_argument("--expect", help="with --await-ota: compare the received bytes to this file")

    args = p.parse_args()
    if args.await_ota:
        # A different default: an update is something a person triggers by
        # hand from another window, and thirty seconds is not long enough to
        # get there.
        if "--timeout" not in sys.argv:
            args.timeout = 300.0
        return asyncio.run(await_ota(args))
    return asyncio.run(run(args))


if __name__ == "__main__":
    raise SystemExit(main())
