// Regression coverage for one race in Mic.tsx's press-and-hold button: the
// person can release before beginRecording()'s `await startCapture()`
// settles (a permission dialog, or just getUserMedia()/addModule() being
// slow - tens to hundreds of milliseconds even on a fast tap), and
// handlePointerUp sees `recording` still false, so it has nothing to stop -
// see beginRecording's own comment on heldRef for the fix. This is the one
// piece of Mic.tsx narrow enough to drive without a real microphone or
// AudioContext: startCapture() is mocked as a controllable promise, and
// createPlayer()/describeMicError() are left as their real, DOM-free
// implementations from audio.ts since neither is ever invoked here (no
// binary reply frame is simulated, and the capture never actually fails).
//
// jsdom has no Pointer Events capture API at all - handlePointerDown calls
// setPointerCapture() unconditionally - so it is stubbed in beforeEach,
// same as any other jsdom gap.
import { act, cleanup, fireEvent, render } from '@testing-library/react'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import Mic from './Mic.tsx'
import type { TurnValue } from './useTurn.ts'
import type { CaptureController } from './audio.ts'

vi.mock('./audio.ts', async (importOriginal) => {
  const actual = await importOriginal<typeof import('./audio.ts')>()
  return { ...actual, startCapture: vi.fn() }
})

// Imported after the mock above so this binding is the mocked one - vitest
// hoists vi.mock() calls above imports, but the import itself still has to
// come after in source order to read the replaced export.
import { startCapture } from './audio.ts'

function makeTurn(overrides: Partial<TurnValue> = {}): TurnValue {
  return {
    connection: { status: 'open', reason: null },
    state: 'idle',
    turns: [],
    sendError: null,
    ask: vi.fn(() => true),
    interrupt: vi.fn(),
    startVoiceTurn: vi.fn(() => true),
    sendVoiceChunk: vi.fn(),
    endVoiceTurn: vi.fn(),
    abandonVoiceTurn: vi.fn(),
    onVoiceReply: vi.fn(() => () => {}),
    ...overrides,
  }
}

beforeEach(() => {
  Element.prototype.setPointerCapture = vi.fn()
  Element.prototype.releasePointerCapture = vi.fn()
})

afterEach(() => {
  cleanup()
  vi.restoreAllMocks()
})

describe('Mic', () => {
  it('stops the capture and ends the turn cleanly when the button is released before startCapture() resolves', async () => {
    // Finding C4: beginRecording() sends "start" (startVoiceTurn()) and then
    // awaits startCapture() before ever calling setRecording(true). Nothing
    // before this fix watched for the button being released during that
    // await, so capture began - and kept streaming - with the button
    // already up, the label stuck on "Recording - release to send", and
    // (per the finding) the server's watchdog eventually answering a full
    // minute of room audio out loud.
    let resolveCapture!: (controller: CaptureController) => void
    vi.mocked(startCapture).mockImplementation(
      () => new Promise<CaptureController>((resolve) => { resolveCapture = resolve }),
    )
    const controller: CaptureController = { stop: vi.fn() }
    const turn = makeTurn()

    const { getByRole } = render(<Mic turn={turn} />)
    const button = getByRole('button')

    // Press and release before startCapture() has settled.
    fireEvent.pointerDown(button, { pointerId: 1 })
    fireEvent.pointerUp(button, { pointerId: 1 })

    expect(turn.startVoiceTurn).toHaveBeenCalledTimes(1)
    // The bug this guards against: at this point `recording` is still
    // false, so handlePointerUp's `if (recording) stopRecording()` is a
    // no-op - there is nothing yet to tell it otherwise without heldRef.
    expect(turn.endVoiceTurn).not.toHaveBeenCalled()

    await act(async () => {
      resolveCapture(controller)
      // Let beginRecording's continuation run past the await.
      await Promise.resolve()
      await Promise.resolve()
    })

    expect(controller.stop).toHaveBeenCalledTimes(1)
    expect(turn.endVoiceTurn).toHaveBeenCalledTimes(1)
    // Capture genuinely started - this is endVoiceTurn's job, not
    // abandonVoiceTurn's (that is for startCapture() itself throwing).
    expect(turn.abandonVoiceTurn).not.toHaveBeenCalled()
    expect(button.textContent).toBe('Hold to speak')
  })

  it('the Space-key equivalent has the same fix: releasing during the await still stops a capture that started late', async () => {
    let resolveCapture!: (controller: CaptureController) => void
    vi.mocked(startCapture).mockImplementation(
      () => new Promise<CaptureController>((resolve) => { resolveCapture = resolve }),
    )
    const controller: CaptureController = { stop: vi.fn() }
    const turn = makeTurn()

    const { getByRole } = render(<Mic turn={turn} />)
    const button = getByRole('button')

    fireEvent.keyDown(button, { key: ' ' })
    fireEvent.keyUp(button, { key: ' ' })

    expect(turn.startVoiceTurn).toHaveBeenCalledTimes(1)
    expect(turn.endVoiceTurn).not.toHaveBeenCalled()

    await act(async () => {
      resolveCapture(controller)
      await Promise.resolve()
      await Promise.resolve()
    })

    expect(controller.stop).toHaveBeenCalledTimes(1)
    expect(turn.endVoiceTurn).toHaveBeenCalledTimes(1)
    expect(button.textContent).toBe('Hold to speak')
  })
})
