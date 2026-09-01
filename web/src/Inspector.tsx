import { useId } from 'react'
import type { Trace } from './useTurn.ts'

interface InspectorProps {
  /** null has two real causes and they need different sentences. An
   * esp32-labelled connection (no session cookie, so session.py never calls
   * _trace at all) versus a turn cancelled before the reply - and therefore
   * before the trace send - ever ran. `interrupted` is what tells them
   * apart. */
  trace: Trace | null
  /** True when this finished turn produced no sentences, which on a
   * cookie-authorised connection can only mean it was cancelled: the server
   * sends _trace before every natural `done`. */
  interrupted: boolean
  open: boolean
  onToggle: () => void
}

/** Formats a millisecond reading as a plain unit-labelled figure. Called
 * only where the value has already been checked non-zero - see the stt_ms
 * comment below - so it never has to decide whether to print at all. */
function ms(value: number): string {
  return `${Math.round(value)} ms`
}

/** The inspector: what server/session.py's `_trace` frame actually said
 * about a turn, collapsed under a one-line readout by default. Renders
 * for every turn that has finished (Message.tsx gates on `turn.done`),
 * whether or not a trace ever arrived - the two are different states and
 * both get words, never a blank space. See tokens.css and
 * firmware/host/preview-template.html for the design language this follows:
 * mono figures on `--sunk`, no colour invented for this task.
 *
 * `open`/`onToggle` are controlled from Chat.tsx rather than owned here,
 * because "exactly one inspector open at a time" is a fact about the whole
 * turn list, not about any single message. */
function Inspector({ trace, interrupted, open, onToggle }: InspectorProps) {
  const panelId = useId()

  if (!trace) {
    // Two different reasons a turn has no trace, and the wrong explanation is
    // worse than none. The server runs _trace() before every natural `done`
    // on a cookie-authorised connection, so for a signed-in person a missing
    // trace means they cancelled - telling them to sign in would be simply
    // untrue. A turn cut off after some sentences had already streamed is
    // still ambiguous from here and falls through to the second line.
    return (
      <p className="inspector-missing">
        {interrupted
          ? 'Interrupted before this turn finished — no trace was recorded.'
          : 'No trace for this turn. Sign in over a session cookie to see it.'}
      </p>
    )
  }

  // stt_ms is 0 for every typed question - there is no STT stage - and a
  // "0 ms" reading would read as a measurement rather than as "not
  // applicable". Shown only when there is something to show, both here and
  // in the expanded numbers below.
  const figures = [
    `emotion ${trace.emotion ?? 'none'}`,
    ...(trace.stt_ms > 0 ? [`${ms(trace.stt_ms)} stt`] : []),
    `${ms(trace.reply_ms)} reply`,
    `${trace.prompt_tokens} tok in`,
    `${trace.facts.length} ${trace.facts.length === 1 ? 'fact' : 'facts'}`,
  ]

  return (
    <div className="inspector">
      <button
        type="button"
        className="inspector-toggle"
        aria-expanded={open}
        aria-controls={panelId}
        onClick={onToggle}
      >
        <span className="inspector-toggle-label">{open ? 'Hide' : 'Why this answer?'}</span>
        <span className="inspector-figures">{figures.join(' · ')}</span>
      </button>

      <div
        id={panelId}
        className={`inspector-panel${open ? ' is-open' : ''}`}
        aria-hidden={!open}
      >
        <div className="inspector-panel-inner">
          <section className="inspector-section">
            <h3 className="inspector-heading">Retrieved facts</h3>
            {trace.facts.length === 0 ? (
              <p className="inspector-empty">Nothing retrieved for this question.</p>
            ) : (
              <ul className="inspector-facts">
                {trace.facts.map((fact, index) => (
                  // Index as key is safe here: this list is rendered once
                  // when the trace frame lands and never reordered or
                  // mutated afterward.
                  <li key={index} className="inspector-fact">
                    <div className="inspector-fact-row">
                      <span className="inspector-fact-text">{fact.text}</span>
                      <span className="inspector-fact-score">{fact.score.toFixed(4)}</span>
                    </div>
                    <div className="inspector-bar-track">
                      <div
                        className="inspector-bar-fill"
                        // Clamped on purpose: cosine similarity runs to -1,
                        // and a negative score would otherwise render as a
                        // negative width. Such a fact is drawn as an empty
                        // bar and read from the number beside it.
                        style={{ width: `${Math.max(0, Math.min(1, fact.score)) * 100}%` }}
                      />
                    </div>
                  </li>
                ))}
              </ul>
            )}
          </section>

          <section className="inspector-section">
            <h3 className="inspector-heading">Assembled prompt</h3>
            <pre className="inspector-prompt">{trace.prompt}</pre>
          </section>

          <section className="inspector-section">
            <h3 className="inspector-heading">Details</h3>
            <dl className="inspector-details">
              <div className="inspector-detail">
                <dt>role</dt>
                <dd>{trace.role}</dd>
              </div>
              <div className="inspector-detail">
                <dt>surface</dt>
                <dd>{trace.surface}</dd>
              </div>
              <div className="inspector-detail">
                <dt>spoken</dt>
                <dd>{trace.spoken ? 'yes' : 'no'}</dd>
              </div>
              <div className="inspector-detail">
                <dt>completion tokens</dt>
                <dd>{trace.completion_tokens} tok</dd>
              </div>
              {trace.stt_ms > 0 && (
                <div className="inspector-detail">
                  <dt>stt</dt>
                  <dd>{ms(trace.stt_ms)}</dd>
                </div>
              )}
            </dl>
          </section>
        </div>
      </div>
    </div>
  )
}

export default Inspector
