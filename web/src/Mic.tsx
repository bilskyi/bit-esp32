// The mic control: press and hold to speak, mirroring the ESP32's own
// button - see audio.ts's header for the protocol this rides on, and
// useTurn.ts's startVoiceTurn/sendVoiceChunk/endVoiceTurn/onVoiceReply for
// the socket side of it. Owns capture and playback, not the socket - `turn`
// is the same useTurn() instance Chat.tsx already has, so this can sit
// beside the composer without a second connection.
import { useCallback, useEffect, useId, useRef, useState } from 'react'
import type { KeyboardEvent, PointerEvent } from 'react'
import type { TurnValue } from './useTurn.ts'
import { createPlayer, describeMicError, startCapture } from './audio.ts'
import type { CaptureController, Player } from './audio.ts'

interface MicProps {
  turn: TurnValue
}

/** A press-and-hold button with a keyboard equivalent (space, while
 * focused), a visible recording state, and a permission denial rendered as
 * a line telling the person how to fix it - never a dead button. Pressing
 * it always interrupts whatever is currently playing rather than being
 * disabled during it: the server's own on_start treats a fresh "start" as
 * exactly that (see server/session.py), so this mirrors the device's own
 * gesture instead of inventing a second one. The label says which. */
function Mic({ turn }: MicProps) {
  const { connection, state, startVoiceTurn, sendVoiceChunk, endVoiceTurn, onVoiceReply } = turn
  const [recording, setRecording] = useState(false)
  const [micError, setMicError] = useState<string | null>(null)
  const captureRef = useRef<CaptureController | null>(null)
  // Lazily created on the first reply chunk, not up front - there is
  // nothing to play until then, and constructing an AudioContext nobody
  // uses yet just to have it ready is not worth doing. See createPlayer's
  // own comment for why a browser's autoplay policy is still expected to
  // let this succeed even though it happens after the button press that
  // triggered it, not literally inside that gesture's call stack. Lives
  // across turns for as long as this component is mounted; stop() (an
  // interrupt) spends it, at which point the next chunk lazily makes a new
  // one.
  const playerRef = useRef<Player | null>(null)
  const errorId = useId()

  useEffect(() => {
    const unregister = onVoiceReply({
      onChunk: (data) => {
        if (!playerRef.current) playerRef.current = createPlayer()
        playerRef.current.push(data)
      },
      onDone: () => {
        playerRef.current?.finish()
      },
      onInterrupt: () => {
        playerRef.current?.stop()
        playerRef.current = null
      },
    })
    return () => {
      unregister()
      playerRef.current?.stop()
      playerRef.current = null
    }
  }, [onVoiceReply])

  // Never leave a live microphone stream behind - a tab switch, a socket
  // drop mid-hold, anything that unmounts this while captureRef is set.
  useEffect(() => {
    return () => {
      captureRef.current?.stop()
      captureRef.current = null
    }
  }, [])

  const disabled = connection.status !== 'open'
  const replying = state === 'speaking'

  const stopRecording = useCallback(() => {
    if (!captureRef.current) return
    captureRef.current.stop()
    captureRef.current = null
    setRecording(false)
    endVoiceTurn()
  }, [endVoiceTurn])

  // A disabled button stops delivering pointer/keyboard events at all in
  // some browsers, including the release that would normally end a
  // recording - so a connection dropping mid-hold cannot be left to the
  // button itself to notice. stopRecording() is already a no-op once there
  // is nothing to stop, so this only ever does something the one time it
  // needs to.
  useEffect(() => {
    if (disabled) stopRecording()
  }, [disabled, stopRecording])

  const beginRecording = useCallback(async () => {
    if (disabled || recording) return
    setMicError(null)
    // One message does double duty: the server treats "start" arriving
    // while it is still replying as an interruption on its own (see
    // startVoiceTurn's comment in useTurn.ts), so this is the entire
    // "pressing it interrupts" behaviour - no separate call needed.
    if (!startVoiceTurn()) return
    try {
      captureRef.current = await startCapture({
        onChunk: sendVoiceChunk,
        onError: (message) => {
          setMicError(message)
          stopRecording()
        },
      })
      setRecording(true)
    } catch (err) {
      setMicError(describeMicError(err))
    }
  }, [disabled, recording, startVoiceTurn, sendVoiceChunk, stopRecording])

  const handlePointerDown = (event: PointerEvent<HTMLButtonElement>) => {
    // Stops this from also firing a click/focus-drag text selection - a
    // press-and-hold button has no use for either. Explicit pointer capture
    // means the release still reaches this element even if the pointer
    // drifts off it before lifting - relying on the button's own bounding
    // box (a plain pointerleave) would otherwise let a real hold-and-drag
    // end the recording without ever seeing a pointerup.
    event.preventDefault()
    event.currentTarget.setPointerCapture(event.pointerId)
    void beginRecording()
  }
  const handlePointerUp = () => {
    if (recording) stopRecording()
  }
  const handleKeyDown = (event: KeyboardEvent<HTMLButtonElement>) => {
    if (event.key !== ' ' && event.key !== 'Spacebar') return
    event.preventDefault()
    // A held key repeats keydown continuously - only the first should start
    // a recording, or every repeat would try to start a new one.
    if (!event.repeat) void beginRecording()
  }
  const handleKeyUp = (event: KeyboardEvent<HTMLButtonElement>) => {
    if (event.key !== ' ' && event.key !== 'Spacebar') return
    event.preventDefault()
    if (recording) stopRecording()
  }

  const label = recording
    ? 'Recording — release to send'
    : replying
      ? 'Hold to interrupt and speak'
      : 'Hold to speak'

  return (
    <div className="mic">
      <button
        type="button"
        className={`mic-button${recording ? ' is-recording' : ''}`}
        disabled={disabled}
        aria-pressed={recording}
        aria-describedby={micError ? errorId : undefined}
        onPointerDown={handlePointerDown}
        onPointerUp={handlePointerUp}
        onPointerCancel={handlePointerUp}
        onKeyDown={handleKeyDown}
        onKeyUp={handleKeyUp}
      >
        {label}
      </button>
      {micError && (
        <p id={errorId} className="chat-status">
          {micError}
        </p>
      )}
    </div>
  )
}

export default Mic
