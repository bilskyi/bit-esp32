// Narrow on purpose, same reasoning as faceFrames.test.tsx: this covers only
// the pure classification in traceStatus.ts, not the JSX around it in
// Message.tsx/Inspector.tsx - see that skill/task's own instruction not to
// add component tests here. This file has no JSX because none is needed,
// but the extension has to match `*.test.tsx` (see vitest.config.ts) to run
// at all.
//
// This logic has broken twice now, both times because it lived inline with
// no test of its own:
//
// 1. Before Task 7, an interrupted *spoken* turn (no trace, no sentences -
//    normal, since a voice reply never gets `reply` frames either way) was
//    told to sign in instead of told it was interrupted, even when already
//    signed in - see commit d020a48.
// 2. Task 7's own fix regressed the other direction: `turn.done &&
//    turn.trace === null`, evaluated only where `turn.done` is already
//    guaranteed, reduces to `trace === null` - true for *any* trace-less
//    turn, sentences or not. On a local dev server with no DEVICE_TOKEN set
//    (auth_required defaults to false there), an anonymous connection is
//    labelled esp32 and never gets a trace at all - so a typed question
//    there, answered normally with sentences, was wrongly told it had been
//    interrupted.
//
// Every combination below is here so a third regression has to break an
// assertion, not just eyeball an inspector panel.
import { describe, expect, it } from 'vitest'
import { traceMissingReason } from './traceStatus.ts'
import type { Trace } from './useTurn.ts'

const SOME_TRACE: Trace = {
  role: 'assistant',
  surface: 'web',
  facts: [],
  prompt: 'prompt',
  prompt_tokens: 10,
  completion_tokens: 5,
  emotion: null,
  spoken: false,
  stt_ms: 0,
  reply_ms: 100,
}

describe('traceMissingReason', () => {
  it('typed + completed: has a trace and sentences, nothing missing to explain', () => {
    expect(traceMissingReason({ trace: SOME_TRACE, sentences: ['Hello.'] })).toBeNull()
  })

  it('typed + interrupted: no trace, no sentences yet - cut off before any reply text arrived', () => {
    expect(traceMissingReason({ trace: null, sentences: [] })).toBe('interrupted')
  })

  it('spoken + completed: has a trace but no sentences - normal, the answer went out as audio', () => {
    expect(traceMissingReason({ trace: { ...SOME_TRACE, spoken: true }, sentences: [] })).toBeNull()
  })

  it('spoken + interrupted: no trace and no sentences - cut off before it could speak', () => {
    expect(traceMissingReason({ trace: null, sentences: [] })).toBe('interrupted')
  })

  it('regression: an anonymous typed turn that answered normally is not mislabelled interrupted', () => {
    // The exact Finding 3 scenario: no DEVICE_TOKEN set, so the connection
    // never gets a trace at all - but the question was answered in full.
    expect(traceMissingReason({ trace: null, sentences: ['A normal, complete answer.'] })).toBe('no-session')
  })
})
