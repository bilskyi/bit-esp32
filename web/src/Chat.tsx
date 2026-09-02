import { useEffect, useRef, useState } from 'react'
import type { FormEvent } from 'react'
import { useSession } from './api'
import type { Section } from './Shell.tsx'
import type { Trace, Turn, TurnValue } from './useTurn.ts'
import Message from './Message.tsx'
import Mic from './Mic.tsx'
import Face from './Face.tsx'

interface ChatProps {
  turn: TurnValue
  /** Lets the header's "Історія" button and the side panel's "Робочий
   * стіл" button actually switch section, the same callback Shell.tsx
   * already drives its own nav from - see App.tsx, which owns the one
   * `Section` state and passes this straight through. */
  onNavigate: (section: Section) => void
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

/** The most recent trace any turn actually got, searching back the same way
 * as lastEmotion - used only for the page header's "persona X · поверхня Y"
 * line, which has nothing real to say before the first trace ever lands. */
function lastTrace(turns: Turn[]): Trace | null {
  for (let i = turns.length - 1; i >= 0; i--) {
    if (turns[i].trace) return turns[i].trace
  }
  return null
}

/** Розмова: the conversation and its inspector, restyled onto the mockup's
 * own Talk section markup (scratchpad/designs/e1-precision.html) and the
 * classes app.css already carries for it. Owns none of the socket state
 * itself - `turn` is the one `useTurn()` instance App.tsx keeps alive for
 * the whole signed-in session, so leaving this section and coming back
 * never drops the connection or the transcript. */
function Chat({ turn, onNavigate }: ChatProps) {
  const { username } = useSession()
  const { connection, state, turns, ask, sendError } = turn
  const [draft, setDraft] = useState('')
  // Which turn's inspector is open, by turn id - a single value rather than
  // per-message state, so opening one always closes any other.
  const [openTraceId, setOpenTraceId] = useState<number | null>(null)

  const threadRef = useRef<HTMLDivElement>(null)
  // Whether to follow new content to the bottom. Read and written from a
  // plain ref rather than state: it changes on every scroll event and must
  // never itself trigger a render.
  const stickToBottomRef = useRef(true)

  const disabled = connection.status !== 'open'
  const trace = lastTrace(turns)

  const handleScroll = () => {
    const el = threadRef.current
    if (!el) return
    const distanceFromBottom = el.scrollHeight - el.scrollTop - el.clientHeight
    stickToBottomRef.current = distanceFromBottom <= BOTTOM_SLACK_PX
  }

  // Follows the transcript to the bottom on every change - a new turn, a
  // sentence landing, a trace attaching - but only when the person was
  // already there. Yanking the view while they are reading something
  // higher up (an open inspector) is worse than a missed scroll.
  useEffect(() => {
    const el = threadRef.current
    if (!el || !stickToBottomRef.current) return
    el.scrollTop = el.scrollHeight
  }, [turns])

  const submit = (event: FormEvent) => {
    event.preventDefault()
    const text = draft.trim()
    if (!text || disabled) return
    // ask() can still refuse here even though the composer wasn't disabled -
    // see its comment in useTurn.ts. Leave the draft in place when it does,
    // so the question is not lost and a retry is just pressing send again.
    if (!ask(text)) return
    setDraft('')
    stickToBottomRef.current = true
  }

  // Real turn/token counts for the side panel - both derived straight from
  // what useTurn.ts already carries, never a separate guess. "репліки"
  // counts message blocks the way the thread itself renders them: one for
  // the question, one for the reply, per turn.
  const replyCount = turns.length * 2
  const tokenTotal = turns.reduce(
    (sum, t) => sum + (t.trace ? t.trace.prompt_tokens + t.trace.completion_tokens : 0),
    0,
  )

  return (
    <section className="view">
      <div className="wrap">
        <div className="head">
          <div className="hl">
            <p className="lab">
              {trace ? `Розмова · persona ${trace.role} · поверхня ${trace.surface}` : 'Розмова'}
            </p>
            <h1 style={{ fontSize: 18, marginTop: 2 }}>
              {turns.length === 0 ? 'Постав перше питання.' : 'Ми на середині розмови.'}
            </h1>
            <p className="prose" style={{ fontSize: 13 }}>
              Одна памʼять на всі поверхні. Те, що ти спитав тут, я знаю й на столі.
            </p>
          </div>
          <div className="acts">
            <button type="button" className="btn" onClick={() => onNavigate('act')}>Історія</button>
          </div>
        </div>

        <div className="grid2 wide-side">
          <div className="chat">
            <div className="thread thread-scroll" ref={threadRef} onScroll={handleScroll}>
              {turns.length === 0 ? (
                <div className="turn">
                  <p className="prose" style={{ fontSize: 13 }}>
                    Напиши питання нижче або натисни мікрофон — памʼять одна на всі поверхні.
                  </p>
                </div>
              ) : (
                turns.map((t) => (
                  <Message
                    key={t.id}
                    turn={t}
                    userName={username ?? 'Ти'}
                    inspectorOpen={openTraceId === t.id}
                    onToggleInspector={() => setOpenTraceId((cur) => (cur === t.id ? null : t.id))}
                  />
                ))
              )}
            </div>

            {disabled ? (
              <p className="chat-status">{connection.reason}</p>
            ) : (
              // Only shown while the composer looks enabled - once
              // `disabled` flips true (which a refused send is usually the
              // leading edge of) connection.reason above already explains
              // why, and showing both would just be noise.
              sendError && <p className="chat-status">{sendError}</p>
            )}

            <form className="ask composer chat-composer" onSubmit={submit}>
              <span className="pre" aria-hidden="true">&gt;</span>
              <input
                type="text"
                className="chat-input"
                value={draft}
                onChange={(event) => setDraft(event.target.value)}
                placeholder="Напиши або натисни мікрофон…"
                aria-label="Написати асистенту"
                disabled={disabled}
              />
              <Mic turn={turn} />
              <button type="submit" className="btn pri" disabled={disabled || !draft.trim()}>Надіслати</button>
            </form>
          </div>

          <div className="stack">
            <div className="grp">
              <div className="grph">
                <p className="lab">Ця розмова</p>
                <span className="cnt">записується</span>
              </div>
              <div className="sidelist">
                <div><b>{replyCount}</b><span>репліки</span></div>
                <div><b>{tokenTotal}</b><span>токенів</span></div>
              </div>
              <p className="prose" style={{ fontSize: 12, marginTop: 8 }}>
                Факти, які я з неї витягну, переживуть саму розмову.
              </p>
            </div>

            <div className="grp" style={{ marginTop: 20 }}>
              <div className="grph"><p className="lab">Голос</p></div>
              <p className="prose" style={{ fontSize: 12 }}>
                На столі те саме питання можна просто сказати. Кнопка на пристрої — і я чую.
              </p>
              <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginTop: 8 }}>
                <Face state={state} emotion={lastEmotion(turns)} online={!disabled} />
                <button type="button" className="btn xs" onClick={() => onNavigate('dev')}>Робочий стіл</button>
              </div>
            </div>
          </div>
        </div>
      </div>
    </section>
  )
}

export default Chat
