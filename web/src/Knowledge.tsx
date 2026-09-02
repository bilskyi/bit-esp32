import { useCallback, useEffect, useId, useMemo, useState } from 'react'
import type { FormEvent } from 'react'
import { ApiError, Unauthorized } from './api'
import { memoryApi } from './settingsApi'
import type { Fact } from './settingsApi'

interface KnowledgeProps {
  notifyUnauthorized: () => void
}

type DangerAction = 'transcripts' | 'everything'

// Typed rather than clicked, because these two differ only in blast radius
// and a misclick between them is exactly the mistake a confirm dialog with
// two buttons invites. The phrase names the action, not a generic "yes" -
// ported from Memory.tsx, just said in the app's own language now.
const CONFIRM_PHRASE: Record<DangerAction, string> = {
  transcripts: 'ВИДАЛИТИ РОЗМОВИ',
  everything: 'ВИДАЛИТИ ВСЕ',
}

function describeError(err: unknown, fallback: string): string {
  return err instanceof ApiError ? err.detail : fallback
}

/** A remembered-on date, short and unambiguous. Returns the raw string if the
 * server ever sends something unparseable, rather than rendering "Invalid
 * Date" over a fact somebody is deciding whether to delete - same rule
 * Memory.tsx held, formatted as the mockup's own rows are (YYYY-MM-DD,
 * mono) so it fits the row's fixed 72px date column on one line instead of
 * wrapping the way "Aug 15, 2026" would. */
function remembered(value: string): string {
  const at = new Date(value)
  if (Number.isNaN(at.getTime())) return value
  const year = at.getFullYear()
  const month = String(at.getMonth() + 1).padStart(2, '0')
  const day = String(at.getDate()).padStart(2, '0')
  return `${year}-${month}-${day}`
}

/** Ukrainian has three plural forms where English has two. Used only for the
 * header line's own count - the fact and instruction rows themselves never
 * need it, since a row is always exactly one thing. */
function ukCount(n: number, one: string, few: string, many: string): string {
  const mod10 = n % 10
  const mod100 = n % 100
  if (mod10 === 1 && mod100 !== 11) return one
  if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) return few
  return many
}

interface FactRowProps {
  fact: Fact
  removing: boolean
  onRemove: () => void
}

function FactRow({ fact, removing, onRemove }: FactRowProps) {
  return (
    <div className="row">
      <span className="date">{remembered(fact.created_at)}</span>
      <span className="txt g">{fact.text}</span>
      <button type="button" className="btn xs ghost" onClick={onRemove} disabled={removing}>
        {removing ? 'Видаляю…' : 'Забути'}
      </button>
    </div>
  )
}

/** Знання: what the assistant knows about this device, in three groups that
 * are each visibly a different kind of thing - facts it extracted on its
 * own (GET /memory/default rows with source "auto"), instructions typed by
 * hand ("user"), and documents, which nothing on the server ingests yet.
 * Ported from Memory.tsx: the API calls, the re-read-after-every-mutation
 * rule, the typed-confirmation danger zone and its error handling all carry
 * over unchanged (see settingsApi.ts's own header comment on why a diff/
 * re-read beats an optimistic patch here). What changed is entirely the
 * markup and copy - the flat list becomes three grouped panels, the English
 * button labels become the app's own Ukrainian, and a third, honest group
 * exists where the mockup drew twelve files and a progress bar. */
function Knowledge({ notifyUnauthorized }: KnowledgeProps) {
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
      handleUnauthorizedOr(err, setLoadError, 'Не вдалося завантажити знання.')
    }
  }, [handleUnauthorizedOr])

  useEffect(() => {
    reload()
  }, [reload])

  const autoFacts = useMemo(() => facts?.filter((f) => f.source === 'auto') ?? null, [facts])
  const userFacts = useMemo(() => facts?.filter((f) => f.source === 'user') ?? null, [facts])

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
      handleUnauthorizedOr(err, setAddError, 'Не вдалося додати інструкцію.')
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
      handleUnauthorizedOr(err, setRowError, 'Не вдалося забути цей запис.')
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
        setDangerNotice('Записані розмови видалено. Факти лишились.')
      } else {
        await memoryApi.clearEverything()
        setDangerNotice('Факти й записані розмови видалено.')
      }
      setDangerAction(null)
      setConfirmText('')
      await reload()
    } catch (err) {
      handleUnauthorizedOr(err, setDangerError, 'Не вийшло.')
    } finally {
      setDangerBusy(false)
    }
  }

  const headline =
    facts === null
      ? 'Знання'
      : facts.length === 0
        ? 'Знання · поки нічого не записано'
        : `Знання · ${autoFacts?.length ?? 0} ${ukCount(autoFacts?.length ?? 0, 'факт', 'факти', 'фактів')} · ${userFacts?.length ?? 0} ${ukCount(userFacts?.length ?? 0, 'інструкція', 'інструкції', 'інструкцій')}`

  return (
    <section className="view">
      <div className="wrap">
        <div className="head">
          <div className="hl">
            <p className="lab">{headline}</p>
            <h1 style={{ fontSize: 18, marginTop: 2 }}>Усе, що я про тебе знаю.</h1>
            <p className="prose" style={{ fontSize: 13 }}>
              Факти я витягла сама з наших розмов; інструкції ти написав рукою. Для пошуку це
              одне й те саме — я міряю схожість і беру найближче. Документи приймати ще не вмію,
              і нижче написано, чому.
            </p>
          </div>
        </div>

        {loadError && (
          <p className="chat-status" role="alert">
            {loadError}
          </p>
        )}
        {rowError && (
          <p className="chat-status" role="alert">
            {rowError}
          </p>
        )}

        {facts === null ? (
          <p className="prose">Завантажую…</p>
        ) : (
          <div className="grid2">
            <div className="stack">
              {/* Facts it extracted on its own - GET /memory/default rows
                  tagged source: "auto". */}
              <div className="grp">
                <div className="grph">
                  <h2>Факти, які я витягла</h2>
                  <span className="pill">я сама</span>
                  {autoFacts && autoFacts.length > 0 && <span className="cnt">{autoFacts.length}</span>}
                </div>
                {autoFacts && autoFacts.length > 0 ? (
                  <div className="rows">
                    {autoFacts.map((fact) => (
                      <FactRow
                        key={fact.id}
                        fact={fact}
                        removing={pendingDeleteId === fact.id}
                        onRemove={() => handleDeleteFact(fact.id)}
                      />
                    ))}
                  </div>
                ) : (
                  <div className="empty">
                    <p className="prose" style={{ fontSize: 12 }}>
                      Ще нічого не витягнула сама з наших розмов.
                    </p>
                  </div>
                )}
              </div>

              {/* Instructions typed by hand - source: "user" rows, plus the
                  control that POSTs a new one. */}
              <div className="grp" style={{ marginTop: 20 }}>
                <div className="grph">
                  <h2>Інструкції, які ти дав</h2>
                  <span className="pill acc">твій голос</span>
                  {userFacts && userFacts.length > 0 && <span className="cnt">{userFacts.length}</span>}
                </div>
                {userFacts && userFacts.length > 0 ? (
                  <div className="rows">
                    {userFacts.map((fact) => (
                      <FactRow
                        key={fact.id}
                        fact={fact}
                        removing={pendingDeleteId === fact.id}
                        onRemove={() => handleDeleteFact(fact.id)}
                      />
                    ))}
                  </div>
                ) : (
                  <div className="empty">
                    <p className="prose" style={{ fontSize: 12 }}>
                      Ти ще не написав жодної інструкції.
                    </p>
                  </div>
                )}
                <form className="grp-add" onSubmit={handleAdd}>
                  <input
                    id={composerId}
                    type="text"
                    value={draft}
                    onChange={(event) => setDraft(event.target.value)}
                    placeholder="Наприклад: звертайся до мене на «ти»."
                    aria-label="Додати інструкцію"
                    disabled={adding}
                  />
                  <button type="submit" className="btn xs pri" disabled={adding || !draft.trim()}>
                    {adding ? 'Додаю…' : 'Додати'}
                  </button>
                </form>
                {addError && (
                  <p className="chat-status" role="alert">
                    {addError}
                  </p>
                )}
              </div>

              {/* Documents - the honest third group. Nothing on the server
                  accepts a file, chunks it or embeds it, so this never shows
                  a list, a fragment count or a progress bar - only what is
                  true today and what changes once it exists. This is the
                  plan's central constraint for this screen and it outranks
                  fidelity to the mockup, which drew twelve files here. */}
              <div className="grp" style={{ marginTop: 20 }}>
                <div className="grph">
                  <h2>Документи</h2>
                  <span className="pill warn">поки недоступно</span>
                </div>
                <div className="empty">
                  <p className="prose" style={{ fontSize: 12 }}>
                    Я не вмію приймати файли: немає ні місця для завантаження, ні коду, який ріже
                    їх на фрагменти й рахує для них ембединги. Знаю лише те, що сказано словами —
                    тут або в розмові.
                  </p>
                  <p className="prose" style={{ fontSize: 12, marginTop: 6 }}>
                    Коли це зʼявиться, файл ляже тут же, на сервері: поріжеться на фрагменти,
                    кожен стане вектором, і в пошуку працюватиме нарівні з фактами та
                    інструкціями.
                  </p>
                </div>
              </div>
            </div>
          </div>
        )}

        <div className="grp" style={{ marginTop: 24 }}>
          <p className="lab" style={{ color: 'var(--danger)' }}>
            Видалити дані для цього пристрою
          </p>
          <div className="rows" style={{ marginTop: 4 }}>
            <div className="row">
              <span className="txt g">
                <b>Видалити розмови.</b> Прибере записані розмови. Факти лишаться.
              </span>
              <button type="button" className="btn xs" onClick={() => startDanger('transcripts')} disabled={dangerBusy}>
                Видалити розмови
              </button>
            </div>
            <div className="row">
              <span className="txt g">
                <b>Видалити все для цього пристрою.</b> Прибере факти й записані розмови. Це
                незворотно.
              </span>
              <button type="button" className="btn xs" onClick={() => startDanger('everything')} disabled={dangerBusy}>
                Видалити все
              </button>
            </div>
          </div>

          {dangerAction && (
            <form className="field" onSubmit={runDanger}>
              <label htmlFor={confirmId}>Введи {CONFIRM_PHRASE[dangerAction]}, щоб підтвердити</label>
              <input
                id={confirmId}
                type="text"
                value={confirmText}
                onChange={(event) => setConfirmText(event.target.value)}
                autoComplete="off"
                disabled={dangerBusy}
              />
              <div className="acts" style={{ marginTop: 4 }}>
                <button
                  type="submit"
                  className="btn pri xs"
                  disabled={dangerBusy || confirmText !== CONFIRM_PHRASE[dangerAction]}
                >
                  {dangerBusy ? 'Видаляю…' : 'Підтвердити'}
                </button>
                <button type="button" className="btn xs ghost" onClick={cancelDanger} disabled={dangerBusy}>
                  Скасувати
                </button>
              </div>
            </form>
          )}

          {dangerError && (
            <p className="chat-status" role="alert">
              {dangerError}
            </p>
          )}
          {dangerNotice && <p className="chat-status">{dangerNotice}</p>}
        </div>
      </div>
    </section>
  )
}

export default Knowledge
