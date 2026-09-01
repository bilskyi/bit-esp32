import { useCallback, useEffect, useId, useState } from 'react'
import { ApiError, Unauthorized, useSession } from './api'
import Memory from './Memory.tsx'
import RoleEditor from './RoleEditor.tsx'
import { appSettingsApi, rolesApi, surfacesApi } from './settingsApi'
import type { AppSettings, Role, Surface } from './settingsApi'

const SURFACE_LABELS: Record<Surface, string> = { esp32: 'Device', web: 'This browser' }
const SURFACES: Surface[] = ['esp32', 'web']

type Selection = number | 'new' | null

function describeError(err: unknown, fallback: string): string {
  return err instanceof ApiError ? err.detail : fallback
}

/** The Settings tab: which role each surface uses, the role editor, memory,
 * and recording/retention. Everything here follows one rule instead of the
 * usual optimistic-update pattern: every mutation re-reads its section from
 * the server rather than patching local state, because these are small,
 * rare writes and a stale role list (someone else's edit, a role that no
 * longer exists) is exactly how a person ends up editing the wrong thing.
 * Memory.tsx keeps that same rule for its own list, independently. */
function Settings() {
  const { notifyUnauthorized } = useSession()

  const [roles, setRoles] = useState<Role[] | null>(null)
  const [surfaces, setSurfaces] = useState<Record<Surface, Role> | null>(null)
  const [appSettings, setAppSettings] = useState<AppSettings | null>(null)
  const [loadError, setLoadError] = useState<string | null>(null)

  const [selection, setSelection] = useState<Selection>(null)

  const [surfaceBusy, setSurfaceBusy] = useState<Surface | null>(null)
  const [surfaceNotice, setSurfaceNotice] = useState<Partial<Record<Surface, string>>>({})
  const [surfaceError, setSurfaceError] = useState<Partial<Record<Surface, string>>>({})

  const [appBusy, setAppBusy] = useState(false)
  const [appError, setAppError] = useState<string | null>(null)
  // The retention number, while the person is mid-edit. Separate from
  // appSettings.retention_days rather than synced to it via an effect: a
  // PUT fires once, on blur or Enter - firing on every keystroke would mean
  // one request per digit typed, and the responses can race back out of
  // order. Null means "showing the server's own value, nothing pending".
  const [retentionDraft, setRetentionDraft] = useState<string | null>(null)

  const recordId = useId()
  const retentionId = useId()

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
      const list = await rolesApi.list()
      setRoles(list)
      return list
    } catch (err) {
      onUnauthorizedOr(err, setLoadError, 'Could not load roles.')
      return null
    }
  }, [onUnauthorizedOr])

  const reloadSurfaces = useCallback(async () => {
    try {
      const value = await surfacesApi.get()
      setSurfaces(value)
    } catch (err) {
      onUnauthorizedOr(err, setLoadError, 'Could not load surface settings.')
    }
  }, [onUnauthorizedOr])

  const reloadAppSettings = useCallback(async () => {
    try {
      setAppSettings(await appSettingsApi.get())
    } catch (err) {
      onUnauthorizedOr(err, setLoadError, 'Could not load recording settings.')
    }
  }, [onUnauthorizedOr])

  useEffect(() => {
    setLoadError(null)
    reloadRoles()
    reloadSurfaces()
    reloadAppSettings()
  }, [reloadRoles, reloadSurfaces, reloadAppSettings])

  const handleSurfaceChange = async (surface: Surface, roleId: number) => {
    setSurfaceBusy(surface)
    setSurfaceError((prev) => ({ ...prev, [surface]: undefined }))
    setSurfaceNotice((prev) => ({ ...prev, [surface]: undefined }))
    try {
      await surfacesApi.set(surface, roleId)
      await reloadSurfaces()
      setSurfaceNotice((prev) => ({
        ...prev,
        [surface]: 'Saved — takes effect on the next question.',
      }))
    } catch (err) {
      if (err instanceof Unauthorized) {
        notifyUnauthorized()
      } else {
        setSurfaceError((prev) => ({
          ...prev,
          [surface]: describeError(err, 'Could not change that.'),
        }))
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

  const handleAppSettingsChange = async (patch: Partial<AppSettings>) => {
    setAppBusy(true)
    setAppError(null)
    try {
      setAppSettings(await appSettingsApi.set(patch))
    } catch (err) {
      onUnauthorizedOr(err, setAppError, 'Could not save that setting.')
      // The control the person just touched otherwise looks like it took
      // even though the server rejected it - re-read to show the truth.
      await reloadAppSettings()
    } finally {
      setAppBusy(false)
    }
  }

  const commitRetention = async () => {
    if (retentionDraft === null) return
    const raw = Number(retentionDraft)
    const days = Number.isFinite(raw) ? Math.max(0, Math.round(raw)) : 0
    setRetentionDraft(null)
    if (appSettings && days === appSettings.retention_days) return
    await handleAppSettingsChange({ retention_days: days })
  }

  const selectedRole = typeof selection === 'number' ? (roles?.find((r) => r.id === selection) ?? null) : null
  const editorKey = selection === null ? 'none' : selection === 'new' ? 'new' : `role-${selection}`

  return (
    <div className="settings">
      {loadError && (
        <p className="settings-error" role="alert">
          {loadError}
        </p>
      )}

      <section className="settings-section">
        <h2 className="settings-section-title">Which role each surface uses</h2>
        <p className="settings-note">
          A change takes effect on the next question — the device does not need rebooting.
        </p>
        {roles === null || surfaces === null ? (
          <p className="settings-empty">Loading…</p>
        ) : (
          SURFACES.map((surface) => (
            <div className="settings-surface-row" key={surface}>
              <div className="field">
                <label htmlFor={`surface-${surface}`}>{SURFACE_LABELS[surface]}</label>
                <select
                  id={`surface-${surface}`}
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
              {surfaceError[surface] && (
                <p className="settings-error" role="alert">
                  {surfaceError[surface]}
                </p>
              )}
              {surfaceNotice[surface] && <p className="chat-status">{surfaceNotice[surface]}</p>}
            </div>
          ))
        )}
      </section>

      <section className="settings-section">
        <h2 className="settings-section-title">Roles</h2>
        {roles === null ? (
          <p className="settings-empty">Loading…</p>
        ) : (
          <ul className="settings-role-list">
            {roles.map((role) => (
              <li key={role.id}>
                <button
                  type="button"
                  className="settings-role-item"
                  aria-current={selection === role.id ? 'true' : undefined}
                  onClick={() => setSelection(role.id)}
                >
                  <span className="settings-role-item-name">{role.name}</span>
                  {role.built_in && <span className="settings-role-item-tag">built-in</span>}
                </button>
              </li>
            ))}
          </ul>
        )}
        <div>
          <button type="button" onClick={() => setSelection('new')} disabled={roles === null}>
            + New role
          </button>
        </div>

        {selection !== null && (
          <div className="role-editor-wrap">
            <h3 className="settings-section-title">
              {selection === 'new' ? 'New role' : (selectedRole?.name ?? 'Role')}
            </h3>
            <RoleEditor
              key={editorKey}
              role={selection === 'new' ? null : selectedRole}
              activeDeviceRoleId={surfaces?.esp32.id ?? null}
              notifyUnauthorized={notifyUnauthorized}
              onSaved={handleRoleSaved}
              onDeleted={handleRoleDeleted}
              onCancel={() => setSelection(null)}
            />
          </div>
        )}
      </section>

      <section className="settings-section">
        <h2 className="settings-section-title">Memory</h2>
        <Memory notifyUnauthorized={notifyUnauthorized} />
      </section>

      <section className="settings-section">
        <h2 className="settings-section-title">Recording and retention</h2>
        {appSettings === null ? (
          <p className="settings-empty">Loading…</p>
        ) : (
          <>
            <div className="field-row">
              <input
                id={recordId}
                type="checkbox"
                checked={appSettings.store_conversations}
                onChange={(event) =>
                  handleAppSettingsChange({ store_conversations: event.target.checked })
                }
                disabled={appBusy}
              />
              <label htmlFor={recordId}>Record conversations</label>
            </div>
            <p className="settings-note">
              Recording stores what was said, not just what was remembered from it.
            </p>

            <div className="field">
              <label htmlFor={retentionId}>Keep for … days</label>
              <input
                id={retentionId}
                type="number"
                min={0}
                step={1}
                value={retentionDraft ?? String(appSettings.retention_days)}
                onChange={(event) => setRetentionDraft(event.target.value)}
                onBlur={commitRetention}
                onKeyDown={(event) => {
                  if (event.key === 'Enter') {
                    event.preventDefault()
                    commitRetention()
                  }
                }}
                disabled={appBusy}
              />
            </div>
            <p className="settings-note">0 keeps them forever.</p>
            <p className="settings-note">
              Facts the assistant extracts from a conversation outlive the conversation itself —
              this retention setting does not remove them. Delete them from Memory above if
              that&rsquo;s what you want.
            </p>
            {appError && (
              <p className="settings-error" role="alert">
                {appError}
              </p>
            )}
          </>
        )}
      </section>
    </div>
  )
}

export default Settings
