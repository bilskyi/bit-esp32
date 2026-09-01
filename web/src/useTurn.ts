// The one WebSocket for the Chat tab: connect, dispatch frames onto turns,
// reconnect with backoff, and hand the whole thing back as one hook so
// App.tsx can keep a single instance alive for the signed-in session (see
// the comment on SignedIn in App.tsx for why that matters).
//
// No JSX here on purpose, same reasoning as api.ts: the socket state machine
// doesn't need it, and keeping this a plain .ts file makes that obvious.

import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { Unauthorized, api, useSession } from './api'

/** Mirrors server/session.py's State enum. Global, not per-turn - the
 * device has one half-duplex state at a time, and Task 6 reads this for the
 * face regardless of which turn is in flight. */
export type ConversationState = 'idle' | 'listening' | 'thinking' | 'speaking'

export interface TraceFact {
  text: string
  score: number
}

/** What server/session.py's _trace() sends, verbatim. Task 4 renders this;
 * this task only has to carry it. */
export interface Trace {
  role: string
  surface: string
  facts: TraceFact[]
  prompt: string
  prompt_tokens: number
  completion_tokens: number
  emotion: string | null
  spoken: boolean
  stt_ms: number
  reply_ms: number
}

export interface Turn {
  id: number
  question: string
  /** One entry per `reply` frame, in arrival order. Joined with a space for
   * display - never replaced wholesale, since frames arrive one sentence at
   * a time and the whole point is to render as they land. */
  sentences: string[]
  emotion: string | null
  trace: Trace | null
  done: boolean
}

export interface Connection {
  status: 'connecting' | 'open' | 'unauthorized'
  /** The line the composer shows for why it is disabled. Null exactly when
   * status is 'open'. */
  reason: string | null
}

export interface TurnValue {
  connection: Connection
  state: ConversationState
  turns: Turn[]
  /** Set when the most recent ask() could not reach the socket - see ask()'s
   * comment. Null otherwise, including right after a later send that does
   * succeed or a fresh reconnect. Chat.tsx shows this so a refused question
   * is never silently lost - the person sees it and can just press send
   * again once connected, rather than the app queuing it behind their back. */
  sendError: string | null
  /** Returns whether the question was actually handed to the socket. A
   * caller that clears its draft unconditionally would lose the question
   * on a `false` return - see Chat.tsx's submit(). */
  ask: (text: string) => boolean
  interrupt: () => void
}

// 1s, 2s, 4s, then capped at 10s - reused for every attempt past the fourth.
const RECONNECT_DELAYS_MS = [1000, 2000, 4000, 10000]

interface ServerFrame {
  type?: string
  value?: unknown
}

function updateLastTurn(turns: Turn[], update: (turn: Turn) => Turn): Turn[] {
  if (turns.length === 0) return turns
  const lastIndex = turns.length - 1
  const next = turns.slice()
  next[lastIndex] = update(next[lastIndex])
  return next
}

/** Read from ask()/interrupt() before calling setTurns - never from inside
 * a setState updater. It decides whether pendingCancelsRef gets bumped, and
 * that has to happen exactly once per real call: React 19 StrictMode
 * invokes a functional setState updater twice (with the same previous
 * state) specifically to catch impure updaters, and a ref mutation living
 * in that updater used to get double-counted there, which is Finding 1 -
 * see markCurrentCancelled below for what that count is for. */
function hasCancellableTurn(turns: Turn[]): boolean {
  if (turns.length === 0) return false
  return !turns[turns.length - 1].done
}

/** Called from the setTurns updaters in ask() and interrupt(): the turn in
 * flight, if any, is about to be cancelled, so mark it done right away
 * rather than waiting on the server. Pure - checks turn.done itself before
 * doing anything, so calling it twice with the same turns array (exactly
 * what StrictMode's double-invoke does) is harmless.
 *
 * Marking it done locally isn't the whole story, though: the wire protocol
 * carries no turn id. When a question arrives mid-reply, session.on_text()
 * cancels the running task and *that* task's own finally block still sends
 * "done" for itself before the new turn produces anything - but by the time
 * it reaches the client, the new turn is already the current one. Left
 * unhandled, that stray "done" would close the new turn before a single
 * sentence arrived. pendingCancelsRef is how many such stray frames are
 * still owed; the "done" handler below swallows exactly that many before
 * applying one for real - see hasCancellableTurn above for where the count
 * itself gets incremented. */
function markCurrentCancelled(turns: Turn[]): Turn[] {
  if (turns.length === 0) return turns
  if (turns[turns.length - 1].done) return turns
  return updateLastTurn(turns, (turn) => ({ ...turn, done: true }))
}

export function useTurn(): TurnValue {
  const { notifyUnauthorized } = useSession()
  const [connection, setConnection] = useState<Connection>({
    status: 'connecting',
    reason: 'Connecting…',
  })
  const [state, setState] = useState<ConversationState>('idle')
  const [turns, setTurns] = useState<Turn[]>([])
  const [sendError, setSendError] = useState<string | null>(null)

  const socketRef = useRef<WebSocket | null>(null)
  const nextIdRef = useRef(0)
  const pendingCancelsRef = useRef(0)

  useEffect(() => {
    // Guards every handler below against setting state after this effect's
    // cleanup has run - React 19 StrictMode mounts, cleans up, and mounts
    // this effect again in dev, and a socket event that lands in the gap
    // must not touch a component that (as far as this effect is concerned)
    // is already gone.
    let alive = true
    let attempt = 0
    let retryTimer: ReturnType<typeof setTimeout> | undefined

    const scheduleReconnect = () => {
      if (!alive) return
      const delay = RECONNECT_DELAYS_MS[Math.min(attempt, RECONNECT_DELAYS_MS.length - 1)]
      attempt += 1
      setConnection({ status: 'connecting', reason: 'Reconnecting…' })
      retryTimer = setTimeout(connect, delay)
    }

    // Whether the handshake was actually refused (cookie gone) rather than
    // just failing to reach the server (down, offline) cannot be told apart
    // from the WebSocket close event alone: server/main.py's ws_endpoint
    // rejects an unauthorised connection with `websocket.close(code=4401)`
    // called *before* `accept()`, and uvicorn's websocket handler turns a
    // pre-accept close into a bare HTTP 403 - confirmed with a raw upgrade
    // request against a running server, no 101, no close frame, no code.
    // A real browser never sees 4401; it just sees a handshake that failed,
    // same as it would for an unreachable server. So when that happens, ask
    // the one endpoint that actually knows - the same GET /me api.ts's own
    // startup probe uses - rather than guess from the socket alone.
    const confirmStillSignedIn = async () => {
      try {
        await api.get('/me')
        scheduleReconnect()
      } catch (err) {
        if (!alive) return
        if (err instanceof Unauthorized) {
          setConnection({ status: 'unauthorized', reason: 'Session ended.' })
          notifyUnauthorized()
        } else {
          // Server unreachable, or some other hiccup - still worth retrying.
          scheduleReconnect()
        }
      }
    }

    const connect = () => {
      if (!alive) return

      setConnection({
        status: 'connecting',
        reason: attempt === 0 ? 'Connecting…' : 'Reconnecting…',
      })

      let openedThisAttempt = false
      const url = `${location.origin.replace(/^http/, 'ws')}/ws?device=default`
      const ws = new WebSocket(url)
      socketRef.current = ws

      ws.onopen = () => {
        if (!alive) return
        openedThisAttempt = true
        attempt = 0
        setConnection({ status: 'open', reason: null })
        // Whatever ask() last refused to send, a fresh connection makes it
        // worth trying again - stale wording from the last drop would just
        // confuse someone who has since reconnected.
        setSendError(null)
      }

      ws.onmessage = (event) => {
        if (!alive) return

        if (typeof event.data !== 'string') {
          // Binary frames are TTS audio (see server/session.py's `render`).
          // A typed question only ever gets `reply` text frames - voice
          // playback on the web client is Task 7's job. Ignoring binary
          // frames here is expected, not a bug: nothing plays because there
          // is nothing to play yet.
          return
        }

        let frame: ServerFrame
        try {
          frame = JSON.parse(event.data) as ServerFrame
        } catch {
          return
        }

        switch (frame.type) {
          case 'state':
            setState(frame.value as ConversationState)
            break
          case 'emotion':
            setTurns((prev) =>
              updateLastTurn(prev, (turn) => ({ ...turn, emotion: frame.value as string })),
            )
            break
          case 'reply':
            setTurns((prev) =>
              updateLastTurn(prev, (turn) => ({
                ...turn,
                sentences: [...turn.sentences, frame.value as string],
              })),
            )
            break
          case 'trace':
            setTurns((prev) =>
              updateLastTurn(prev, (turn) => ({ ...turn, trace: frame.value as Trace })),
            )
            break
          case 'done':
            if (pendingCancelsRef.current > 0) {
              // The ack for a turn we already cancelled locally - see
              // cancelCurrentTurn's comment. Not this turn's done.
              pendingCancelsRef.current -= 1
              break
            }
            setTurns((prev) => updateLastTurn(prev, (turn) => ({ ...turn, done: true })))
            break
          default:
            // The server may grow frame types before this app does; an
            // unknown one is ignored, not an error.
            break
        }
      }

      ws.onclose = (event) => {
        socketRef.current = null
        if (!alive) return

        if (event.code === 4401) {
          // Kept for the day server/main.py closes *after* accept() (or a
          // future proxy layer preserves it): the cookie is gone, and
          // reconnecting would just be knocking on a door that never opens
          // again this session - go back to login, like any other 401.
          setConnection({ status: 'unauthorized', reason: 'Session ended.' })
          notifyUnauthorized()
          return
        }

        if (!openedThisAttempt) {
          // This attempt's handshake never completed - could be the 403
          // above, could be the server simply being down. confirmStillSignedIn
          // tells the two apart.
          void confirmStillSignedIn()
          return
        }

        scheduleReconnect()
      }

      ws.onerror = () => {
        // A close event always follows an error on a WebSocket, so all the
        // reconnect bookkeeping lives in onclose alone; handling it here
        // too would schedule it twice.
        ws.close()
      }
    }

    connect()

    return () => {
      alive = false
      if (retryTimer !== undefined) clearTimeout(retryTimer)
      const ws = socketRef.current
      socketRef.current = null
      if (ws) {
        ws.onopen = null
        ws.onmessage = null
        ws.onclose = null
        ws.onerror = null
        ws.close()
      }
    }
  }, [notifyUnauthorized])

  const ask = useCallback(
    (text: string): boolean => {
      const socket = socketRef.current
      if (!socket || socket.readyState !== WebSocket.OPEN) {
        // The composer's disabled prop is one render behind the socket in
        // the gap between onclose nulling socketRef and the state update
        // that disables the UI reaching the screen - a question submitted
        // in that gap must not look accepted when it never reached the
        // server. Refuse outright rather than queuing it: this app's whole
        // point is that a question either goes now or the person sees it
        // did not and can retry, never a silent drop and never a hidden
        // queue firing later against whatever has changed by then.
        setSendError('Not connected - your question was not sent. Try again once reconnected.')
        return false
      }

      setSendError(null)
      if (hasCancellableTurn(turns)) pendingCancelsRef.current += 1
      const id = nextIdRef.current++
      setTurns((prev) => [
        ...markCurrentCancelled(prev),
        { id, question: text, sentences: [], emotion: null, trace: null, done: false },
      ])
      socket.send(JSON.stringify({ type: 'text', value: text }))
      return true
    },
    [turns],
  )

  const interrupt = useCallback(() => {
    if (hasCancellableTurn(turns)) pendingCancelsRef.current += 1
    setTurns((prev) => markCurrentCancelled(prev))
    socketRef.current?.send(JSON.stringify({ type: 'cancel' }))
  }, [turns])

  return useMemo(
    () => ({ connection, state, turns, sendError, ask, interrupt }),
    [connection, state, turns, sendError, ask, interrupt],
  )
}
