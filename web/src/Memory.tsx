import { useCallback, useEffect, useId, useState } from 'react'
import type { FormEvent } from 'react'
import { ApiError, Unauthorized } from './api'
import { memoryApi } from './settingsApi'
import type { Fact } from './settingsApi'

interface MemoryProps {
  notifyUnauthorized: () => void
}

type DangerAction = 'transcripts' | 'everything'

// Typed rather than clicked, because these two differ only in blast radius
// and a misclick between them is exactly the mistake a confirm dialog with
// two buttons invites. The phrase names the action, not a generic "yes".
const CONFIRM_PHRASE: Record<DangerAction, string> = {
  transcripts: 'DELETE TRANSCRIPTS',
  everything: 'DELETE EVERYTHING',
}

function describeError(err: unknown, fallback: string): string {
  return err instanceof ApiError ? err.detail : fallback
}

/** Remembered facts (GET /memory/default): what the assistant knows about
 * this device, whether the LLM extracted it at session end (`auto`) or a
 * person typed it here (`user`). Re-reads the whole list after every
 * mutation rather than patching locally - see Settings.tsx's header
 * comment on why that is the rule for this whole tab. */
/** A remembered-on date, short and unambiguous. Returns the raw string if the
 * server ever sends something unparseable, rather than rendering "Invalid
 * Date" over a fact somebody is deciding whether to delete. */
function remembered(value: string): string {
  const at = new Date(value)
  if (Number.isNaN(at.getTime())) return value
  return at.toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' })
}

function Memory({ notifyUnauthorized }: MemoryProps) {
  const [facts, setFacts] = useState<Fact[] | null>(null)
  const [loadError, setLoadError] = useState<string | null>(null)

  const [draft, setDraft] = useState('')
  const [adding, setAdding] = useState(false)
  const [addError, setAddError] = useState<string | null>(null)

  const [pendingDeleteId, setPendingDeleteId] = useState<number | null>(null)
  const [rowError, setRowError] = useState<string | null>(null)

  const [dangerAction, setDangerAction] = useState<DangerAction | null>(null)
  const [confirmText, setConfirmText] = useState('')
  const [dangerBusy, setDangerBusy] = useState(false)
  const [dangerError, setDangerError] = useState<string | null>(null)
  const [dangerNotice, setDangerNotice] = useState<string | null>(null)

  const composerId = useId()
  const confirmId = useId()

  const handleUnauthorizedOr = useCallback(
    (err: unknown, setter: (message: string) => void, fallback: string) => {
      if (err instanceof Unauthorized) {
        notifyUnauthorized()
        return
      }
      setter(describeError(err, fallback))
    },
    [notifyUnauthorized],
  )

  const reload = useCallback(async () => {
    try {
      const list = await memoryApi.list()
      setFacts(list)
      setLoadError(null)
    } catch (err) {
      handleUnauthorizedOr(err, setLoadError, 'Could not load memory.')
    }
  }, [handleUnauthorizedOr])

  useEffect(() => {
    reload()
  }, [reload])

  const handleAdd = async (event: FormEvent) => {
    event.preventDefault()
    const text = draft.trim()
    if (!text) return
    setAdding(true)
    setAddError(null)
    try {
      await memoryApi.add(text)
      setDraft('')
      await reload()
    } catch (err) {
      handleUnauthorizedOr(err, setAddError, 'Could not add that.')
    } finally {
      setAdding(false)
    }
  }

  const handleDeleteFact = async (id: number) => {
    setPendingDeleteId(id)
    setRowError(null)
    try {
      await memoryApi.remove(id)
      await reload()
    } catch (err) {
      handleUnauthorizedOr(err, setRowError, 'Could not delete that.')
    } finally {
      setPendingDeleteId(null)
    }
  }

  const startDanger = (action: DangerAction) => {
    setDangerAction(action)
    setConfirmText('')
    setDangerError(null)
    setDangerNotice(null)
  }

  const cancelDanger = () => {
    setDangerAction(null)
    setConfirmText('')
  }

  const runDanger = async (event: FormEvent) => {
    event.preventDefault()
    if (!dangerAction || confirmText !== CONFIRM_PHRASE[dangerAction]) return
    setDangerBusy(true)
    setDangerError(null)
    try {
      if (dangerAction === 'transcripts') {
        await memoryApi.clearConversations()
        setDangerNotice('Recorded conversations removed. Remembered facts stay.')
      } else {
        await memoryApi.clearEverything()
        setDangerNotice('Remembered facts and recorded conversations removed.')
      }
      setDangerAction(null)
      setConfirmText('')
      await reload()
    } catch (err) {
      handleUnauthorizedOr(err, setDangerError, 'That did not work.')
    } finally {
      setDangerBusy(false)
    }
  }

  return (
    <div className="memory">
      <form className="memory-composer" onSubmit={handleAdd}>
        <div className="field">
          <label htmlFor={composerId}>Add a standing instruction</label>
          <input
            id={composerId}
            type="text"
            value={draft}
            onChange={(event) => setDraft(event.target.value)}
            placeholder="e.g. I go by Kate, not my full name."
            disabled={adding}
          />
        </div>
        <button type="submit" disabled={adding || !draft.trim()}>
          {adding ? 'Adding…' : 'Add'}
        </button>
      </form>
      {addError && (
        <p className="settings-error" role="alert">
          {addError}
        </p>
      )}

      {loadError && (
        <p className="settings-error" role="alert">
          {loadError}
        </p>
      )}
      {rowError && (
        <p className="settings-error" role="alert">
          {rowError}
        </p>
      )}

      {facts === null ? (
        <p className="settings-empty">Loading memory…</p>
      ) : facts.length === 0 ? (
        <p className="settings-empty">Nothing remembered for this device yet.</p>
      ) : (
        <ul className="memory-list">
          {facts.map((fact) => (
            <li className="memory-item" key={fact.id}>
              <span className="memory-item-text">{fact.text}</span>
              <div className="memory-item-meta">
                <span className="memory-item-tag">{fact.source}</span>
                {/* The date is the only way to tell whose fact this is. The
                    deployed server accumulated facts from strangers who found
                    the URL before the token was set, and deciding what to
                    delete means knowing when it arrived. */}
                <span className="memory-item-date">{remembered(fact.created_at)}</span>
                <button
                  type="button"
                  onClick={() => handleDeleteFact(fact.id)}
                  disabled={pendingDeleteId === fact.id}
                >
                  {pendingDeleteId === fact.id ? 'Removing…' : 'Remove'}
                </button>
              </div>
            </li>
          ))}
        </ul>
      )}

      <div className="memory-danger-zone">
        <p className="settings-kicker">Delete data for this device</p>

        <div className="memory-danger-row">
          <div>
            <p className="settings-note">
              <strong>Delete transcripts.</strong> Removes recorded conversations. Remembered
              facts stay.
            </p>
          </div>
          <button type="button" onClick={() => startDanger('transcripts')} disabled={dangerBusy}>
            Delete transcripts
          </button>
        </div>

        <div className="memory-danger-row">
          <div>
            <p className="settings-note">
              <strong>Delete everything for this device.</strong> Removes remembered facts and
              recorded conversations. This cannot be undone.
            </p>
          </div>
          <button type="button" onClick={() => startDanger('everything')} disabled={dangerBusy}>
            Delete everything for this device
          </button>
        </div>

        {dangerAction && (
          <form className="memory-confirm" onSubmit={runDanger}>
            <div className="field">
              <label htmlFor={confirmId}>
                Type {CONFIRM_PHRASE[dangerAction]} to confirm
              </label>
              <input
                id={confirmId}
                type="text"
                value={confirmText}
                onChange={(event) => setConfirmText(event.target.value)}
                autoComplete="off"
                disabled={dangerBusy}
              />
            </div>
            <div className="role-editor-actions">
              <button
                type="submit"
                disabled={dangerBusy || confirmText !== CONFIRM_PHRASE[dangerAction]}
              >
                {dangerBusy ? 'Deleting…' : 'Confirm'}
              </button>
              <button type="button" onClick={cancelDanger} disabled={dangerBusy}>
                Cancel
              </button>
            </div>
          </form>
        )}

        {dangerError && (
          <p className="settings-error" role="alert">
            {dangerError}
          </p>
        )}
        {dangerNotice && <p className="chat-status">{dangerNotice}</p>}
      </div>
    </div>
  )
}

export default Memory
