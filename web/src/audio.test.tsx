// Narrow on purpose, same reasoning as faceFrames.test.tsx: this covers only
// the pure, DOM-free math in audio.ts - float<->PCM16LE conversion and the
// resampling arithmetic. There is no browser and no microphone in this
// harness, so startCapture()'s AudioWorklet plumbing and createPlayer()'s
// AudioContext scheduling are exercised by hand in a real browser instead
// (see the task's report for exactly what to check). This file has no JSX
// because none is needed, but the extension has to match `*.test.tsx` (see
// vitest.config.ts) to run at all.
import { describe, expect, it } from 'vitest'
import { describeMicError, downsample, floatTo16LE } from './audio.ts'

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

describe('downsample', () => {
  it('returns the input unchanged when the rates already match', () => {
    const input = new Float32Array([0.1, 0.2, 0.3])
    expect(downsample(input, 16000, 16000)).toBe(input)
  })

  it('produces the expected sample count converting 48 kHz to 16 kHz', () => {
    // 10 ms at 48 kHz is 480 samples; at 16 kHz the same 10 ms is 160 -
    // exactly the 3:1 ratio the brief's example calls out.
    const input = new Float32Array(480).fill(1)
    const output = downsample(input, 48000, 16000)
    expect(output.length).toBe(160)
  })

  it('averages whole groups of samples rather than picking every Nth one', () => {
    // Group 0 is samples [0,1,2] -> average 1; group 1 is [3,4,5] -> average
    // 4. Picking every 3rd sample instead would read 0 and 3, not 1 and 4 -
    // this is the aliasing the brief warns against.
    const input = new Float32Array([0, 1, 2, 3, 4, 5])
    const output = downsample(input, 3, 1)
    expect(Array.from(output)).toEqual([1, 4])
  })
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
