import { useId, useRef, useState } from 'react'
import type { FormEvent } from 'react'
import { ApiError, Unauthorized } from './api'
import {
  LANGUAGES,
  MOODS,
  blankRoleDraft,
  buildRolePatch,
  draftFromRole,
  fieldForRoleError,
  rolesApi,
} from './settingsApi'
import type { Role, RoleDraft } from './settingsApi'

const PROMPT_MAX = 2000

interface RoleEditorProps {
  /** null means "creating a new role" - there is nothing to diff a patch
   * against yet, so Save posts the whole draft instead of a patch. */
  role: Role | null
  /** The id of whichever role is currently active on the esp32 surface, so
   * this form can tell whether it is editing *that* role - see the prompt
   * warning below. */
  activeDeviceRoleId: number | null
  notifyUnauthorized: () => void
  /** Called after a create or an edit is saved. Passed the new role's id
   * only on create, so Settings.tsx can select it - an edit keeps whatever
   * was already selected. */
  onSaved: (createdId?: number) => void | Promise<void>
  onDeleted: () => void | Promise<void>
  onCancel: () => void
}

function describeError(err: unknown, fallback: string): string {
  return err instanceof ApiError ? err.detail : fallback
}

/** The role list plus this editor is the whole feature this task exists
 * for: everything about a role except its id and built_in flag is exposed
 * here, including a built-in role's own prompt, because rewording *that*
 * without touching the code is the point (see server/persona.py and
 * RESUME.md for why the measured wording still deserves a warning, not a
 * lock).
 *
 * Owns its own working copy of the role (`draft`) and diffs it against
 * `originalRef` on save via buildRolePatch - see settingsApi.ts for why a
 * diff, not a spread, is what keeps "untouched" and "cleared to built-in"
 * from colliding on the wire. `originalRef` is a ref rather than derived
 * from props so a successful save can move the baseline forward without
 * Settings.tsx's post-save reload (which hands back a *new* role object
 * with the same id) wiping an edit made in the same instant. */
function RoleEditor({
  role, activeDeviceRoleId, notifyUnauthorized, onSaved, onDeleted, onCancel,
}: RoleEditorProps) {
  const isNew = role === null
  const [draft, setDraft] = useState<RoleDraft>(() => (role ? draftFromRole(role) : blankRoleDraft()))
  const originalRef = useRef<RoleDraft>(draft)

  const [saving, setSaving] = useState(false)
  const [deleting, setDeleting] = useState(false)
  const [formError, setFormError] = useState<string | null>(null)
  const [fieldErrors, setFieldErrors] = useState<Partial<Record<keyof RoleDraft, string>>>({})

  const nameId = useId()
  const promptId = useId()
  const sentencesId = useId()

  const builtIn = role?.built_in ?? false
  const promptLength = draft.prompt?.length ?? 0
  const overLimit = promptLength > PROMPT_MAX

  // The one line of caution this feature needs: shown only while editing
  // the role the device is actually using, and only once its prompt is
  // something other than the measured, built-in wording (already
  // customised, or being customised right now).
  const showDeviceWarning =
    role !== null && role.id === activeDeviceRoleId && draft.prompt !== null

  const setField = <K extends keyof RoleDraft>(key: K, value: RoleDraft[K]) => {
    setDraft((prev) => ({ ...prev, [key]: value }))
    setFieldErrors((prev) => (prev[key] ? { ...prev, [key]: undefined } : prev))
  }

  const toggleLanguage = (code: string, checked: boolean) => {
    if (checked) {
      if (!draft.languages.includes(code)) setField('languages', [...draft.languages, code])
      return
    }
    // A role needs at least one voice - refusing silently here (rather than
    // letting the box uncheck and failing on save) is what keeps the last
    // checkbox from looking interactive when it isn't.
    if (draft.languages.length <= 1) return
    setField('languages', draft.languages.filter((c) => c !== code))
  }

  const handleFail = (err: unknown, fallback: string) => {
    if (err instanceof Unauthorized) {
      notifyUnauthorized()
      return
    }
    if (err instanceof ApiError) {
      const field = fieldForRoleError(err.detail)
      if (field) {
        setFieldErrors((prev) => ({ ...prev, [field]: err.detail }))
        return
      }
    }
    setFormError(describeError(err, fallback))
  }

  const handleSubmit = async (event: FormEvent) => {
    event.preventDefault()
    setFormError(null)
    setFieldErrors({})

    const name = draft.name.trim()
    if (!name) {
      setFieldErrors({ name: 'A role needs a name.' })
      return
    }
    if (draft.languages.length === 0) {
      setFieldErrors({ languages: 'A role needs at least one language.' })
      return
    }

    setSaving(true)
    try {
      if (role === null) {
        const created = await rolesApi.create({ ...draft, name })
        await onSaved(created.id)
      } else {
        const patch = buildRolePatch(originalRef.current, { ...draft, name })
        if (Object.keys(patch).length > 0) {
          await rolesApi.update(role.id, patch)
          originalRef.current = { ...draft, name }
        }
        await onSaved()
      }
    } catch (err) {
      handleFail(err, role === null ? 'Could not create that role.' : 'Could not save that role.')
    } finally {
      setSaving(false)
    }
  }

  const handleDelete = async () => {
    if (role === null || role.built_in) return
    setDeleting(true)
    setFormError(null)
    try {
      await rolesApi.remove(role.id)
      await onDeleted()
    } catch (err) {
      handleFail(err, 'Could not delete that role.')
    } finally {
      setDeleting(false)
    }
  }

  const revertPrompt = () => setField('prompt', null)

  return (
    <form className="role-editor" onSubmit={handleSubmit}>
      {formError && (
        <p className="settings-error" role="alert">
          {formError}
        </p>
      )}

      {builtIn && (
        <p className="settings-note">
          Built in. It is re-created on every start, so it cannot be renamed or deleted.
        </p>
      )}

      <div className="field">
        <label htmlFor={nameId}>Name</label>
        <input
          id={nameId}
          type="text"
          value={draft.name}
          onChange={(event) => setField('name', event.target.value)}
          disabled={builtIn || saving}
          required
        />
        {fieldErrors.name && (
          <p className="settings-error" role="alert">
            {fieldErrors.name}
          </p>
        )}
      </div>

      <div className="field">
        <label htmlFor={promptId}>Persona prompt</label>
        <textarea
          id={promptId}
          rows={5}
          value={draft.prompt ?? ''}
          placeholder="Using the built-in wording. Type to override it."
          onChange={(event) => setField('prompt', event.target.value)}
          disabled={saving}
        />
        <div className="role-editor-prompt-footer">
          <span className={`chat-status${overLimit ? ' is-over-limit' : ''}`}>
            {promptLength}/{PROMPT_MAX}
          </span>
          <button
            type="button"
            onClick={revertPrompt}
            disabled={draft.prompt === null || saving}
          >
            Use the built-in wording
          </button>
        </div>
        {showDeviceWarning && (
          <p className="settings-warning">
            The device&rsquo;s default wording was measured. Overriding it may change how often
            the face picks the right emotion — &ldquo;Use the built-in wording&rdquo; puts it
            back.
          </p>
        )}
        {fieldErrors.prompt && (
          <p className="settings-error" role="alert">
            {fieldErrors.prompt}
          </p>
        )}
      </div>

      <div className="field">
        <label htmlFor={sentencesId}>Max sentences</label>
        <input
          id={sentencesId}
          type="number"
          min={1}
          max={10}
          step={1}
          value={draft.max_sentences}
          onChange={(event) => {
            const raw = Number(event.target.value)
            const clamped = Number.isFinite(raw) ? Math.min(10, Math.max(1, Math.round(raw))) : 1
            setField('max_sentences', clamped)
          }}
          disabled={saving}
        />
      </div>

      <div className="field-row">
        <input
          id={`${sentencesId}-markdown`}
          type="checkbox"
          checked={draft.markdown_allowed}
          onChange={(event) => setField('markdown_allowed', event.target.checked)}
          disabled={saving}
        />
        <label htmlFor={`${sentencesId}-markdown`}>Markdown allowed</label>
      </div>

      <fieldset className="field">
        <legend>Languages</legend>
        <div className="checkbox-group">
          {LANGUAGES.map((code) => {
            const checked = draft.languages.includes(code)
            const inputId = `${nameId}-lang-${code}`
            return (
              <div className="field-row" key={code}>
                <input
                  id={inputId}
                  type="checkbox"
                  checked={checked}
                  onChange={(event) => toggleLanguage(code, event.target.checked)}
                  disabled={saving || (checked && draft.languages.length <= 1)}
                />
                <label htmlFor={inputId}>{code}</label>
              </div>
            )
          })}
        </div>
        <p className="settings-note">A role needs at least one language.</p>
        {fieldErrors.languages && (
          <p className="settings-error" role="alert">
            {fieldErrors.languages}
          </p>
        )}
      </fieldset>

      <div className="field">
        <label htmlFor={`${nameId}-mood`}>Pinned mood</label>
        <select
          id={`${nameId}-mood`}
          value={draft.pinned_mood ?? ''}
          onChange={(event) => setField('pinned_mood', event.target.value || null)}
          disabled={saving}
        >
          <option value="">Not pinned</option>
          {MOODS.map((mood) => (
            <option key={mood} value={mood}>
              {mood}
            </option>
          ))}
        </select>
        {fieldErrors.pinned_mood && (
          <p className="settings-error" role="alert">
            {fieldErrors.pinned_mood}
          </p>
        )}
      </div>

      <div className="role-editor-actions">
        <button type="submit" disabled={saving || deleting}>
          {saving ? 'Saving…' : isNew ? 'Create role' : 'Save changes'}
        </button>
        <button type="button" onClick={onCancel} disabled={saving || deleting}>
          Cancel
        </button>
        {!isNew && (
          <button
            type="button"
            onClick={handleDelete}
            disabled={builtIn || saving || deleting}
            title={builtIn ? 'Built-in roles cannot be deleted.' : undefined}
          >
            {deleting ? 'Deleting…' : 'Delete role'}
          </button>
        )}
      </div>
    </form>
  )
}

export default RoleEditor
