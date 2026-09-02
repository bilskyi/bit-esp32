// The data layer for the Settings tab: roles, which role each surface uses,
// app-wide recording/retention settings, and remembered facts. Kept out of
// JSX for the same reason api.ts and useTurn.ts are - Settings.tsx,
// RoleEditor.tsx and Memory.tsx all need these types and calls, and none of
// the logic here needs a component to exist.
//
// server/main.py is the source of truth this was written against; see its
// RoleIn/RolePatch/AppSettingsIn models and the /roles, /settings and
// /memory routes.

import { api } from './api'

// server/roles.py's SPEAKABLE_LANGUAGES - the only three edge-tts has a
// voice for. A role may narrow this set (down to one) but never extend it;
// server/roles.py._validate rejects anything else with a 422, and this list
// is what turns "the three checkboxes" into something that cannot drift out
// of sync with the backend by hand.
export const LANGUAGES = ['uk', 'ru', 'en'] as const
export type Language = (typeof LANGUAGES)[number]

// server/roles.py's MOODS - exactly the nine faces the firmware draws. A
// tenth would be silently ignored by the device rather than rejected, so
// this list is what keeps the select from ever offering one.
export const MOODS = [
  'neutral', 'happy', 'excited', 'curious', 'confused',
  'surprised', 'sad', 'annoyed', 'sleepy',
] as const
export type Mood = (typeof MOODS)[number]

export type Surface = 'esp32' | 'web'

/** Every field GET /roles and GET /settings/surfaces return, verbatim. */
export interface Role {
  id: number
  name: string
  prompt: string | null
  max_sentences: number
  markdown_allowed: boolean
  languages: string[]
  pinned_mood: string | null
  built_in: boolean
}

/** The editable fields of a role, without the server-assigned id/built_in.
 * What RoleEditor.tsx holds as its working copy, and what buildRolePatch
 * below diffs against the role it started from. */
export interface RoleDraft {
  name: string
  prompt: string | null
  max_sentences: number
  markdown_allowed: boolean
  languages: string[]
  pinned_mood: string | null
}

export function draftFromRole(role: Role): RoleDraft {
  return {
    name: role.name,
    prompt: role.prompt,
    max_sentences: role.max_sentences,
    markdown_allowed: role.markdown_allowed,
    languages: [...role.languages],
    pinned_mood: role.pinned_mood,
  }
}

/** Starting point for the "new role" form - server/roles.py's own RoleIn
 * defaults, so a role created without touching a field behaves the same
 * whether it was typed here or posted by hand. */
export function blankRoleDraft(): RoleDraft {
  return {
    name: '',
    prompt: null,
    max_sentences: 2,
    markdown_allowed: false,
    languages: [...LANGUAGES],
    pinned_mood: null,
  }
}

export type RolePatch = Partial<RoleDraft>

function sameLanguages(a: string[], b: string[]): boolean {
  if (a.length !== b.length) return false
  const sortedA = [...a].sort()
  const sortedB = [...b].sort()
  return sortedA.every((value, index) => value === sortedB[index])
}

/**
 * Diffs a draft against the role it started from and keeps only the fields
 * that actually changed - this is the whole answer to "untouched" versus
 * "cleared". PUT /roles/{id} (server/main.py's update_role, backed by
 * RolePatch's model_fields_set) patches only the keys present in the body,
 * and `prompt: null` is a real instruction there ("revert to the built-in
 * wording"), not an absence.
 *
 * A naive `{ ...draft }` spread would send every field on every save,
 * including a `prompt: null` for a role nobody touched the prompt of just
 * because some *other* field changed. Diffing against the original instead
 * means an untouched `prompt` is `=== original.prompt` and is left out of
 * the payload entirely, while clicking "Use the built-in wording" sets
 * `draft.prompt` to `null` - different from a non-null original - so *that*
 * shows up as an explicit `prompt: null`. Nothing here has to track "was
 * this field touched" as a separate bit; the comparison already carries it.
 */
export function buildRolePatch(original: RoleDraft, draft: RoleDraft): RolePatch {
  const patch: RolePatch = {}
  if (draft.name !== original.name) patch.name = draft.name
  if (draft.prompt !== original.prompt) patch.prompt = draft.prompt
  if (draft.max_sentences !== original.max_sentences) patch.max_sentences = draft.max_sentences
  if (draft.markdown_allowed !== original.markdown_allowed) {
    patch.markdown_allowed = draft.markdown_allowed
  }
  if (!sameLanguages(draft.languages, original.languages)) patch.languages = [...draft.languages]
  if (draft.pinned_mood !== original.pinned_mood) patch.pinned_mood = draft.pinned_mood
  return patch
}

/** Which field an error from a role create/update most likely names, so it
 * can be shown next to that field rather than as a generic banner. Matched
 * against the server's own wording (server/roles.py's NameTaken/ValueError
 * messages, and FastAPI's own "at most 2000 characters" for an over-length
 * prompt) - not exhaustive, and callers fall back to a form-level message
 * when nothing matches. */
export function fieldForRoleError(detail: string): keyof RoleDraft | null {
  const lower = detail.toLowerCase()
  if (lower.includes('name')) return 'name'
  if (lower.includes('language')) return 'languages'
  if (lower.includes('mood')) return 'pinned_mood'
  if (lower.includes('prompt') || lower.includes('character')) return 'prompt'
  return null
}

export interface AppSettings {
  store_conversations: boolean
  retention_days: number
}

export type FactSource = 'auto' | 'user'

export interface Fact {
  id: number
  text: string
  source: FactSource
  created_at: string
}

// The one device this whole app talks to - the same literal useTurn.ts
// connects the socket with ("?device=default") and server/main.py defaults
// to when no `device` query param is given. Memory and conversations are
// namespaced by device_id on the server, but this project has exactly one.
// Exported (additively - every other caller here still uses it privately)
// so Devices.tsx can show the real id instead of a second copy of the
// literal.
export const DEVICE_ID = 'default'

export const rolesApi = {
  list: (): Promise<Role[]> => api.get('/roles'),
  create: (draft: RoleDraft): Promise<{ id: number }> => api.post('/roles', draft),
  update: (id: number, patch: RolePatch): Promise<{ status: string }> =>
    api.put(`/roles/${id}`, patch),
  remove: (id: number): Promise<{ status: string }> => api.del(`/roles/${id}`),
}

export const surfacesApi = {
  get: (): Promise<Record<Surface, Role>> => api.get('/settings/surfaces'),
  set: (surface: Surface, roleId: number): Promise<{ status: string }> =>
    api.put(`/settings/surfaces/${surface}`, { role_id: roleId }),
}

export const appSettingsApi = {
  get: (): Promise<AppSettings> => api.get('/settings/app'),
  set: (patch: Partial<AppSettings>): Promise<AppSettings> => api.put('/settings/app', patch),
}

/** One message inside a recorded conversation - server/memory/store.py's
 * conversation_rows() shape, verbatim. */
export interface ConversationMessage {
  role: 'user' | 'assistant'
  text: string
  emotion: string | null
}

/** One recorded conversation, from GET /conversations/{id} (Task 5). Real
 * since 2 Sep 2026, when recording went on in production - Journal.tsx is
 * the first screen to read this back rather than only writing it. */
export interface ConversationRow {
  id: number
  surface: Surface
  role_name: string
  started_at: string
  ended_at: string | null
  turns: number
  messages: ConversationMessage[]
}

/** One session's usage, from GET /usage/{id} (Task 5) - server/memory/
 * store.py's usage_rows(), which has been logging since long before
 * recording did. Per-session, not per-minute: see Journal.tsx's own
 * comment on why that rules out a "percent of the free-tier ceiling" bar. */
export interface UsageRow {
  turns: number
  audio_seconds: number
  prompt_tokens: number
  completion_tokens: number
  tts_chars: number
  created_at: string
}

export const journalApi = {
  conversations: (): Promise<ConversationRow[]> => api.get(`/conversations/${DEVICE_ID}`),
  usage: (): Promise<UsageRow[]> => api.get(`/usage/${DEVICE_ID}`),
}

export const memoryApi = {
  list: (): Promise<Fact[]> => api.get(`/memory/${DEVICE_ID}`),
  add: (text: string): Promise<{ id: number }> => api.post(`/memory/${DEVICE_ID}`, { text }),
  remove: (factId: number): Promise<{ status: string }> =>
    api.del(`/memory/${DEVICE_ID}/${factId}`),
  // Facts and recorded conversations both, per server/memory/store.py's
  // forget() - this is the control that actually cleans a stranger's data
  // off the deployed server, so it must not be confused with clearConversations.
  clearEverything: (): Promise<{ status: string }> => api.del(`/memory/${DEVICE_ID}`),
  // Transcripts only; facts extracted from them stay. server/main.py's
  // clear_conversations calls Store.delete_conversations, deliberately kept
  // apart from forget() for exactly this distinction.
  clearConversations: (): Promise<{ status: string }> => api.del(`/conversations/${DEVICE_ID}`),
}
