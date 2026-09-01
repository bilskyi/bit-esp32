// Narrow on purpose, same reasoning as faceFrames.test.tsx: this covers only
// the pure, DOM-free math in audio.ts - float<->PCM16LE conversion and the
// resampling arithmetic. There is no browser and no microphone in this
// harness, so startCapture()'s AudioWorklet plumbing and createPlayer()'s
// AudioContext scheduling are exercised by hand in a real browser instead
// (see the task's report for exactly what to check). This file has no JSX
// because none is needed, but the extension has to match `*.test.tsx` (see
// vitest.config.ts) to run at all.
import { describe, expect, it } from 'vitest'
import { createResampler, describeMicError, floatTo16LE } from './audio.ts'

describe('floatTo16LE', () => {
  it('converts known float samples to the expected little-endian bytes', () => {
    const bytes = new Int16Array(floatTo16LE(new Float32Array([0, 0.5, -0.5])))
    // setInt16 truncates toward zero rather than rounding, so 0.5 * 0x7fff
    // (16383.5) lands on 16383, not 16384.
    expect(Array.from(bytes)).toEqual([0, Math.trunc(0.5 * 0x7fff), Math.trunc(-0.5 * 0x8000)])
  })

  it('clips at +1 and -1 rather than wrapping, including values past the boundary', () => {
    const bytes = new Int16Array(floatTo16LE(new Float32Array([1, -1, 2.5, -3.7])))
    expect(Array.from(bytes)).toEqual([0x7fff, -0x8000, 0x7fff, -0x8000])
  })
})

describe('createResampler', () => {
  it('returns each chunk unchanged when the rates already match', () => {
    const input = new Float32Array([0.1, 0.2, 0.3])
    expect(createResampler(16000, 16000).push(input)).toBe(input)
  })

  it('produces the expected sample count converting 48 kHz to 16 kHz in one push', () => {
    // 10 ms at 48 kHz is 480 samples; at 16 kHz the same 10 ms is 160 -
    // exactly the 3:1 ratio the brief's example calls out.
    const input = new Float32Array(480).fill(1)
    const output = createResampler(48000, 16000).push(input)
    expect(output.length).toBe(160)
  })

  it('averages whole groups of samples rather than picking every Nth one', () => {
    // Group 0 is samples [0,1,2] -> average 1; group 1 is [3,4,5] -> average
    // 4. Picking every 3rd sample instead would read 0 and 3, not 1 and 4 -
    // this is the aliasing the brief warns against.
    const input = new Float32Array([0, 1, 2, 3, 4, 5])
    const output = createResampler(3, 1).push(input)
    expect(Array.from(output)).toEqual([1, 4])
  })

  it('carries a partial group across two pushes instead of rounding each one independently', () => {
    // Ratio 3:1. First push has 4 samples: one full group [0,1,2] -> 1, plus
    // a lone leftover sample (3) that is not yet a complete group. A
    // stateless per-call implementation would have to round that lone
    // sample into its own output sample (or drop it) right there; this one
    // must hold it and fold it into the next push instead.
    const resampler = createResampler(3, 1)
    const first = resampler.push(new Float32Array([0, 1, 2, 3]))
    expect(Array.from(first)).toEqual([1])

    // Second push continues the carried-over sample (3) with two more
    // (4, 5) to complete group 1 -> average 4.
    const second = resampler.push(new Float32Array([4, 5]))
    expect(Array.from(second)).toEqual([4])
  })

  // Finding 1: downsample() used to be called fresh per 128-sample
  // AudioWorklet callback with no state carried between calls, so it
  // recomputed Math.round(chunkLength / ratio) every time - exact only when
  // the chunk length divides evenly by the ratio. Measured over 5 s of
  // 128-sample chunks, that came out +0.78% fast at 48 kHz -> 16 kHz and
  // -0.95% slow at 44.1 kHz -> 16 kHz, constant for the whole recording
  // because the rounding bias has the same sign every callback. This test
  // pins the property a stateful resampler must have instead: the total
  // output length across many small chunks converges on the exact ratio,
  // not the once-per-call rounding of it - it would not have caught the bug
  // if it only checked a single call, which is why it drives 128-sample
  // chunks for a real recording's worth of audio.
  it.each([48000, 44100])(
    'keeps total output length within one sample of the exact ratio across many 128-sample chunks (%d Hz)',
    (inputRate) => {
      const chunkLen = 128 // AudioWorkletProcessor's fixed render quantum
      const seconds = 5
      const totalInput = Math.floor((inputRate * seconds) / chunkLen) * chunkLen
      const resampler = createResampler(inputRate, 16000)

      let totalOutput = 0
      for (let i = 0; i < totalInput; i += chunkLen) {
        totalOutput += resampler.push(new Float32Array(chunkLen).fill(1)).length
      }

      const expected = (totalInput * 16000) / inputRate
      expect(Math.abs(totalOutput - expected)).toBeLessThanOrEqual(1)
    },
  )
})

describe('describeMicError', () => {
  it('tells the person how to grant permission on a denial, not a generic failure', () => {
    const message = describeMicError(new DOMException('denied', 'NotAllowedError'))
    expect(message).toMatch(/allow it/i)
  })

  it('names the secure-context requirement for the synthetic NotSupportedError', () => {
    const message = describeMicError(new DOMException('insecure', 'NotSupportedError'))
    expect(message).toMatch(/https|localhost/i)
  })

  it('falls back to a generic message for an error it does not recognise', () => {
    const message = describeMicError(new Error('boom'))
    expect(message.length).toBeGreaterThan(0)
  })
})
