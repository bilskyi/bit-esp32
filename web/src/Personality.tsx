import { useCallback, useEffect, useId, useRef, useState } from 'react'
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
  surfacesApi,
} from './settingsApi'
import type { Role, RoleDraft, Surface } from './settingsApi'

interface PersonalityProps {
  notifyUnauthorized: () => void
}

const PROMPT_MAX = 2000

// settingsApi.ts's own LANGUAGES is ['uk','ru','en'] - the order
// server/roles.py's SPEAKABLE_LANGUAGES declares them in. The approved
// design shows the three checkboxes uk/en/ru, each with its own autonym;
// this is a display-only reordering, not a change to the set itself.
const LANGUAGE_ORDER: (typeof LANGUAGES)[number][] = ['uk', 'en', 'ru']
const LANGUAGE_LABELS: Record<(typeof LANGUAGES)[number], string> = {
  uk: 'uk — українська',
  en: 'en — english',
  ru: 'ru — русский',
}

const SURFACES: Surface[] = ['esp32', 'web']
const SURFACE_LABEL: Record<Surface, string> = { esp32: 'Робочий стіл', web: 'Цей браузер' }
const SURFACE_SELECT_LABEL: Record<Surface, string> = {
  esp32: 'Персона для пристрою',
  web: 'Персона для браузера',
}

function describeError(err: unknown, fallback: string): string {
  return err instanceof ApiError ? err.detail : fallback
}

// Same paths Shell.tsx draws for the "Поверхні" nav - duplicated rather than
// exported from there, since Shell.tsx's icons are private to its own nav
// markup and this is a different piece of chrome (a surface picker, not a
// section link) that happens to want the same glyph.
function DesktopIcon() {
  return (
    <svg className="ic" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" aria-hidden="true">
      <rect x="3" y="5.6" width="18" height="12.8" rx="2.2" strokeLinejoin="round" />
      <rect x="7.4" y="9.8" width="2.8" height="4.4" rx="1.2" fill="currentColor" stroke="none" />
      <rect x="13.8" y="9.8" width="2.8" height="4.4" rx="1.2" fill="currentColor" stroke="none" />
    </svg>
  )
}

function BrowserIcon() {
  return (
    <svg className="ic" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" aria-hidden="true">
      <rect x="3" y="4.5" width="18" height="12" rx="1.8" />
      <path strokeLinecap="round" d="M8 20h8M12 16.5V20" />
    </svg>
  )
}

interface PersonaEditorProps {
  /** null means "creating a new role" - same meaning as RoleEditor.tsx's own
   * prop, ported unchanged. */
  role: Role | null
  /** The role id the esp32 surface is actually running - used only to decide
   * whether the measured-wording warning below the prompt applies. */
  activeDeviceRoleId: number | null
  /** Whether this role is the one the web surface (this browser) is running
   * right now - purely cosmetic, the "активна тут" pill in the header. */
  isActiveHere: boolean
  notifyUnauthorized: () => void
  onSaved: (createdId?: number) => void | Promise<void>
  onDeleted: () => void | Promise<void>
}

/** The persona editor: RoleEditor.tsx's logic (draft state, the original-vs-
 * draft diff via buildRolePatch, field-level 409/422 mapping, the language-
 * floor and prompt-limit guards) carried over essentially unchanged, onto
 * the approved design's markup and Ukrainian copy. See settingsApi.ts's own
 * comment on buildRolePatch for why a diff against `originalRef` - not a
 * plain spread - is what keeps "never touched this field" and "explicitly
 * cleared it" from colliding on the wire; settingsApi.test.tsx pins that
 * behaviour down directly and must keep passing untouched. */
function PersonaEditor({
  role, activeDeviceRoleId, isActiveHere, notifyUnauthorized, onSaved, onDeleted,
}: PersonaEditorProps) {
  const isNew = role === null
  const [draft, setDraft] = useState<RoleDraft>(() => (role ? draftFromRole(role) : blankRoleDraft()))
  const originalRef = useRef<RoleDraft>(draft)

  const [saving, setSaving] = useState(false)
  const [deleting, setDeleting] = useState(false)
  const [formError, setFormError] = useState<string | null>(null)
  const [fieldErrors, setFieldErrors] = useState<Partial<Record<keyof RoleDraft, string>>>({})

  const nameId = useId()
  const promptId = useId()

  const builtIn = role?.built_in ?? false
  const promptLength = draft.prompt?.length ?? 0
  const overLimit = promptLength > PROMPT_MAX

  // The one line of caution this feature needs: shown only while editing the
  // role the device is actually using, and only once its prompt is
  // something other than the measured, built-in wording (already
  // customised, or being customised right now) - ported verbatim from
  // RoleEditor.tsx.
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

  // The nine mood buttons are one toggle group: pressing the one already
  // pinned unpins it (there is no separate "not pinned" control), pressing
  // any other moves the pin there.
  const togglePinnedMood = (mood: string) => {
    setField('pinned_mood', draft.pinned_mood === mood ? null : mood)
  }

  const revertPrompt = () => setField('prompt', null)

  const resetDraft = () => {
    setDraft(role ? draftFromRole(role) : blankRoleDraft())
    setFormError(null)
    setFieldErrors({})
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
      setFieldErrors({ name: 'Персоні потрібна назва.' })
      return
    }
    if (draft.languages.length === 0) {
      setFieldErrors({ languages: 'Потрібна хоча б одна мова.' })
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
      handleFail(err, role === null ? 'Не вдалося створити персону.' : 'Не вдалося зберегти персону.')
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
      handleFail(err, 'Не вдалося видалити персону.')
    } finally {
      setDeleting(false)
    }
  }

  return (
    <div className="panel" style={{ minWidth: 0 }}>
      <div className="ph">
        <h2>{isNew ? 'Нова персона' : (role?.name ?? 'Персона')}</h2>
        {isActiveHere && <span className="pill acc">активна тут</span>}
        <span className="cnt">редактор персони</span>
      </div>
      <form onSubmit={handleSubmit}>
      <div className="pad formcap">
        {formError && (
          <p className="chat-status" role="alert">
            {formError}
          </p>
        )}

        {builtIn && (
          <div className="locked boxed" style={{ marginTop: 0, marginBottom: 12 }}>
            <span className="dot acc" aria-hidden="true" />
            <p>
              <b>Вбудовану персону не можна перейменувати чи видалити.</b> Вона створюється заново
              щоразу, коли стартує сервер, — тож будь-яка правка назви однаково зникла б. Зроби
              копію собі й редагуй її.
            </p>
          </div>
        )}

        <div className="field" style={{ marginTop: 0 }}>
          <label className="flab" htmlFor={nameId}>
            <span>назва</span>
          </label>
          <input
            id={nameId}
            type="text"
            value={draft.name}
            onChange={(event) => setField('name', event.target.value)}
            disabled={builtIn || saving}
            required
          />
          {fieldErrors.name && (
            <p className="chat-status" role="alert">
              {fieldErrors.name}
            </p>
          )}
        </div>

        <div className="field">
          <label className="flab" htmlFor={promptId}>
            <span>промпт персони</span>
            <span className={overLimit ? 'c warn' : 'c'}>
              {promptLength} / {PROMPT_MAX}
            </span>
          </label>
          <textarea
            id={promptId}
            spellCheck={false}
            value={draft.prompt ?? ''}
            placeholder="Використовую вбудоване формулювання. Почни писати, щоб перевизначити його."
            onChange={(event) => setField('prompt', event.target.value)}
            disabled={saving}
          />
          <div style={{ marginTop: 6 }}>
            <button
              type="button"
              className="btn xs ghost"
              onClick={revertPrompt}
              disabled={draft.prompt === null || saving}
            >
              Використати вбудований варіант
            </button>
          </div>
          {showDeviceWarning && (
            <p className="prose" style={{ fontSize: 12, marginTop: 6 }}>
              Типове формулювання пристрою підібране виміряно: змінивши його, можна погіршити, як
              часто обличчя вгадує правильну емоцію. «Використати вбудований варіант» поверне його
              назад.
            </p>
          )}
          {fieldErrors.prompt && (
            <p className="chat-status" role="alert">
              {fieldErrors.prompt}
            </p>
          )}
        </div>

        <div className="field">
          <p className="flab">
            <span>довжина й розмітка</span>
          </p>
          <div className="opts">
            <label className="check">
              <input
                type="checkbox"
                checked={draft.markdown_allowed}
                onChange={(event) => setField('markdown_allowed', event.target.checked)}
                disabled={saving}
              />
              <span>markdown увімкнено</span>
            </label>
            <span className="opts" style={{ gap: 6 }}>
              <span className="dim" style={{ fontSize: 11 }}>
                речень максимум
              </span>
              <select
                aria-label="Максимум речень"
                value={draft.max_sentences}
                onChange={(event) => setField('max_sentences', Number(event.target.value))}
                disabled={saving}
              >
                {Array.from({ length: 10 }, (_, i) => i + 1).map((n) => (
                  <option key={n} value={n}>
                    {n}
                  </option>
                ))}
              </select>
            </span>
          </div>
        </div>

        <div className="field">
          <p className="flab">
            <span>мови</span>
            <span className="c">лише зі списку, не вільний текст</span>
          </p>
          <div className="opts">
            {LANGUAGE_ORDER.map((code) => {
              const checked = draft.languages.includes(code)
              return (
                <label className="check" key={code}>
                  <input
                    type="checkbox"
                    checked={checked}
                    onChange={(event) => toggleLanguage(code, event.target.checked)}
                    disabled={saving || (checked && draft.languages.length <= 1)}
                  />
                  <span>{LANGUAGE_LABELS[code]}</span>
                </label>
              )
            })}
          </div>
          {fieldErrors.languages && (
            <p className="chat-status" role="alert">
              {fieldErrors.languages}
            </p>
          )}
        </div>

        <div className="field">
          <p className="flab">
            <span>прикріплений настрій</span>
            <span className="c">один з девʼяти</span>
          </p>
          <div className="moods">
            {MOODS.map((mood) => (
              <button
                key={mood}
                type="button"
                className="mood"
                aria-pressed={draft.pinned_mood === mood}
                onClick={() => togglePinnedMood(mood)}
                disabled={saving}
              >
                {mood}
              </button>
            ))}
          </div>
          <p className="prose" style={{ fontSize: 12, marginTop: 6 }}>
            Прикріплений настрій перебиває той, що я вибрала б сама. Очі на пристрої слухаються
            саме його. Натисни активний настрій ще раз, щоб відкріпити.
          </p>
          {fieldErrors.pinned_mood && (
            <p className="chat-status" role="alert">
              {fieldErrors.pinned_mood}
            </p>
          )}
        </div>
      </div>
        <div className="pf" style={{ minHeight: 40 }}>
          <span className="acts">
            <button type="submit" className="btn pri" disabled={saving || deleting}>
              {saving ? 'Зберігаю…' : isNew ? 'Створити' : 'Зберегти'}
            </button>
            <button type="button" className="btn" onClick={resetDraft} disabled={saving || deleting}>
              Скинути
            </button>
          </span>
          {!isNew && (
            <span className="acts">
              <button
                type="button"
                className="btn ghost"
                onClick={handleDelete}
                disabled={builtIn || saving || deleting}
                title={builtIn ? 'Вбудовані персони не видаляються.' : undefined}
              >
                {deleting ? 'Видаляю…' : 'Видалити персону'}
              </button>
            </span>
          )}
        </div>
      </form>
    </div>
  )
}

/** Характер: the roles list (GET /roles) and this editor, plus which surface
 * runs which role (GET/PUT /settings/surfaces) - the one section of this
 * task with a real backend behind every control on it. Follows Settings.tsx's
 * own rule of re-reading after every mutation rather than patching local
 * state, for the same reason: these are rare writes, and a stale role list
 * is exactly how someone ends up editing the wrong persona. */
function Personality({ notifyUnauthorized }: PersonalityProps) {
  const [roles, setRoles] = useState<Role[] | null>(null)
  const [surfaces, setSurfaces] = useState<Record<Surface, Role> | null>(null)
  const [loadError, setLoadError] = useState<string | null>(null)

  const [selection, setSelection] = useState<number | 'new' | null>(null)

  const [surfaceBusy, setSurfaceBusy] = useState<Surface | null>(null)
  const [surfaceNotice, setSurfaceNotice] = useState<Partial<Record<Surface, string>>>({})
  const [surfaceError, setSurfaceError] = useState<Partial<Record<Surface, string>>>({})

  const onUnauthorizedOr = useCallback(
    (err: unknown, setter: (message: string) => void, fallback: string) => {
      if (err instanceof Unauthorized) {
        notifyUnauthorized()
        return
      }
      setter(describeError(err, fallback))
    },
    [notifyUnauthorized],
  )

  const reloadRoles = useCallback(async () => {
    try {
      setRoles(await rolesApi.list())
    } catch (err) {
      onUnauthorizedOr(err, setLoadError, 'Не вдалося завантажити персони.')
    }
  }, [onUnauthorizedOr])

  const reloadSurfaces = useCallback(async () => {
    try {
      setSurfaces(await surfacesApi.get())
    } catch (err) {
      onUnauthorizedOr(err, setLoadError, 'Не вдалося завантажити налаштування поверхонь.')
    }
  }, [onUnauthorizedOr])

  useEffect(() => {
    // useEffect's own callback cannot be async; the standard shape for
    // "fire two independent GETs on mount" is a nested async function,
    // invoked once, right here.
    async function load() {
      await Promise.all([reloadRoles(), reloadSurfaces()])
    }
    load()
  }, [reloadRoles, reloadSurfaces])

  const handleSurfaceChange = async (surface: Surface, roleId: number) => {
    setSurfaceBusy(surface)
    setSurfaceError((prev) => ({ ...prev, [surface]: undefined }))
    setSurfaceNotice((prev) => ({ ...prev, [surface]: undefined }))
    try {
      await surfacesApi.set(surface, roleId)
      await reloadSurfaces()
      setSurfaceNotice((prev) => ({ ...prev, [surface]: 'Збережено — діє з наступного питання.' }))
    } catch (err) {
      if (err instanceof Unauthorized) {
        notifyUnauthorized()
      } else {
        setSurfaceError((prev) => ({ ...prev, [surface]: describeError(err, 'Не вдалося змінити.') }))
      }
    } finally {
      setSurfaceBusy(null)
    }
  }

  const handleRoleSaved = async (createdId?: number) => {
    await Promise.all([reloadRoles(), reloadSurfaces()])
    if (createdId !== undefined) setSelection(createdId)
  }

  const handleRoleDeleted = async () => {
    setSelection(null)
    await Promise.all([reloadRoles(), reloadSurfaces()])
  }

  // Nothing explicitly picked yet defaults to whichever persona this browser
  // is actually running, so the editor is never empty on first paint -
  // derived during render (not set via an effect) so there is never a frame
  // where selection is still null after roles and surfaces have loaded.
  const effectiveSelection: number | 'new' | null = selection ?? (surfaces ? surfaces.web.id : null)
  const selectedRole =
    typeof effectiveSelection === 'number' ? (roles?.find((r) => r.id === effectiveSelection) ?? null) : null
  const editorKey =
    effectiveSelection === null ? 'none' : effectiveSelection === 'new' ? 'new' : `role-${effectiveSelection}`
  const customCount = roles ? roles.filter((r) => !r.built_in).length : 0

  return (
    <section className="view">
      <div className="wrap">
        <div className="head">
          <div className="hl">
            <p className="lab">Характер · по одній персоні на поверхню</p>
            <h1 style={{ fontSize: 18, marginTop: 2 }}>Ким мені бути.</h1>
            <p className="prose" style={{ fontSize: 13 }}>
              Кожна поверхня має свою персону. На столі я коротка, тут — докладна. Памʼять при
              цьому одна.
            </p>
          </div>
        </div>

        {loadError && (
          <p className="chat-status" role="alert">
            {loadError}
          </p>
        )}

        {roles === null || surfaces === null ? (
          <p className="prose">Завантажую…</p>
        ) : (
          <div className="grid2" style={{ gridTemplateColumns: 'minmax(0,1fr)' }}>
            <div className="pers-two" style={{ display: 'grid', gap: 16, gridTemplateColumns: 'minmax(0,1fr)' }}>
              <div className="stack">
                <div className="grp">
                  <div className="grph">
                    <h2>Персони</h2>
                    <span className="cnt">
                      {roles.length} · своїх {customCount}
                    </span>
                  </div>
                  <div className="plist" role="tablist" aria-label="Персони">
                    {roles.map((role) => (
                      <button
                        key={role.id}
                        type="button"
                        role="tab"
                        aria-selected={effectiveSelection === role.id}
                        onClick={() => setSelection(role.id)}
                      >
                        <span className="pn">{role.name}</span>
                        {role.built_in && <span className="pill">вбудована</span>}
                        {role.id === surfaces.web.id && <span className="pill acc">активна тут</span>}
                      </button>
                    ))}
                  </div>
                  <div className="foot">
                    <span>вбудовані не видаляються</span>
                    <span className="acts">
                      <button type="button" className="btn xs" onClick={() => setSelection('new')}>
                        Нова персона
                      </button>
                    </span>
                  </div>
                </div>

                <div className="grp" style={{ marginTop: 20 }}>
                  <div className="grph">
                    <h2>Яка де</h2>
                    <span className="cnt">2 поверхні</span>
                  </div>
                  {SURFACES.map((surface) => (
                    <div className="surf" key={surface}>
                      <span className="sl">
                        {surface === 'esp32' ? <DesktopIcon /> : <BrowserIcon />}
                        {SURFACE_LABEL[surface]}
                      </span>
                      <select
                        aria-label={SURFACE_SELECT_LABEL[surface]}
                        value={surfaces[surface].id}
                        onChange={(event) => handleSurfaceChange(surface, Number(event.target.value))}
                        disabled={surfaceBusy === surface}
                      >
                        {roles.map((role) => (
                          <option key={role.id} value={role.id}>
                            {role.name}
                          </option>
                        ))}
                      </select>
                    </div>
                  ))}
                  <p className="prose" style={{ fontSize: 12, marginTop: 8 }}>
                    Зміна діє з наступним питанням — пристрій нічого не перезавантажує.
                  </p>
                  {SURFACES.map(
                    (surface) =>
                      (surfaceError[surface] || surfaceNotice[surface]) && (
                        <p
                          key={surface}
                          className="chat-status"
                          role={surfaceError[surface] ? 'alert' : undefined}
                        >
                          {surfaceError[surface] ?? surfaceNotice[surface]}
                        </p>
                      ),
                  )}
                </div>
              </div>

              {effectiveSelection !== null && (
                <PersonaEditor
                  key={editorKey}
                  role={effectiveSelection === 'new' ? null : selectedRole}
                  activeDeviceRoleId={surfaces.esp32.id}
                  isActiveHere={selectedRole !== null && selectedRole.id === surfaces.web.id}
                  notifyUnauthorized={notifyUnauthorized}
                  onSaved={handleRoleSaved}
                  onDeleted={handleRoleDeleted}
                />
              )}
            </div>
          </div>
        )}
      </div>
    </section>
  )
}

export default Personality
