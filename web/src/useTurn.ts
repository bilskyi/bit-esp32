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
  /** True for a turn started by startVoiceTurn() rather than ask(). The
   * server never sends the transcript of a spoken question back, nor the
   * text of a spoken reply (that goes out as binary PCM instead - see
   * onVoiceReply below), so `question` is a fixed placeholder and
   * `sentences` legitimately stays empty for the whole turn even when it
   * succeeds. Message.tsx and Inspector.tsx need this flag to tell that
   * apart from an interrupted typed turn, which also has no sentences but
   * for a different reason. */
  spoken: boolean
  /** One entry per `reply` frame, in arrival order. Joined with a space for
   * display - never replaced wholesale, since frames arrive one sentence at
   * a time and the whole point is to render as they land. Always empty for
   * a spoken turn - see `spoken` above. */
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

/** What Mic.tsx registers to receive the binary/completion side of a spoken
 * reply - see onVoiceReply below for why this bypasses React state. */
export interface VoiceReplyHandlers {
  /** A binary frame arrived: one chunk of PCM16LE audio for the reply in
   * progress, ready for audio.ts's Player.push(). */
  onChunk: (data: ArrayBuffer) => void
  /** This reply's own `done` frame arrived - no more chunks are coming for
   * it. Whatever is already scheduled keeps playing; see Player.finish(). */
  onDone: () => void
  /** A new question - typed or spoken - just cancelled whatever was still
   * in flight, exactly like the device's own button interrupting a reply
   * (server/session.py's on_start). Whatever is currently sounding should
   * stop immediately, not fade out; see Player.stop(). */
  onInterrupt: () => void
}

export interface TurnValue {
  connection: Connection
  state: ConversationState
  turns: Turn[]
  /** Set when the most recent ask() or startVoiceTurn() could not reach the
   * socket - see ask()'s comment. Null otherwise, including right after a
   * later send that does succeed or a fresh reconnect. Chat.tsx shows this
   * so a refused question is never silently lost - the person sees it and
   * can just press send (or the mic) again once connected, rather than the
   * app queuing it behind their back. */
  sendError: string | null
  /** Returns whether the question was actually handed to the socket. A
   * caller that clears its draft unconditionally would lose the question
   * on a `false` return - see Chat.tsx's submit(). */
  ask: (text: string) => boolean
  interrupt: () => void
  /** Sends `{"type":"start"}` and opens a new turn locally, mirroring the
   * device's own button-down: the server treats this exactly like a typed
   * question for cancelling whatever reply is still in flight (on_start
   * calls on_cancel when THINKING or SPEAKING), so pressing the mic while a
   * reply plays interrupts it rather than needing a separate call. Returns
   * whether it actually reached the socket, same contract as ask(). Binary
   * chunks follow via sendVoiceChunk(), then endVoiceTurn(). */
  startVoiceTurn: () => boolean
  /** One binary chunk of the utterance in progress - sent immediately,
   * never buffered client-side, matching the device's own streaming (see
   * this file's header and README.md's protocol table). A no-op if the
   * socket is not open, same as sendError's other silent-drop cases. */
  sendVoiceChunk: (chunk: ArrayBuffer) => void
  /** Sends `{"type":"end"}` - the button was released. */
  endVoiceTurn: () => void
  /** Marks the last turn done locally without expecting any server
   * acknowledgement and without touching pendingCancelsRef - see Mic.tsx's
   * beginRecording(), the only caller. That is deliberately different from
   * cancelCurrentTurn (ask()/interrupt()/startVoiceTurn()'s shared path),
   * which always counts one stray "done" it expects the server to send for
   * whatever it cancels. That assumption holds for a turn that reached
   * THINKING or SPEAKING (on_cancel there really does cancel a running reply
   * task, whose `finally` sends "done"), but not for a voice turn where
   * "start" was sent and startCapture() then threw before any audio, let
   * alone "end", ever followed: server/session.py never created a reply task
   * for it, so nothing is left to cancel, so a retry's "start" hits on_start's
   * "start while already listening" branch, which drops it silently and
   * owes no "done" at all (see that branch's own comment). Counting one
   * there anyway is exactly the bug this fixes: it left pendingCancelsRef
   * permanently off by one, silently swallowing the *next* turn's genuine
   * "done" and stranding it on "thinking…" forever. */
  abandonVoiceTurn: () => void
  /** Registers the one live handler for binary reply frames and the
   * completion/interrupt signals around them - see VoiceReplyHandlers.
   * Mic.tsx is the only caller; registering a new set of handlers replaces
   * whatever was registered before. Returns an unregister function. */
  onVoiceReply: (handlers: VoiceReplyHandlers) => () => void
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
  // The one live registrant from onVoiceReply() - see VoiceReplyHandlers.
  const voiceHandlersRef = useRef<VoiceReplyHandlers | null>(null)
  // The last `prev` array cancelCurrentTurn has already accounted for -
  // see that function's comment for what this guards against.
  const lastCancelCheckRef = useRef<Turn[] | null>(null)

  /** The single place a turn gets cancelled locally, from every caller that
   * can preempt one: ask(), interrupt(), startVoiceTurn(). Marks the last
   * turn done immediately rather than waiting on the server, and tracks how
   * many stray "done" frames are still owed for it - see the giant comment
   * on the `case 'done':` branch below for what that count is and why it
   * exists at all.
   *
   * Must be called only from inside a setTurns() updater, and only ever
   * with the `prev` that updater receives - never a value read from the
   * `turns` closure. That used to be exactly backwards (Task 3's carried-
   * forward defect): hasCancellableTurn() and the pendingCancelsRef bump
   * read `turns` from the enclosing closure, which is fine for a single
   * call but wrong the moment two real calls (say, two rapid mic presses,
   * both landing before React has re-rendered) land in the same
   * synchronous tick - both would read the *same* stale `turns`, so both
   * would decide a cancellable turn exists and both would bump the ref,
   * even when the server itself only sends one stray "done" for the pair
   * (Session.on_cancel clears self._reply on the first cancel, so a second
   * one back-to-back is a no-op there). The extra count then ate the
   * *next* turn's genuine "done", leaving it stuck on "thinking..." forever
   * - see useTurn.test.tsx's test for two calls in one tick.
   *
   * Deriving everything from `prev` fixes that: React threads a second
   * queued updater's `prev` from the first one's return value, so a second
   * *real* call in the same tick always receives an already-updated `prev`
   * (reflecting the first call's cancellation) and correctly finds nothing
   * left to cancel. The one wrinkle is React 19 StrictMode, which
   * deliberately invokes a functional updater twice with the *identical*
   * `prev` reference to catch impure updaters - without a guard, that
   * would double-count a single real call all over again (this was Finding
   * 1, from Task 3's own review). lastCancelCheckRef closes that: it is
   * pure identity - "have I already processed exactly this prev" - so
   * StrictMode's second invocation with the same reference is a no-op, but
   * a second call's genuinely different (already-updated) `prev` is not. */
  const cancelCurrentTurn = useCallback((prev: Turn[]): Turn[] => {
    if (prev.length === 0 || prev[prev.length - 1].done) return prev
    if (lastCancelCheckRef.current !== prev) {
      lastCancelCheckRef.current = prev
      pendingCancelsRef.current += 1
      // A new question, typed or spoken, always means whatever was still
      // sounding from the last one should stop now, not fade out - the
      // same thing a mic press interrupting a reply needs. Routed through
      // here rather than called separately by each of ask()/interrupt()/
      // startVoiceTurn() so it shares this function's exactly-once
      // guarantee instead of needing its own.
      voiceHandlersRef.current?.onInterrupt()
    }
    return updateLastTurn(prev, (turn) => ({ ...turn, done: true }))
  }, [])

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
      // Plain Blob otherwise - audio.ts's Player and createResampler() both
      // work on typed arrays over ArrayBuffer, and there is no other
      // consumer of a binary frame.
      ws.binaryType = 'arraybuffer'
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
          // Binary frames are TTS audio for a spoken reply (see
          // server/session.py's `render`) - forwarded straight to whoever is
          // playing it back, bypassing React state on purpose: a PCM frame
          // can arrive many times a second, and a setTurns() call per frame
          // would be a re-render per frame for something no component
          // actually needs to render. A typed question never produces one
          // of these (speak=False there), so there is exactly one
          // registrant to forward to - see onVoiceReply below. Before Mic
          // ever mounts (or if voice is never used) there is nothing
          // registered, and dropping the frame here is not a bug.
          voiceHandlersRef.current?.onChunk(event.data as ArrayBuffer)
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
            // Outside the updater, not inside it: a function passed to
            // setTurns() can be invoked twice by StrictMode (see
            // cancelCurrentTurn's comment), and this must fire exactly
            // once per real "done". Harmless when nothing is registered,
            // and harmless for a typed turn's own done (no chunks were
            // ever pushed, so there is nothing to finish).
            voiceHandlersRef.current?.onDone()
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
      const id = nextIdRef.current++
      setTurns((prev) => [
        ...cancelCurrentTurn(prev),
        { id, question: text, spoken: false, sentences: [], emotion: null, trace: null, done: false },
      ])
      socket.send(JSON.stringify({ type: 'text', value: text }))
      return true
    },
    [cancelCurrentTurn],
  )

  const interrupt = useCallback(() => {
    setTurns((prev) => cancelCurrentTurn(prev))
    socketRef.current?.send(JSON.stringify({ type: 'cancel' }))
  }, [cancelCurrentTurn])

  const startVoiceTurn = useCallback((): boolean => {
    const socket = socketRef.current
    if (!socket || socket.readyState !== WebSocket.OPEN) {
      // Same contract as ask() above, and for the same reason - see its
      // comment.
      setSendError('Not connected - recording was not started. Try again once reconnected.')
      return false
    }

    setSendError(null)
    const id = nextIdRef.current++
    setTurns((prev) => [
      ...cancelCurrentTurn(prev),
      { id, question: '(spoken)', spoken: true, sentences: [], emotion: null, trace: null, done: false },
    ])
    // The server treats a "start" arriving while it is THINKING or SPEAKING
    // as an interruption on its own (session.py's on_start calls
    // on_cancel) - the same thing the device's own button does. So this one
    // message is both "interrupt whatever is playing" and "start
    // listening"; no separate cancel frame is needed, and cancelCurrentTurn
    // above already did the matching local bookkeeping for it.
    socket.send(JSON.stringify({ type: 'start' }))
    return true
  }, [cancelCurrentTurn])

  const sendVoiceChunk = useCallback((chunk: ArrayBuffer) => {
    const socket = socketRef.current
    if (socket && socket.readyState === WebSocket.OPEN) {
      socket.send(chunk)
    }
    // Silently dropped otherwise - Mic.tsx's capture is already tearing
    // down by the time a caller could see this, same as ask()'s composer
    // being one render behind the socket, except there is no separate
    // error to show for a single dropped chunk mid-recording.
  }, [])

  const endVoiceTurn = useCallback(() => {
    const socket = socketRef.current
    if (socket && socket.readyState === WebSocket.OPEN) {
      socket.send(JSON.stringify({ type: 'end' }))
    }
  }, [])

  const abandonVoiceTurn = useCallback(() => {
    // Finalises the turn locally, exactly like cancelCurrentTurn's own
    // `{ ...turn, done: true }` - but with no pendingCancelsRef bump and no
    // onInterrupt() call, since nothing was ever sent for this turn beyond
    // "start" and nothing is currently sounding to interrupt. Guarding on
    // `turn.done` (rather than assuming the last turn is always the one to
    // abandon) keeps a stray or repeated call harmless, the same tolerance
    // cancelCurrentTurn has for a `prev` whose last turn is already done.
    setTurns((prev) => updateLastTurn(prev, (turn) => (turn.done ? turn : { ...turn, done: true })))
  }, [])

  const onVoiceReply = useCallback((handlers: VoiceReplyHandlers): (() => void) => {
    voiceHandlersRef.current = handlers
    return () => {
      // Only clears it if this registration is still the live one - a
      // stale unregister running after something else already replaced it
      // (StrictMode's mount/cleanup/mount, in particular) must not clobber
      // the new registration.
      if (voiceHandlersRef.current === handlers) voiceHandlersRef.current = null
    }
  }, [])

  return useMemo(
    () => ({
      connection,
      state,
      turns,
      sendError,
      ask,
      interrupt,
      startVoiceTurn,
      sendVoiceChunk,
      endVoiceTurn,
      abandonVoiceTurn,
      onVoiceReply,
    }),
    [
      connection,
      state,
      turns,
      sendError,
      ask,
      interrupt,
      startVoiceTurn,
      sendVoiceChunk,
      endVoiceTurn,
      abandonVoiceTurn,
      onVoiceReply,
    ],
  )
}
