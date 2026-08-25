"""Streaming MP3 -> raw PCM conversion.

edge-tts hardcodes its output to audio-24khz-48kbitrate-mono-mp3 (see the
literal in edge_tts/communicate.py), but the ESP32 expects 16 kHz 16-bit mono
little-endian PCM. Converting has to happen while bytes arrive, not after the
utterance completes, or the 300 ms TTS slice of the latency budget is gone.

PyAV is used rather than an ffmpeg subprocess so the Railway image needs no
system packages and there is no child process to supervise.
"""

import av

PCM_SAMPLE_RATE = 16000
PCM_SAMPLE_WIDTH = 2


class Mp3ToPcm:
    """Incremental MP3 decoder emitting 16 kHz mono s16le PCM."""

    def __init__(self, rate: int = PCM_SAMPLE_RATE) -> None:
        self._codec = av.codec.CodecContext.create("mp3", "r")
        self._resampler = av.audio.resampler.AudioResampler(
            format="s16", layout="mono", rate=rate
        )
        self._prefix = b""
        self._header_done = False

    def feed(self, mp3: bytes) -> bytes:
        """Decode what is decodable so far; returns b'' when more input is needed."""
        if not mp3:
            return b""
        mp3 = self._strip_id3(mp3)
        if not mp3:
            return b""
        return self._decode(self._codec.parse(mp3))

    def flush(self) -> bytes:
        """Drain the decoder and resampler at end of stream."""
        out = self._decode(self._codec.parse(b""))
        return out + self._resample(None)

    def _decode(self, packets) -> bytes:
        out = bytearray()
        for packet in packets:
            for frame in self._codec.decode(packet):
                out += self._resample(frame)
        return bytes(out)

    def _resample(self, frame) -> bytes:
        out = bytearray()
        for resampled in self._resampler.resample(frame):
            out += bytes(resampled.planes[0])[: resampled.samples * PCM_SAMPLE_WIDTH]
        return bytes(out)

    def _strip_id3(self, mp3: bytes) -> bytes:
        """Drop a leading ID3v2 tag, which the bare mp3 decoder rejects.

        The tag may straddle chunk boundaries, so bytes are held back until the
        full header length is known.
        """
        if self._header_done:
            return mp3
        buf = self._prefix + mp3
        if len(buf) < 10:
            self._prefix = buf
            return b""
        if buf[:3] != b"ID3":
            self._header_done = True
            self._prefix = b""
            return buf
        size = 10 + (
            (buf[6] & 0x7F) << 21 | (buf[7] & 0x7F) << 14 | (buf[8] & 0x7F) << 7 | (buf[9] & 0x7F)
        )
        if len(buf) < size:
            self._prefix = buf
            return b""
        self._header_done = True
        self._prefix = b""
        return buf[size:]
