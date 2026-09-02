import type { Turn } from './useTurn.ts'
import Inspector from './Inspector.tsx'
import { traceMissingReason } from './traceStatus.ts'

interface MessageProps {
  turn: Turn
  /** The signed-in person's own name, for the question's `.tmeta .nm` label -
   * see api.ts's useSession(), which Chat.tsx already reads for this. */
  userName: string
  /** Whether this turn's inspector is the one open turn - see Chat.tsx,
   * which owns this as a single id rather than per-message state so opening
   * one always closes any other. */
  inspectorOpen: boolean
  onToggleInspector: () => void
}

function ms(value: number): string {
  return `${Math.round(value)} ms`
}

/** One turn, rendered as the mockup's own two `.turn` blocks - `.turn.me`
 * for the question, `.turn.ai` for the reply - never a bubble. See
 * scratchpad/designs/e1-precision.html's Talk section for the markup this
 * follows verbatim, and app.css's "---------- talk ----------" block for
 * the classes it attaches to.
 *
 * The assistant's name in `.tmeta .nm` is always "ЛАА" - the product's own
 * name, used the same way in Shell.tsx's brand mark - never the active
 * role's name. The role (e.g. "Web default") shows instead as a `.tag`
 * once the trace for this turn has landed, the same slot the mockup uses
 * for it, so the two identities (the assistant vs. the persona it is
 * currently running) never collapse into one label.
 *
 * The mockup's example reply also renders a `.laa` span coloring the word
 * "Laaa" at the start of the text - but that word only appears there
 * because *this specific demo persona* has a standing instruction to start
 * every reply with it (see the mockup's Знання section). That is real
 * model output for one persona's own configuration, not a feature of this
 * component, so it is not special-cased here: whatever the model actually
 * returns is what renders, verbatim.
 *
 * There is still no play/pause affordance here for a spoken reply - Mic.tsx
 * owns the one live audio pipeline (see useTurn.ts's onVoiceReply), and it
 * plays automatically as the reply streams in, the same way the device's
 * speaker does.
 *
 * The inspector renders once the turn is done, whether it was answered or
 * interrupted - see Inspector.tsx's own header for why a missing trace
 * still gets a line rather than nothing. */
function Message({ turn, userName, inspectorOpen, onToggleInspector }: MessageProps) {
  const trace = turn.trace
  const reason = traceMissingReason(turn)

  return (
    <>
      <div className="turn me">
        <div className="tmeta">
          <span className="nm">{userName}</span>
          <span className="tag">{turn.spoken ? 'голосом' : 'набрано'}</span>
        </div>
        <p className="msg">{turn.question}</p>
      </div>

      <div className="turn ai">
        <div className="tmeta">
          <span className="nm">ЛАА</span>
          {/* `turn.emotion` can arrive well before `trace` does (see
              useTurn.ts) - showing it as soon as it lands, rather than
              waiting on the trace, is what makes this feel live. */}
          {turn.emotion && <span className="tag">{turn.emotion}</span>}
          {trace && <span className="tag">{ms(trace.reply_ms)}</span>}
          {trace && <span className="tag">{trace.role}</span>}
        </div>

        {turn.sentences.length > 0 ? (
          // One <p> per sentence, in arrival order - the mockup's example
          // separates these with a "streamnote" giving the gap between
          // them, but useTurn.ts never timestamps individual reply frames,
          // so that figure does not exist here to show; omitted rather than
          // guessed.
          turn.sentences.map((sentence, index) => (
            <p key={index} className="msg">{sentence}</p>
          ))
        ) : !turn.done ? (
          <p className="msg dim">думаю…</p>
        ) : reason === 'interrupted' ? (
          <p className="msg dim">перервано</p>
        ) : (
          // A spoken turn that finished normally: the trace frame arrived,
          // so it was not cancelled, but there was never any `reply` text to
          // show - the answer was heard, not read.
          <p className="msg dim">прозвучало голосом</p>
        )}

        {turn.done && (
          <Inspector
            trace={trace}
            sentences={turn.sentences}
            spoken={turn.spoken}
            open={inspectorOpen}
            onToggle={onToggleInspector}
          />
        )}
      </div>
    </>
  )
}

export default Message
