import type { Turn } from './useTurn.ts'
import Inspector from './Inspector.tsx'

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
 * There is deliberately no speaker affordance here. `state` turns
 * "speaking" for a typed question too, but nothing is actually playing -
 * that only becomes true once Task 7 adds voice - so a play button here
 * would do nothing and just be a lie.
 *
 * The inspector renders once the turn is done, whether it was answered or
 * interrupted - see Inspector.tsx's comment on why `trace: null` covers
 * both an unauthenticated (esp32) connection and an interrupted turn with
 * the same line. While a turn is still in flight there is nothing to show
 * yet: the trace frame is the last thing the server sends for it. */
function Message({ turn, inspectorOpen, onToggleInspector }: MessageProps) {
  const roleName = turn.trace?.role ?? 'assistant'

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
        ) : (
          <p className="message-thinking">{turn.done ? 'interrupted' : 'thinking…'}</p>
        )}
      </div>

      {turn.done && (
        <Inspector trace={turn.trace} open={inspectorOpen} onToggle={onToggleInspector} />
      )}
    </div>
  )
}

export default Message
