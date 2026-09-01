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
 * There is still no play/pause affordance here for a spoken reply - Mic.tsx
 * owns the one live audio pipeline (see useTurn.ts's onVoiceReply), and it
 * plays automatically as the reply streams in, the same way the device's
 * speaker does. A control here would just be a second, redundant way to
 * control audio this component has no handle on.
 *
 * The inspector renders once the turn is done, whether it was answered or
 * interrupted. server/session.py's _trace() runs right before every natural
 * `done` (see its own comment), so a finished turn with no trace can only
 * mean it was cancelled - that is `interrupted` below, and it is what
 * Inspector needs to pick between its two "no trace" sentences. Sentence
 * count is not that signal: a spoken turn (`turn.spoken`) never gets `reply`
 * frames at all - its answer goes out as audio instead, see useTurn.ts's
 * `Turn.spoken` - so "no sentences" is the normal, successful outcome for
 * one of those, not evidence of anything going wrong. While a turn is still
 * in flight there is nothing to show yet - the trace frame is the last thing
 * the server sends for it. */
function Message({ turn, inspectorOpen, onToggleInspector }: MessageProps) {
  const roleName = turn.trace?.role ?? 'assistant'
  const interrupted = turn.done && turn.trace === null

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
        ) : interrupted ? (
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
          interrupted={interrupted}
          open={inspectorOpen}
          onToggle={onToggleInspector}
        />
      )}
    </div>
  )
}

export default Message
