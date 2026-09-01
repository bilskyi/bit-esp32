import { useEffect, useRef, useState } from 'react'
import type { FormEvent, KeyboardEvent } from 'react'
import type { Turn, TurnValue } from './useTurn.ts'
import Face from './Face.tsx'
import Message from './Message.tsx'
import Mic from './Mic.tsx'

interface ChatProps {
  turn: TurnValue
}

/** How close to the bottom still counts as "at the bottom". A few pixels of
 * slack for the sub-pixel rounding scrollHeight arithmetic is prone to. */
const BOTTOM_SLACK_PX = 24

/** The most recent emotion any turn actually got tagged with, searching back
 * from the newest turn. Not just the last turn's own `emotion`: a fresh
 * question starts with `emotion: null` (see useTurn.ts's ask()) until its
 * own `emotion` frame lands, and the face should keep the previous mood
 * through listening/thinking rather than snapping to neutral the instant a
 * new turn starts. */
function lastEmotion(turns: Turn[]): string | null {
  for (let i = turns.length - 1; i >= 0; i--) {
    const emotion = turns[i].emotion
    if (emotion) return emotion
  }
  return null
}

/** The Chat tab: a scrolling transcript with a composer pinned under it.
 * Owns none of the socket state itself - `turn` is the one `useTurn()`
 * instance App.tsx keeps alive for the whole signed-in session, so leaving
 * this tab and coming back never drops the connection or the transcript. */
function Chat({ turn }: ChatProps) {
  const { connection, state, turns, ask, sendError } = turn
  const [draft, setDraft] = useState('')
  // Which turn's inspector is open, by turn id - a single value rather than
  // per-message state, so opening one always closes any other (Task 4's
  // requirement). null means none open.
  const [openTraceId, setOpenTraceId] = useState<number | null>(null)

  const scrollRef = useRef<HTMLDivElement>(null)
  const textareaRef = useRef<HTMLTextAreaElement>(null)
  // Whether to follow new content to the bottom. Read and written from plain
  // refs rather than state: it changes on every scroll event and must never
  // itself trigger a render.
  const stickToBottomRef = useRef(true)

  const disabled = connection.status !== 'open'

  const handleScroll = () => {
    const el = scrollRef.current
    if (!el) return
    const distanceFromBottom = el.scrollHeight - el.scrollTop - el.clientHeight
    stickToBottomRef.current = distanceFromBottom <= BOTTOM_SLACK_PX
  }

  // Follows the transcript to the bottom on every change - a new turn, a
  // sentence landing, a trace attaching - but only when the person was
  // already there. Yanking the view while they are reading something
  // higher up (an inspector panel, from Task 4 onward) is worse than a
  // missed scroll.
  useEffect(() => {
    const el = scrollRef.current
    if (!el || !stickToBottomRef.current) return
    el.scrollTop = el.scrollHeight
  }, [turns])

  // Grows the textarea to fit what's typed, up to the CSS max-height where
  // it starts scrolling internally instead.
  useEffect(() => {
    const el = textareaRef.current
    if (!el) return
    el.style.height = 'auto'
    el.style.height = `${el.scrollHeight}px`
  }, [draft])

  const submit = () => {
    const text = draft.trim()
    if (!text || disabled) return
    // ask() can still refuse here even though the composer wasn't disabled -
    // see its comment in useTurn.ts. Leave the draft in place when it does,
    // so the question is not lost and a retry is just pressing send again.
    if (!ask(text)) return
    setDraft('')
    stickToBottomRef.current = true
  }

  const handleSubmit = (event: FormEvent) => {
    event.preventDefault()
    submit()
  }

  const handleKeyDown = (event: KeyboardEvent<HTMLTextAreaElement>) => {
    if (event.key === 'Enter' && !event.shiftKey) {
      event.preventDefault()
      submit()
    }
  }

  return (
    <div className="chat-layout">
      {/* Above the transcript on mobile (plain DOM order); a sticky right
          rail beside it from 900px - see app.css. Task 6's own element: the
          device's face, driven by the same `turn` this tab already has. */}
      <aside className="face-rail">
        <Face state={state} emotion={lastEmotion(turns)} online={!disabled} />
      </aside>

      <div className="chat">
        <div className="chat-scroll" ref={scrollRef} onScroll={handleScroll}>
          {turns.length === 0 ? (
            <p className="chat-empty">Ask it something. Ukrainian, Russian or English.</p>
          ) : (
            turns.map((t) => (
              <Message
                key={t.id}
                turn={t}
                inspectorOpen={openTraceId === t.id}
                onToggleInspector={() => setOpenTraceId((cur) => (cur === t.id ? null : t.id))}
              />
            ))
          )}
        </div>

        {disabled ? (
          <p className="chat-status">{connection.reason}</p>
        ) : (
          // Only shown while the composer looks enabled - once `disabled`
          // flips true (which a refused send is usually the leading edge of)
          // connection.reason above already explains why, and showing both
          // would just be noise.
          sendError && <p className="chat-status">{sendError}</p>
        )}

        <form className="chat-composer" onSubmit={handleSubmit}>
          <Mic turn={turn} />
          <textarea
            ref={textareaRef}
            className="chat-input"
            value={draft}
            onChange={(event) => setDraft(event.target.value)}
            onKeyDown={handleKeyDown}
            placeholder="Ask a question…"
            disabled={disabled}
            rows={1}
          />
          <button type="submit" disabled={disabled || !draft.trim()}>
            Send
          </button>
        </form>
      </div>
    </div>
  )
}

export default Chat
