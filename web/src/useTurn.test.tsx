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
    // The turn being cancelled below has actually reached the server state
    // (SPEAKING) where a reply task exists and a cancel really does owe a
    // stray "done" - see cancelCurrentTurn's state gate in useTurn.ts. Without
    // this frame the swallow this test checks for would not happen at all,
    // and the test would pass for the wrong reason.
    act(() => ws.frame({ type: 'state', value: 'speaking' }))

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

  it('does not double-count two real cancels in the same tick, so the next turn\'s real "done" is not swallowed', () => {
    // Task 7's carried-forward defect: hasCancellableTurn()/pendingCancelsRef
    // used to read `turns` from the enclosing closure. A single call is fine
    // that way, but two *real* calls landing in the same synchronous tick
    // (two rapid presses of an interrupt/mic control, say) both see the same
    // stale `turns` and both bump the ref - even though Session.on_cancel
    // (server/session.py) clears self._reply on the first cancel, so a
    // second cancel back-to-back is a no-op there and the server sends only
    // one stray "done", not two. The extra count then swallows the *next*
    // turn's genuine "done". cancelCurrentTurn fixes this by deriving
    // everything from the `prev` each setTurns updater actually receives,
    // guarded against StrictMode's separate double-invoke-with-same-prev
    // behaviour - see its comment in useTurn.ts.
    const { result } = renderHook(() => useTurn(), { wrapper })
    const ws = lastSocket()
    act(() => ws.open())

    act(() => {
      result.current.ask('first question')
    })
    act(() => ws.frame({ type: 'reply', value: 'Partial answer.' }))
    // As above: the state gate needs the server to actually be THINKING or
    // SPEAKING for a cancel to owe a stray "done" at all.
    act(() => ws.frame({ type: 'state', value: 'thinking' }))

    // Two real interrupts in one tick - both actually reach the socket, but
    // only the first is a real cancellation server-side.
    act(() => {
      result.current.interrupt()
      result.current.interrupt()
    })
    expect(ws.sent.filter((s) => s.includes('"cancel"'))).toHaveLength(2)

    // The single stray "done" the server actually sends for the pair.
    act(() => ws.frame({ type: 'done' }))

    // A fresh question after the interrupt - its own "done" must land, not
    // be eaten by a pendingCancelsRef left over-counted from above.
    act(() => {
      result.current.ask('second question')
    })
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

  it('a failed capture leaves no live turn and no pending cancel, so the next turn\'s own "done" is not swallowed', () => {
    // Finding 2: Mic.tsx's beginRecording() sends "start" via startVoiceTurn()
    // before calling startCapture() - if that throws (permission denied, no
    // device, a suspended context), nothing further used to be sent for this
    // turn: no "end", so server/session.py's on_end/_run_reply never ran for
    // it, so nothing would ever cancel a reply task that does not exist, so
    // no "done" was ever owed for it. cancelCurrentTurn did not know that: a
    // retry's startVoiceTurn() still bumped pendingCancelsRef expecting one
    // stray "done" that server/session.py's on_start "start while already
    // listening" branch (a deliberate, silent drop - see its own comment)
    // never sends - permanently off by one, silently eating the *next* real
    // turn's "done" and stranding it on "thinking…" forever. Confirmed by a
    // RED version of this test (no abandonVoiceTurn() call) that failed on
    // exactly `turns[1].done` before this fix.
    const { result } = renderHook(() => useTurn(), { wrapper })
    const ws = lastSocket()
    act(() => ws.open())

    act(() => {
      result.current.startVoiceTurn()
    })
    // Capture fails right here in the real flow - Mic.tsx's beginRecording()
    // catch calls this instead of leaving the turn dangling.
    act(() => {
      result.current.abandonVoiceTurn()
    })

    expect(result.current.turns).toHaveLength(1)
    expect(result.current.turns[0].done).toBe(true)

    // A retry, exactly like pressing the mic again after seeing the error.
    act(() => {
      result.current.startVoiceTurn()
    })
    act(() => ws.frame({ type: 'done' }))

    expect(result.current.turns).toHaveLength(2)
    expect(result.current.turns[1].done).toBe(true)
  })

  it('abandonVoiceTurn is a harmless no-op once the turn is already done', () => {
    // Guards the same StrictMode double-invoke concern cancelCurrentTurn's
    // own comment describes: nothing here should double-finalise or throw if
    // called again, or if the last turn already finished on its own.
    const { result } = renderHook(() => useTurn(), { wrapper })
    const ws = lastSocket()
    act(() => ws.open())

    act(() => {
      result.current.startVoiceTurn()
    })
    act(() => ws.frame({ type: 'done' }))
    expect(result.current.turns[0].done).toBe(true)

    act(() => {
      result.current.abandonVoiceTurn()
    })
    expect(result.current.turns).toHaveLength(1)
    expect(result.current.turns[0].done).toBe(true)
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

  it('finalises an orphaned turn and drops stale accounting when the socket reconnects mid-turn', () => {
    // Finding C3(a): server/main.py hands a fresh socket a brand-new
    // Session, which owes nothing for a turn the *previous* socket never
    // finished. The pre-fix code left that turn `done: false` forever and
    // still bumped pendingCancelsRef on the next ask(), expecting a stray
    // "done" the new Session was never going to send - which then ate that
    // next turn's genuine "done" instead. ws.onopen now resets the
    // accounting and finalises the dangling turn itself.
    vi.useFakeTimers()
    try {
      const { result } = renderHook(() => useTurn(), { wrapper })
      const ws1 = lastSocket()
      act(() => ws1.open())

      act(() => {
        result.current.ask('first question')
      })
      act(() => ws1.frame({ type: 'state', value: 'thinking' }))
      act(() => ws1.frame({ type: 'reply', value: 'Partial answer.' }))

      // The socket drops mid-reply - no "end", no "done" ever follows for
      // this turn on this socket.
      act(() => ws1.close())

      // RECONNECT_DELAYS_MS[0] is 1000ms.
      act(() => {
        vi.advanceTimersByTime(1000)
      })
      const ws2 = lastSocket()
      expect(ws2).not.toBe(ws1)
      act(() => ws2.open())

      // The fix lands entirely in ws.onopen, before any frame from the new
      // session has arrived: the orphaned turn is finalised locally (so it
      // stops looking cancellable) and the stale "thinking" state does not
      // survive the reconnect (I5) - the face/mic must not keep announcing
      // a reply that the new session knows nothing about.
      expect(result.current.turns[0].done).toBe(true)
      expect(result.current.state).toBe('idle')
      expect(result.current.connection.status).toBe('open')

      act(() => {
        result.current.ask('second question')
      })
      act(() => ws2.frame({ type: 'state', value: 'thinking' }))
      act(() => ws2.frame({ type: 'reply', value: 'Second answer.' }))
      act(() => ws2.frame({ type: 'done' }))

      // Pre-fix, this "done" was swallowed by a pendingCancelsRef the
      // reconnect never should have left non-zero - stranding this turn on
      // "thinking..." forever, and with it the inspector, which
      // Message.tsx gates on `turn.done`.
      expect(result.current.turns).toHaveLength(2)
      expect(result.current.turns[1].sentences).toEqual(['Second answer.'])
      expect(result.current.turns[1].done).toBe(true)
    } finally {
      vi.useRealTimers()
    }
  })

  it('does not swallow a typed question\'s "done" when it interrupts a turn still LISTENING', () => {
    // Finding C3(b): server/session.py's on_text, when the server is still
    // LISTENING (a voice turn in progress, no "end" sent yet), just clears
    // the audio buffer and starts a reply - it never calls on_cancel,
    // because no reply task exists yet to cancel. So exactly one "done"
    // comes back for the typed turn, not two. The frame sequence below is
    // exactly what the reviewer observed against a live mock server for
    // start-then-text. The pre-fix code bumped pendingCancelsRef whenever
    // it cancelled *any* live turn, regardless of server state, and ate
    // that one "done" - stranding the typed turn on "thinking..." forever.
    const { result } = renderHook(() => useTurn(), { wrapper })
    const ws = lastSocket()
    act(() => ws.open())

    act(() => {
      result.current.startVoiceTurn()
    })
    act(() => ws.frame({ type: 'state', value: 'listening' }))

    act(() => {
      result.current.ask('typed while listening')
    })
    act(() => ws.frame({ type: 'state', value: 'thinking' }))
    act(() => ws.frame({ type: 'emotion', value: 'curious' }))
    act(() => ws.frame({ type: 'state', value: 'speaking' }))
    act(() => ws.frame({ type: 'reply', value: 'One.' }))
    act(() => ws.frame({ type: 'reply', value: 'Two.' }))
    act(() =>
      ws.frame({
        type: 'trace',
        value: {
          role: 'r', surface: 'web', facts: [], prompt: '', prompt_tokens: 0,
          completion_tokens: 0, emotion: 'curious', spoken: false, stt_ms: 0, reply_ms: 0,
        },
      }),
    )
    act(() => ws.frame({ type: 'done' }))

    expect(result.current.turns).toHaveLength(2)
    expect(result.current.turns[0].done).toBe(true)
    expect(result.current.turns[1].sentences).toEqual(['One.', 'Two.'])
    expect(result.current.turns[1].done).toBe(true)

    // A further question must not inherit any leftover miscount either.
    act(() => {
      result.current.ask('third question')
    })
    act(() => ws.frame({ type: 'state', value: 'thinking' }))
    act(() => ws.frame({ type: 'reply', value: 'Third answer.' }))
    act(() => ws.frame({ type: 'done' }))

    expect(result.current.turns).toHaveLength(3)
    expect(result.current.turns[2].done).toBe(true)
  })
})
