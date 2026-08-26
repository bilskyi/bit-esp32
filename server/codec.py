"""IMA/DVI ADPCM decoding for the microphone uplink.

The device sends four bits per sample instead of sixteen. That is the whole
point: the link between this board and this router delivers well under the
32 KB/s raw 16 kHz PCM needs, stalling for a second or more at a time while
reporting a healthy -55 dBm. At 8 KB/s the same link carries an utterance
without truncating it.

Opus is out of reach on an ESP32-C3, which is why the spec ruled it out. ADPCM
is not the same proposition: the encoder is a few hundred bytes of arithmetic
with no tables beyond the two below, and it costs the device nothing measurable.

Implemented here rather than taken from `audioop` because that module is gone
in Python 3.13. The tests check this against `audioop` while it still exists.
"""

# Standard IMA ADPCM tables. Both sides must use these exact values.
_STEP_TABLE = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209,
    230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
    963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749,
    3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
)

_INDEX_TABLE = (-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)


def adpcm_to_pcm(data: bytes, predictor: int = 0, index: int = 0) -> bytes:
    """Expand 4-bit ADPCM into 16-bit little-endian PCM.

    The high nibble of each byte is the earlier sample, matching both the
    reference implementation and the firmware's packing.
    """
    return _decode(data, predictor, index)[0]


def _decode(data: bytes, predictor: int, index: int) -> tuple[bytes, int, int]:
    """Decode, returning the trailing coder state so a stream can continue."""
    if not data:
        return b"", predictor, index

    out = bytearray(len(data) * 4)
    pos = 0

    for byte in data:
        for code in ((byte >> 4) & 0x0F, byte & 0x0F):
            step = _STEP_TABLE[index]

            # Reconstruct the difference from the three magnitude bits, each
            # contributing a successively halved fraction of the step.
            diff = step >> 3
            if code & 4:
                diff += step
            if code & 2:
                diff += step >> 1
            if code & 1:
                diff += step >> 2

            predictor = predictor - diff if code & 8 else predictor + diff
            predictor = max(-32768, min(32767, predictor))

            index += _INDEX_TABLE[code]
            index = max(0, min(88, index))

            out[pos] = predictor & 0xFF
            out[pos + 1] = (predictor >> 8) & 0xFF
            pos += 2

    return bytes(out), predictor, index


class AdpcmDecoder:
    """Stateful decoder for a stream arriving in chunks.

    ADPCM is differential: every sample is expressed relative to the one
    before. Decoding each network frame from a fresh state would put a step
    discontinuity - an audible click - at every 1 KB boundary, so the predictor
    and step index have to survive between calls.
    """

    def __init__(self) -> None:
        self._predictor = 0
        self._index = 0

    def feed(self, data: bytes) -> bytes:
        pcm, self._predictor, self._index = _decode(data, self._predictor, self._index)
        return pcm
