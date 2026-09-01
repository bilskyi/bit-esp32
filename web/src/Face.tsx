// The device's own eyes, in the browser.
//
// This does not draw a face - see firmware/host/face_preview.c's header for
// why: reimplementing the eyes in JavaScript drifts from firmware/main/face.c
// within a day. Instead it plays frames firmware/host/face_export.c rendered
// from that exact C, one short loop per state:emotion pair, committed as
// web/src/face-frames.json. This file's only job is picking the right loop
// for the live state/emotion and unpacking its frames onto a canvas -
// nothing here decides what an emotion looks like. Loop selection and frame
// decoding live in faceFrames.ts, not here, so this file exports only the
// component (see that file's header for why).
import { useCallback, useEffect, useRef, useState } from 'react'
import {
  FACE_FPS,
  FACE_H,
  FACE_W,
  getDecodedLoop,
  loopKey,
  parseHexColor,
  unpackFrame,
} from './faceFrames.ts'
import type { Rgb } from './faceFrames.ts'
import type { ConversationState } from './useTurn.ts'

function useReducedMotion(): boolean {
  const [reduced, setReduced] = useState(
    () => window.matchMedia('(prefers-reduced-motion: reduce)').matches,
  )
  useEffect(() => {
    const mql = window.matchMedia('(prefers-reduced-motion: reduce)')
    const onChange = () => setReduced(mql.matches)
    mql.addEventListener('change', onChange)
    return () => mql.removeEventListener('change', onChange)
  }, [])
  return reduced
}

interface FaceProps {
  /** The device's own half-duplex state - see useTurn.ts's ConversationState. */
  state: ConversationState
  /** The most recently chosen emotion, or null before one has ever arrived.
   * Chat.tsx derives this from turns rather than the current turn alone, so
   * it keeps showing the last mood between turns instead of snapping to
   * neutral the instant a new question starts. */
  emotion: string | null
  /** False exactly when useTurn()'s connection is not 'open'. */
  online: boolean
}

/** A 128x64 canvas playing the loop that matches the live state/emotion,
 * decoded from firmware/host/face_export.c's output - never drawn by this
 * file. See tokens.css and firmware/host/preview-template.html for the
 * design language: --glass for the ground, --emit for lit pixels, --grid
 * for the panel's texture. */
function Face({ state, emotion, online }: FaceProps) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null)
  const imageDataRef = useRef<ImageData | null>(null)
  const colorRef = useRef<Rgb | null>(null)
  const decodedRef = useRef<Map<string, Uint8Array[]>>(new Map())
  // The loop the animation effect below should be playing, as of the most
  // recent render. A ref rather than a dependency of that effect, so a
  // state or emotion change never tears down and restarts
  // requestAnimationFrame - it is picked up at the next frame boundary
  // instead (see the tick function there), which is the point: switching
  // loops mid-frame would be a visible jump-cut.
  const desiredKeyRef = useRef('idle:neutral')

  const reducedMotion = useReducedMotion()

  // The socket being down always shows the idle loop, not whatever the last
  // known state happened to be - a stale "listening" or "thinking" reads as
  // "still working on it" when the truth is "not talking to anything".
  const desiredKey = online ? loopKey(state, emotion) : 'idle:neutral'

  useEffect(() => {
    desiredKeyRef.current = desiredKey
  }, [desiredKey])

  useEffect(() => {
    const canvas = canvasRef.current
    if (!canvas) return
    const ctx = canvas.getContext('2d')
    if (!ctx) return
    imageDataRef.current = ctx.createImageData(FACE_W, FACE_H)
    colorRef.current = parseHexColor(
      getComputedStyle(document.documentElement).getPropertyValue('--emit') || '#cfeaff',
    )
  }, [])

  const paint = useCallback((key: string, frameIndex: number) => {
    const canvas = canvasRef.current
    const imageData = imageDataRef.current
    const color = colorRef.current
    if (!canvas || !imageData || !color) return
    const ctx = canvas.getContext('2d')
    if (!ctx) return
    const frames = getDecodedLoop(decodedRef.current, key)
    if (frames.length === 0) return
    unpackFrame(frames[frameIndex % frames.length], FACE_W, FACE_H, color, imageData.data)
    ctx.putImageData(imageData, 0, 0)
  }, [])

  // prefers-reduced-motion: paint whichever loop is current, but only its
  // first frame, and never start the animation effect below.
  useEffect(() => {
    if (reducedMotion) paint(desiredKey, 0)
  }, [reducedMotion, desiredKey, paint])

  useEffect(() => {
    if (reducedMotion) return

    let raf = 0
    let last = performance.now()
    let acc = 0
    let activeKey = desiredKeyRef.current
    let frameIndex = 0
    const frameDuration = 1000 / FACE_FPS

    paint(activeKey, frameIndex)

    const tick = (now: number) => {
      const dt = now - last
      last = now
      acc += dt
      // A frame boundary is exactly where a pending loop switch is allowed
      // to take effect - see desiredKeyRef's comment. Anywhere else would be
      // mid-frame.
      while (acc >= frameDuration) {
        acc -= frameDuration
        const desired = desiredKeyRef.current
        if (desired !== activeKey) {
          activeKey = desired
          frameIndex = 0
        } else {
          const frames = getDecodedLoop(decodedRef.current, activeKey)
          frameIndex = frames.length > 0 ? (frameIndex + 1) % frames.length : 0
        }
      }
      paint(activeKey, frameIndex)
      raf = requestAnimationFrame(tick)
    }
    raf = requestAnimationFrame(tick)
    return () => cancelAnimationFrame(raf)
  }, [reducedMotion, paint])

  // The raw value from the wire, not the sanitised one loopKey() falls back
  // to - if the two ever disagree, that mismatch is exactly what someone
  // debugging this panel needs to see, not something to paper over here too.
  const label = online ? (emotion ? `${state} · ${emotion}` : state) : 'offline'

  return (
    <div className="face">
      <div className="face-stage">
        <canvas
          ref={canvasRef}
          className="face-canvas"
          width={FACE_W}
          height={FACE_H}
          aria-hidden="true"
        />
      </div>
      <p className="face-readout">{label}</p>
    </div>
  )
}

export default Face
