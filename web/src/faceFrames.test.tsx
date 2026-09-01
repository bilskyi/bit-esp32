// Narrow on purpose, same reasoning as settingsApi.test.tsx: the base64 ->
// framebuffer -> pixel unpacking is logic a browser click-through cannot
// falsify (a wrong bit in the page/row math would still paint *something*,
// just the wrong somethings), not a test of Face.tsx's canvas rendering
// itself. The vitest harness only picks up `*.test.tsx` (see
// vitest.config.ts); this file has no JSX because none is needed, but the
// extension has to match to run at all.
import { describe, expect, it } from 'vitest'
import { base64ToBytes, unpackFrame } from './faceFrames.ts'
import type { Rgb } from './faceFrames.ts'

const W = 8
const H = 16 // two pages, so the test exercises the page boundary too

const RED: Rgb = { r: 200, g: 10, b: 5 }

/** Builds a W*H/8 framebuffer with exactly the given (x, y) pixels lit, in
 * face.h's own layout - bit n of byte `page*W + x` is row `page*8 + n` -
 * and returns it base64-encoded, the same way face_export.c writes it. */
function frameOf(lit: Array<[number, number]>): string {
  const bytes = new Uint8Array((W * H) / 8)
  for (const [x, y] of lit) {
    const page = y >> 3
    const bit = y & 7
    bytes[page * W + x] |= 1 << bit
  }
  return btoa(String.fromCharCode(...bytes))
}

describe('base64ToBytes + unpackFrame', () => {
  it('lights exactly the encoded pixels, in the page/bit layout face.h documents', () => {
    // (0, 0): bit 0 of page 0's first byte. (7, 7): bit 7 of page 0's last
    // byte - the top edge of the first page. (3, 8): bit 0 of page 1 - the
    // first row of the second page, where a wrong page computation would
    // land on page 0 instead.
    const lit: Array<[number, number]> = [
      [0, 0],
      [7, 7],
      [3, 8],
    ]
    const bytes = base64ToBytes(frameOf(lit))
    expect(bytes.length).toBe((W * H) / 8)

    const target = new Uint8ClampedArray(W * H * 4)
    unpackFrame(bytes, W, H, RED, target)

    const litSet = new Set(lit.map(([x, y]) => `${x},${y}`))
    for (let y = 0; y < H; y++) {
      for (let x = 0; x < W; x++) {
        const o = (y * W + x) * 4
        const pixel = [target[o], target[o + 1], target[o + 2], target[o + 3]]
        if (litSet.has(`${x},${y}`)) {
          expect(pixel).toEqual([RED.r, RED.g, RED.b, 255])
        } else {
          expect(pixel).toEqual([0, 0, 0, 0])
        }
      }
    }
  })

  it('clears a previously lit pixel rather than leaving it behind in a reused buffer', () => {
    // The buffer starts as if a previous frame had lit (0, 0); unpacking a
    // frame with nothing lit there must turn it back off, not skip it
    // because "there is nothing to draw".
    const target = new Uint8ClampedArray(W * H * 4)
    target.set([RED.r, RED.g, RED.b, 255], 0)

    const bytes = base64ToBytes(frameOf([]))
    unpackFrame(bytes, W, H, RED, target)

    expect([target[0], target[1], target[2], target[3]]).toEqual([0, 0, 0, 0])
  })
})
