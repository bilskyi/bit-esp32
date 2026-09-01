// Regression coverage for the socket hook only - see the review that added
// this file for why useTurn() gets a test harness when nothing else in
// web/ does. Narrow on purpose: this exists for this hook, not as a
// template for testing every component.
//
// React 19's StrictMode - which main.tsx enables - double-invokes the
// updater function passed to a functional setState call, specifically to
// catch impure updaters. Every test here renders the hook inside
// <StrictMode> for that reason: a test that skipped it would not exercise
// the bug the first test below is guarding against.
import { StrictMode } from 'react'
import type { ReactNode } from 'react'
import { act, cleanup, renderHook } from '@testing-library/react'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { SessionContext } from './api'
import type { SessionValue } from './api'
import { useTurn } from './useTurn.ts'

/** Stands in for a real WebSocket. StrictMode mounts useTurn()'s connect
 * effect twice in dev (mount, cleanup, mount again), so at least two of
 * these get created per render; lastSocket() below always fetches the
 * survivor. */
class FakeWebSocket {
  static readonly CONNECTING = 0
  static readonly OPEN = 1
  static readonly CLOSING = 2
  static readonly CLOSED = 3
  static instances: FakeWebSocket[] = []

  url: string
  readyState = FakeWebSocket.CONNECTING
  onopen: (() => void) | null = null
  onclose: ((event: { code: number }) => void) | null = null
  onmessage: ((event: { data: string }) => void) | null = null
  onerror: (() => void) | null = null
  sent: string[] = []

  constructor(url: string) {
    this.url = url
    FakeWebSocket.instances.push(this)
  }

  send(data: string) {
    if (this.readyState !== FakeWebSocket.OPEN) {
      throw new Error('FakeWebSocket.send() called while not open')
    }
    this.sent.push(data)
  }

  close(code = 1000) {
    this.readyState = FakeWebSocket.CLOSED
    this.onclose?.({ code })
  }

  /** Test helper: completes the handshake. */
  open() {
    this.readyState = FakeWebSocket.OPEN
    this.onopen?.()
  }

  /** Test helper: delivers one server frame, matching the JSON text frames
   * server/session.py sends - see useTurn.ts's onmessage switch. */
  frame(value: Record<string, unknown>) {
    this.onmessage?.({ data: JSON.stringify(value) })
  }
}

function lastSocket(): FakeWebSocket {
  const ws = FakeWebSocket.instances.at(-1)
  if (!ws) throw new Error('no FakeWebSocket instance was created')
  return ws
}

// A fresh, stable object per test - read by the wrapper below via closure
// rather than built inside it, so its identity never changes mid-test even
// if React re-invokes the wrapper's render function.
let session: SessionValue

beforeEach(() => {
  FakeWebSocket.instances = []
  vi.stubGlobal('WebSocket', FakeWebSocket)
  session = {
    state: 'in',
    username: 'tester',
    notice: null,
    signIn: vi.fn(),
    signOut: vi.fn(),
    notifyUnauthorized: vi.fn(),
  }
})

afterEach(() => {
  cleanup()
  vi.unstubAllGlobals()
})

function wrapper({ children }: { children: ReactNode }) {
  return (
    <StrictMode>
      <SessionContext.Provider value={session}>{children}</SessionContext.Provider>
    </StrictMode>
  )
}

describe('useTurn', () => {
  it('does not swallow the second turn\'s real "done" after an interrupt under StrictMode', () => {
    // Finding 1: cancelCurrentTurn used to bump pendingCancelsRef.current
    // from inside the setTurns updater. StrictMode invokes that updater
    // twice with the same previous state to check it is pure, so the ref
    // got double-counted, and the extra count then ate the *next* turn's
    // genuine "done" frame - leaving it stuck showing "thinking..." even
    // though its reply had streamed in full.
    const { result } = renderHook(() => useTurn(), { wrapper })
    const ws = lastSocket()
    act(() => ws.open())

    act(() => {
      result.current.ask('first question')
    })
    act(() => ws.frame({ type: 'reply', value: 'Partial answer.' }))

    // Interrupts the still-streaming first turn - the exact moment the
    // pre-fix code double-counted the pending cancel.
    act(() => {
      result.current.ask('second question')
    })

    // The stray "done" for the cancelled first turn (see useTurn.ts's
    // comment on markCurrentCancelled/pendingCancelsRef) - meant to be
    // swallowed.
    act(() => ws.frame({ type: 'done' }))
    // The second turn's reply, then its own, genuine "done".
    act(() => ws.frame({ type: 'reply', value: 'Second answer.' }))
    act(() => ws.frame({ type: 'done' }))

    expect(result.current.turns).toHaveLength(2)
    expect(result.current.turns[0].done).toBe(true)
    expect(result.current.turns[1].sentences).toEqual(['Second answer.'])
    expect(result.current.turns[1].done).toBe(true)
  })

  it('refuses to send when the socket is not open, and leaves no turn claiming to be in progress', () => {
    // Finding 2: ask() used to trust the composer's disabled prop and send
    // unconditionally. In the gap between onclose nulling socketRef and the
    // re-render that disables the UI, a question could be accepted,
    // appended to turns as "thinking...", and never actually reach the
    // socket.
    const { result } = renderHook(() => useTurn(), { wrapper })
    const ws = lastSocket()
    // Deliberately never opened - readyState stays CONNECTING, standing in
    // for that gap.

    let accepted = true
    act(() => {
      accepted = result.current.ask('are you there?')
    })

    expect(accepted).toBe(false)
    expect(result.current.turns).toHaveLength(0)
    expect(result.current.sendError).toBeTruthy()
    expect(ws.sent).toHaveLength(0)
  })

  it('accumulates reply frames in arrival order and only marks the turn done once "done" lands', () => {
    const { result } = renderHook(() => useTurn(), { wrapper })
    const ws = lastSocket()
    act(() => ws.open())

    act(() => {
      result.current.ask('tell me something')
    })

    act(() => ws.frame({ type: 'reply', value: 'One.' }))
    act(() => ws.frame({ type: 'reply', value: 'Two.' }))
    act(() => ws.frame({ type: 'reply', value: 'Three.' }))

    expect(result.current.turns[0].sentences).toEqual(['One.', 'Two.', 'Three.'])
    expect(result.current.turns[0].done).toBe(false)

    act(() => ws.frame({ type: 'done' }))

    expect(result.current.turns[0].sentences).toEqual(['One.', 'Two.', 'Three.'])
    expect(result.current.turns[0].done).toBe(true)
  })
})
