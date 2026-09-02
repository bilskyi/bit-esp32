import { Fragment, useEffect, useId, useRef } from 'react'
import type { Trace } from './useTurn.ts'
import { traceMissingReason } from './traceStatus.ts'

interface InspectorProps {
  /** null has two real causes and they need different sentences - see
   * traceStatus.ts's traceMissingReason, which this component calls with
   * `trace` and `sentences` together to tell them apart: an esp32-labelled
   * connection (no session cookie, so session.py never calls _trace at all)
   * versus a turn cancelled before the reply - and therefore before the
   * trace send - ever ran. */
  trace: Trace | null
  /** Needed only to classify a missing trace - see traceMissingReason.
   * Never rendered directly here; Message.tsx already renders these as the
   * reply body when there are any. */
  sentences: string[]
  /** Whether this turn started from typed text or the device's mic. Needed
   * only to phrase the "recognition didn't run" note honestly: `stt_ms` is
   * 0 for every typed question (there is no STT stage for one), and that
   * note is only true when that is *why* it is 0 - not merely whenever it
   * happens to be. */
  spoken: boolean
  open: boolean
  onToggle: () => void
}

/** Formats a millisecond reading as a plain unit-labelled figure. Called
 * only where the value has already been checked non-zero - see the stt_ms
 * comments below - so it never has to decide whether to print at all. */
function ms(value: number): string {
  return `${Math.round(value)} ms`
}

/** Ukrainian has three plural forms where English has two. Only "речення"
 * (sentence) ever needs this here - every other figure in this file keeps
 * the mockup's own bilingual technical shorthand ("tok in", "facts") rather
 * than translating it, per the brief: take the mockup's copy as given. */
function sentenceWord(n: number): string {
  const mod100 = n % 100
  const mod10 = n % 10
  if (mod100 >= 11 && mod100 <= 14) return 'речень'
  if (mod10 === 1) return 'речення'
  if (mod10 >= 2 && mod10 <= 4) return 'речення'
  return 'речень'
}

function ChevronIcon() {
  return (
    <svg viewBox="0 0 24 24" width="11" height="11" fill="none" stroke="currentColor" strokeWidth="2" aria-hidden="true">
      <path d="M6 9l6 6 6-6" />
    </svg>
  )
}

/** The inspector: what server/session.py's `_trace` frame actually said
 * about a turn, collapsed under a one-line readout by default. Renders for
 * every turn that has finished (Message.tsx gates on `turn.done`), whether
 * or not a trace ever arrived - the two are different states and both get
 * words, never a blank space. Markup and classes are the mockup's own
 * (.tfoot/.why/.insp/.insp-h/.insp-b/.sublab/.ret/.rrow/.promptbox/.kv) -
 * see scratchpad/designs/e1-precision.html's Talk section.
 *
 * `open`/`onToggle` are controlled from Chat.tsx rather than owned here,
 * because "exactly one inspector open at a time" is a fact about the whole
 * turn list, not about any single message. */
function Inspector({ trace, sentences, spoken, open, onToggle }: InspectorProps) {
  const panelId = useId()
  const panelRef = useRef<HTMLDivElement>(null)

  // .thread-scroll (app.css) caps the transcript to a fixed height so a
  // long conversation scrolls inside its own box rather than the whole
  // page - but that means opening a panel this tall can push most of it
  // below whatever was already visible, with nothing to bring it back into
  // view on its own. `block: 'nearest'` scrolls only that ancestor, and
  // only as far as it has to - never yanking the page if the panel already
  // fits.
  useEffect(() => {
    if (open) panelRef.current?.scrollIntoView({ block: 'nearest', behavior: 'smooth' })
  }, [open])

  if (!trace) {
    // Two different reasons a turn has no trace, and the wrong explanation is
    // worse than none. The server runs _trace() before every natural `done`
    // on a cookie-authorised connection, so for a signed-in person a missing
    // trace means they cancelled - telling them to sign in would be simply
    // untrue. A turn cut off after some sentences had already streamed is
    // still ambiguous from here and falls through to the second line - see
    // traceMissingReason's own comment on that boundary.
    const reason = traceMissingReason({ trace, sentences })
    return (
      <p className="dim" style={{ fontSize: 11, marginTop: 6 }}>
        {reason === 'interrupted'
          ? 'Перервано до завершення репліки — трасування не встигло записатися.'
          : 'Трасування немає — це зʼєднання без сесійного cookie (наприклад, пристрій).'}
      </p>
    )
  }

  // stt_ms is 0 for every typed question - there is no STT stage - and a
  // "0 ms" reading would read as a measurement rather than as "not
  // applicable". The explanatory line under the details grid only appears
  // when that absence is genuinely because the question was typed - see
  // the `spoken` prop's own comment.
  const sttRan = trace.stt_ms > 0
  const sentenceLabel = `${sentences.length} ${sentenceWord(sentences.length)}`

  // The compact readout beside "чому саме ця відповідь?" - emotion and
  // reply time are already shown as tags on the turn itself (Message.tsx),
  // so this line covers what isn't: how much of the reply this is, and what
  // it cost. Sorted the same way the retrieval table below is, for the same
  // reason: best-first only where "best" is well-defined (score), plain
  // arrival order for facts count and tokens which have none.
  const figures = `${sentenceLabel} · ${trace.prompt_tokens} tok in · ${trace.completion_tokens} tok out · ${trace.facts.length} facts`

  // Best-first: server/session.py's trace.facts is not documented to arrive
  // sorted, so this is done here rather than assumed.
  const sortedFacts = [...trace.facts].sort((a, b) => b.score - a.score)
  const wordCount = trace.prompt.trim().length === 0 ? 0 : trace.prompt.trim().split(/\s+/).length

  return (
    <Fragment>
      <div className="tfoot">
        <button type="button" className="why" aria-expanded={open} aria-controls={panelId} onClick={onToggle}>
          <ChevronIcon />
          чому саме ця відповідь?
        </button>
        <span className="dim" style={{ fontSize: 11 }}>{figures}</span>
      </div>

      <div ref={panelRef} id={panelId} className="insp" hidden={!open}>
        <div className="insp-h">
          <span className="s">
            emotion <b>{trace.emotion ?? 'none'}</b>
            {sttRan && <> · {ms(trace.stt_ms)} stt</>}
            {' · '}{ms(trace.reply_ms)} · {trace.prompt_tokens} tok in · {trace.facts.length} facts
          </span>
          <button type="button" className="btn xs ghost x" onClick={onToggle}>закрити</button>
        </div>

        <div className="insp-b">
          <p className="sublab">
            <span>Що я дістала з памʼяті · найкраще спершу</span>
            <span className="r">score · текст</span>
          </p>
          {sortedFacts.length === 0 ? (
            <p className="dim" style={{ fontSize: 12 }}>Нічого не знайшла для цього питання.</p>
          ) : (
            <div className="ret">
              {sortedFacts.map((fact, index) => (
                // Index as key is safe: this list is built once when the
                // trace frame lands and never reordered or mutated after.
                <div key={index} className={index === 0 ? 'rrow' : 'rrow low'}>
                  <span className="sc">{fact.score.toFixed(4)}</span>
                  <span className="bar">
                    <i
                      // Clamped on purpose: cosine similarity runs to -1,
                      // and a negative score would otherwise render as a
                      // negative width.
                      style={{ width: `${Math.max(0, Math.min(1, fact.score)) * 100}%` }}
                    />
                  </span>
                  {/* The mockup's own example also shows a row sourced from
                      a document, tagged "документ". Nothing ingests
                      documents yet, and trace.facts here carries only
                      text+score - no source type - so no tag is rendered.
                      This is the one place a document-source column would
                      go once that changes. */}
                  <span className="t">{fact.text}</span>
                </div>
              ))}
            </div>
          )}

          <p className="sublab" style={{ marginTop: 12 }}>
            <span>Системний промпт, як він пішов у модель</span>
          </p>
          <div className="promptbox">
            <pre>{trace.prompt}</pre>
            <div className="pf">
              <span>{wordCount} слів у промпті</span>
              <span className="r">{trace.prompt_tokens} tok in</span>
            </div>
          </div>

          <div className="kv bare" style={{ marginTop: 12, gridTemplateColumns: 'repeat(auto-fit, minmax(168px, 1fr))' }}>
            <div><span className="k">persona</span><span className="v">{trace.role}</span></div>
            <div><span className="k">поверхня</span><span className="v">{trace.surface}</span></div>
            <div><span className="k">озвучено</span><span className={trace.spoken ? 'v' : 'v no'}>{trace.spoken ? 'так' : 'ні'}</span></div>
            <div><span className="k">tok out</span><span className="v acc">{trace.completion_tokens}</span></div>
            <div><span className="k">розпізнавання</span><span className={sttRan ? 'v' : 'v no'}>{sttRan ? ms(trace.stt_ms) : 'не запускалось'}</span></div>
          </div>
          {!spoken && !sttRan && (
            <p className="prose" style={{ fontSize: 12, marginTop: 8 }}>
              Питання було набране, тому мова в текст не переводилась — тут немає нуля мілісекунд, тут немає кроку.
            </p>
          )}
        </div>
      </div>
    </Fragment>
  )
}

export default Inspector
