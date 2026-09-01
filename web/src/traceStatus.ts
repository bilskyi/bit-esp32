// Why a finished turn has no trace - pulled out of Message.tsx/Inspector.tsx
// into its own module because this exact bit of logic broke twice while it
// lived inline with no test of its own (see traceStatus.test.tsx's header
// for the second break). Narrow on purpose, same reasoning as faceFrames.ts:
// this is the pure classification both components render around, not the
// JSX itself.
import type { Trace } from './useTurn.ts'

export type TraceMissingReason = 'interrupted' | 'no-session' | null

/** Only ever meaningful for a turn that has already finished (`turn.done`)
 * - both callers only reach this once that is already guaranteed, exactly
 * how the inline expression this replaces got it wrong the second time: it
 * folded `turn.done` into the comparison even though it was always true by
 * the time it ran, so the whole thing silently reduced to `trace === null`.
 *
 * Two real causes a finished turn can have no trace, and Inspector.tsx reads
 * a different sentence for each:
 *
 * - 'interrupted': cancelled before server/session.py's _trace() ever ran -
 *   it sends right before every natural "done" (see its own comment), so a
 *   trace-less, sentence-less finished turn can only mean it was cut off.
 * - 'no-session': the connection was never cookie-authorised (auth_required
 *   defaults to false with no DEVICE_TOKEN set, so a local dev connection is
 *   labelled esp32 and session.py never calls _trace() at all, for any turn
 *   on it, ever) - a typed question there still answers normally, with
 *   `sentences`, so sentence count is what tells this apart from a real
 *   interruption. A spoken turn's *successful* reply also has no sentences
 *   (see useTurn.ts's Turn.spoken - its answer goes out as audio, never as
 *   `reply` frames), but it always has a trace on a traced connection, so it
 *   never reaches `sentences.length === 0` here for the wrong reason.
 * - null: the turn has a trace - nothing missing to explain. */
export function traceMissingReason(turn: { trace: Trace | null; sentences: string[] }): TraceMissingReason {
  if (turn.trace !== null) return null
  return turn.sentences.length === 0 ? 'interrupted' : 'no-session'
}
