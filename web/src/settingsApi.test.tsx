// Narrow on purpose, same reasoning as useTurn.test.tsx: this is logic a
// browser click-through cannot falsify (a PUT body is invisible in the UI),
// not a test of RoleEditor's form itself. The vitest harness only picks up
// `*.test.tsx` (see vitest.config.ts); this file has no JSX because none is
// needed, but the extension has to match to run at all.
import { describe, expect, it } from 'vitest'
import { buildRolePatch } from './settingsApi'
import type { RoleDraft } from './settingsApi'

function draft(overrides: Partial<RoleDraft> = {}): RoleDraft {
  return {
    name: 'Coach',
    prompt: 'Be encouraging.',
    max_sentences: 3,
    markdown_allowed: false,
    languages: ['uk', 'en'],
    pinned_mood: 'excited',
    ...overrides,
  }
}

describe('buildRolePatch', () => {
  it('omits every field left untouched', () => {
    const original = draft()
    const same = draft()
    expect(buildRolePatch(original, same)).toEqual({})
  })

  it('sends only the field that changed, not the rest of the draft', () => {
    const original = draft()
    const edited = draft({ max_sentences: 5 })
    expect(buildRolePatch(original, edited)).toEqual({ max_sentences: 5 })
  })

  it('sends an explicit prompt: null for "Use the built-in wording", distinct from never touching it', () => {
    const original = draft({ prompt: 'Custom wording.' })

    // Untouched: the field the person never opened is left out entirely,
    // not sent back as the same string.
    expect(buildRolePatch(original, draft({ prompt: 'Custom wording.' }))).toEqual({})

    // Cleared: the revert button sets the draft's prompt to null - a real
    // instruction to fall back to the built-in wording, not an omission.
    const reverted = draft({ prompt: null })
    const patch = buildRolePatch(original, reverted)
    expect(Object.keys(patch)).toEqual(['prompt'])
    expect(patch.prompt).toBeNull()
  })

  it('treats an already-null prompt as untouched when it is still null', () => {
    const original = draft({ prompt: null })
    const same = draft({ prompt: null })
    expect(buildRolePatch(original, same)).toEqual({})
  })

  it('ignores language order when nothing actually changed', () => {
    const original = draft({ languages: ['uk', 'en'] })
    const reordered = draft({ languages: ['en', 'uk'] })
    expect(buildRolePatch(original, reordered)).toEqual({})
  })

  it('includes pinned_mood: null when unpinning, and omits it when it was never pinned', () => {
    const wasPinned = draft({ pinned_mood: 'happy' })
    expect(buildRolePatch(wasPinned, draft({ pinned_mood: null }))).toEqual({ pinned_mood: null })

    const neverPinned = draft({ pinned_mood: null })
    expect(buildRolePatch(neverPinned, draft({ pinned_mood: null }))).toEqual({})
  })
})
