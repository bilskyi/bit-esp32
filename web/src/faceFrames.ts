// Loop selection and frame decoding for the Face panel - the logic behind
// Face.tsx that a browser click-through cannot falsify (same reasoning as
// settingsApi.ts/useTurn.ts), and split out of Face.tsx itself so that .tsx
// file exports nothing but the component: oxlint's react/only-export-components
// flags a component file that also exports plain functions, because it
// breaks Fast Refresh.
//
// Nothing here draws anything. face-frames.json's frames come from
// firmware/host/face_export.c, which drives the real firmware/main/face.c -
// see that file's header for why this is not reimplemented in JavaScript.
import faceFrames from './face-frames.json'
import type { ConversationState } from './useTurn.ts'

export interface FaceFramesFile {
  w: number
  h: number
  fps: number
  loops: Record<string, string[]>
}

// The single cast site for the generated file's shape. If face_export.c's
// output shape ever changes, this is where it breaks loudly instead of
// wherever FRAMES happens to get indexed next.
const FRAMES = faceFrames as FaceFramesFile

export const FACE_W = FRAMES.w
export const FACE_H = FRAMES.h
export const FACE_FPS = FRAMES.fps

export interface Rgb {
  r: number
  g: number
  b: number
}

// tokens.css's --emit, copied here only as the last-resort fallback if the
// custom property cannot be read at all - the same fallback
// preview-template.html's own paint() falls back to.
export const DEFAULT_EMIT: Rgb = { r: 0xcf, g: 0xea, b: 0xff }

export function parseHexColor(input: string): Rgb {
  const hex = input.trim().replace(/^#/, '')
  if (hex.length === 3) {
    return {
      r: parseInt(hex[0] + hex[0], 16),
      g: parseInt(hex[1] + hex[1], 16),
      b: parseInt(hex[2] + hex[2], 16),
    }
  }
  if (hex.length === 6) {
    return {
      r: parseInt(hex.slice(0, 2), 16),
      g: parseInt(hex.slice(2, 4), 16),
      b: parseInt(hex.slice(4, 6), 16),
    }
  }
  return DEFAULT_EMIT
}

// Exactly the nine names face.h's face_emotion_t enumerates, server/emotion.py
// carries on the wire, and face_export.c used to build face-frames.json's
// loop keys. Kept as an explicit set rather than trusting whatever string
// arrives: a name that doesn't match this list is the likeliest bug in this
// whole feature, and it must fail visibly rather than silently render
// nothing (or, worse, throw and blank the panel).
const KNOWN_EMOTIONS = new Set([
  'neutral',
  'happy',
  'excited',
  'curious',
  'confused',
  'surprised',
  'sad',
  'annoyed',
  'sleepy',
])

let warnedUnknownEmotion = false
const warnedMissingLoop = new Set<string>()

/** The loop key to animate for a given state/emotion pair - always a key
 * that actually exists in FRAMES.loops, falling back to "neutral" for an
 * emotion this build of face-frames.json does not have (logged once, not
 * per frame, so a stuck connection does not spam the console). */
export function loopKey(state: ConversationState, emotion: string | null): string {
  if (emotion !== null && !KNOWN_EMOTIONS.has(emotion)) {
    if (!warnedUnknownEmotion) {
      warnedUnknownEmotion = true
      console.warn(`Face: unknown emotion "${emotion}" from the server, falling back to neutral`)
    }
    return `${state}:neutral`
  }
  return `${state}:${emotion ?? 'neutral'}`
}

function resolveLoop(key: string): string[] {
  const loop = FRAMES.loops[key]
  if (loop && loop.length > 0) return loop
  if (!warnedMissingLoop.has(key)) {
    warnedMissingLoop.add(key)
    console.warn(`Face: no frames for "${key}" in face-frames.json, falling back to idle:neutral`)
  }
  return FRAMES.loops['idle:neutral'] ?? []
}

/** Base64 -> raw framebuffer bytes, one FACE_W*FACE_H/8 buffer per frame. */
export function base64ToBytes(b64: string): Uint8Array {
  const bin = atob(b64)
  const bytes = new Uint8Array(bin.length)
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i)
  return bytes
}

/** One framebuffer's bytes -> a reused RGBA buffer (an ImageData's `.data`
 * in Face.tsx's render path; a plain Uint8ClampedArray in
 * faceFrames.test.tsx). This is the layout face.h documents and
 * firmware/host/preview-template.html's own decoder uses: 8 pages of `w`
 * columns, bit n of byte `page*w + x` being row `page*8 + n`. Writes every
 * pixel unconditionally, lit or not, so a stale pixel from the previous
 * frame can never survive into this one in a buffer that gets reused. */
export function unpackFrame(
  bytes: Uint8Array,
  w: number,
  h: number,
  color: Rgb,
  target: Uint8ClampedArray,
): void {
  for (let y = 0; y < h; y++) {
    const page = (y >> 3) * w
    const mask = 1 << (y & 7)
    const rowOffset = y * w * 4
    for (let x = 0; x < w; x++) {
      const o = rowOffset + x * 4
      if ((bytes[page + x] & mask) !== 0) {
        target[o] = color.r
        target[o + 1] = color.g
        target[o + 2] = color.b
        target[o + 3] = 255
      } else {
        target[o] = 0
        target[o + 1] = 0
        target[o + 2] = 0
        target[o + 3] = 0
      }
    }
  }
}

/** Decodes a loop's frames once and remembers them in `cache` (keyed by
 * loop key), so switching back to a state:emotion pair already visited this
 * session never re-runs atob on the same 30-ish base64 strings. */
export function getDecodedLoop(cache: Map<string, Uint8Array[]>, key: string): Uint8Array[] {
  let decoded = cache.get(key)
  if (!decoded) {
    decoded = resolveLoop(key).map(base64ToBytes)
    cache.set(key, decoded)
  }
  return decoded
}
