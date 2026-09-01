import type { Turn } from './useTurn.ts'
import Inspector from './Inspector.tsx'
import { traceMissingReason } from './traceStatus.ts'

interface MessageProps {
  turn: Turn
  /** Whether this turn's inspector is the one open turn - see Chat.tsx,
   * which owns this as a single id rather than per-message state so opening
   * one always closes any other. */
  inspectorOpen: boolean
  onToggleInspector: () => void
}

/** One turn, rendered as two blocks - a small mono label above the text,
 * never a bubble. See tokens.css and firmware/host/preview-template.html
 * for the design language this follows.
 *
 * The reply's label carries the active role's name once the trace frame
 * lands; before that (or for a role-less session) it falls back to
 * "assistant" rather than showing nothing. Reply text is the one place
 * that switches to `.prose` - a monospace paragraph of Cyrillic is tiring
 * to read, see tokens.css.
 *
 * There is still no play/pause affordance here for a spoken reply - Mic.tsx
 * owns the one live audio pipeline (see useTurn.ts's onVoiceReply), and it
 * plays automatically as the reply streams in, the same way the device's
 * speaker does. A control here would just be a second, redundant way to
 * control audio this component has no handle on.
 *
 * The inspector renders once the turn is done, whether it was answered or
 * interrupted. server/session.py's _trace() runs right before every natural
 * `done` (see its own comment), so a finished turn with no trace means
 * either it was cancelled, or the connection itself never gets traced at
 * all (no session cookie) - see traceStatus.ts's traceMissingReason, which
 * is what Inspector needs to pick between its two "no trace" sentences.
 * Below, only the 'interrupted' case is ever relevant: `sentences.length >
 * 0` is already handled by the branch above it, so a spoken turn's normal
 * "no sentences, but it has a trace" outcome (its answer goes out as audio
 * instead - see useTurn.ts's `Turn.spoken`) never reaches this ternary at
 * all. While a turn is still in flight there is nothing to show yet - the
 * trace frame is the last thing the server sends for it. */
function Message({ turn, inspectorOpen, onToggleInspector }: MessageProps) {
  const roleName = turn.trace?.role ?? 'assistant'
  const reason = traceMissingReason(turn)

  return (
    <div className="message">
      <div className="message-block">
        <p className="message-label">you</p>
        <p className="message-question">{turn.question}</p>
      </div>

      <div className="message-block">
        <p className="message-label">{roleName}</p>
        {turn.sentences.length > 0 ? (
          <p className="prose message-reply">{turn.sentences.join(' ')}</p>
        ) : !turn.done ? (
          <p className="message-thinking">thinking…</p>
        ) : reason === 'interrupted' ? (
          <p className="message-thinking">interrupted</p>
        ) : (
          // A spoken turn that finished normally: the trace frame arrived,
          // so it was not cancelled, but there was never any `reply` text to
          // show - the answer was heard, not read. Never reachable for a
          // typed turn, which always gets `reply` frames for a real answer.
          <p className="message-thinking">played aloud</p>
        )}
      </div>

      {turn.done && (
        <Inspector
          trace={turn.trace}
          sentences={turn.sentences}
          open={inspectorOpen}
          onToggle={onToggleInspector}
        />
      )}
    </div>
  )
}

export default Message
