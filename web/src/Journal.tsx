import { useCallback, useEffect, useId, useMemo, useState } from 'react'
import { ApiError, Unauthorized } from './api'
import { appSettingsApi, journalApi } from './settingsApi'
import type { AppSettings, ConversationRow, Surface, UsageRow } from './settingsApi'
import type { Section } from './Shell.tsx'

interface JournalProps {
  onNavigate: (section: Section) => void
  notifyUnauthorized: () => void
}

function describeError(err: unknown, fallback: string): string {
  return err instanceof ApiError ? err.detail : fallback
}

/** Ukrainian has three plural forms where English has two. Duplicated from
 * Knowledge.tsx rather than shared - same call Devices.tsx already made
 * about lastEmotion: this task does not touch Knowledge.tsx to export it,
 * and a three-line pure function is cheaper to repeat than to plumb. */
function ukCount(n: number, one: string, few: string, many: string): string {
  const mod10 = n % 10
  const mod100 = n % 100
  if (mod10 === 1 && mod100 !== 11) return one
  if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) return few
  return many
}

const WEEKDAYS_UK = ['Нд', 'Пн', 'Вт', 'Ср', 'Чт', 'Пт', 'Сб']

function startOfDay(d: Date): Date {
  const c = new Date(d)
  c.setHours(0, 0, 0, 0)
  return c
}

function ymd(d: Date): string {
  const y = d.getFullYear()
  const m = String(d.getMonth() + 1).padStart(2, '0')
  const day = String(d.getDate()).padStart(2, '0')
  return `${y}-${m}-${day}`
}

function shortDate(d: Date): string {
  const m = String(d.getMonth() + 1).padStart(2, '0')
  const day = String(d.getDate()).padStart(2, '0')
  return `${day}.${m}`
}

function timeOf(iso: string): string {
  const d = new Date(iso)
  if (Number.isNaN(d.getTime())) return iso
  return `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`
}

function surfaceLabel(surface: Surface | string): string {
  if (surface === 'web') return 'браузер'
  if (surface === 'esp32') return 'робочий стіл'
  return surface
}

/** The first one or two real things that were actually said, not a
 * generated summary - server/memory/store.py's conversation_rows() returns
 * the transcript verbatim, and this is the only excerpt of it Журнал shows. */
function excerptOf(conv: ConversationRow): string {
  const said = conv.messages
    .filter((m) => m.role === 'user')
    .map((m) => m.text.trim())
    .filter(Boolean)
  if (said.length === 0) return 'без тексту в записі'
  const joined = said.slice(0, 2).join(' · ')
  return joined.length > 96 ? `${joined.slice(0, 96).trimEnd()}…` : joined
}

interface DayBucket {
  date: Date
  key: string
  questions: number
  conversations: number
}

/** Buckets real conversation rows into calendar days, for the "questions
 * per day" chart. This is the task's own honest-data trap: recording only
 * went on in production on 2 Sep 2026, so the window this returns is
 * bounded below by the earliest row that actually exists, never padded
 * back to a full week with zeroes that would look like quiet days rather
 * than "not recording yet". A day *inside* that real window with zero
 * turns is a true zero (recording was on, nothing happened) and is kept;
 * a day *before* the earliest row is simply not in the list at all. */
function questionsPerDay(rows: ConversationRow[]): DayBucket[] {
  if (rows.length === 0) return []
  const today = startOfDay(new Date())
  const earliestTime = Math.min(...rows.map((r) => startOfDay(new Date(r.started_at)).getTime()))
  const sevenAgo = startOfDay(new Date(today))
  sevenAgo.setDate(sevenAgo.getDate() - 6)
  const windowStart = earliestTime > sevenAgo.getTime() ? new Date(earliestTime) : sevenAgo
  const days: DayBucket[] = []
  for (const d = new Date(windowStart); d.getTime() <= today.getTime(); d.setDate(d.getDate() + 1)) {
    days.push({ date: new Date(d), key: ymd(d), questions: 0, conversations: 0 })
  }
  // windowStart is built from real data and capped at today, so this is
  // unreachable in practice - kept only so a clock skew between browser and
  // server cannot silently render an empty chart instead of one real point.
  if (days.length === 0) days.push({ date: today, key: ymd(today), questions: 0, conversations: 0 })
  const byKey = new Map(days.map((b) => [b.key, b]))
  for (const row of rows) {
    const bucket = byKey.get(ymd(startOfDay(new Date(row.started_at))))
    if (bucket) {
      bucket.questions += row.turns
      bucket.conversations += 1
    }
  }
  return days
}

interface RetentionPanelProps {
  appSettings: AppSettings | null
  appBusy: boolean
  appError: string | null
  appNotice: string | null
  retentionDraft: string | null
  onRetentionDraft: (value: string) => void
  onCommitRetention: () => void
  onToggleRecording: (checked: boolean) => void
  onNavigate: (section: Section) => void
}

/** The two facts that matter about retention, plus the control itself -
 * ported from Settings.tsx's own retention field (same "commit on blur/
 * Enter, not on every keystroke" reasoning: a PUT per digit would race). */
function RetentionPanel({
  appSettings, appBusy, appError, appNotice, retentionDraft,
  onRetentionDraft, onCommitRetention, onToggleRecording, onNavigate,
}: RetentionPanelProps) {
  const retentionId = useId()
  return (
    <div className="panel note pad">
      <p className="lab" style={{ color: 'var(--accent)', marginBottom: 6 }}>
        Запис
      </p>
      {appSettings === null ? (
        <p className="prose" style={{ fontSize: 12 }}>
          Завантажую…
        </p>
      ) : (
        <>
          <p className="prose" style={{ fontSize: 12 }}>
            {appSettings.retention_days === 0
              ? 'Розмови зберігаються без обмеження в часі — «0» означає назавжди.'
              : `Розмови зникають за ${appSettings.retention_days} ${ukCount(appSettings.retention_days, 'день', 'дні', 'днів')}.`}{' '}
            Але факти, які я з них витягла, лишаються — вони переживають саму розмову. Тому Знання
            варто час від часу переглядати.
          </p>
          <div className="field" style={{ marginTop: 10, maxWidth: 160 }}>
            <label className="flab" htmlFor={retentionId}>
              <span>зберігати, днів</span>
              <span className="c">0 = назавжди</span>
            </label>
            <input
              id={retentionId}
              type="number"
              min={0}
              step={1}
              value={retentionDraft ?? String(appSettings.retention_days)}
              onChange={(event) => onRetentionDraft(event.target.value)}
              onBlur={onCommitRetention}
              onKeyDown={(event) => {
                if (event.key === 'Enter') {
                  event.preventDefault()
                  onCommitRetention()
                }
              }}
              disabled={appBusy}
            />
          </div>
          <label className="check" style={{ marginTop: 10 }}>
            <input
              type="checkbox"
              checked={appSettings.store_conversations}
              onChange={(event) => onToggleRecording(event.target.checked)}
              disabled={appBusy}
            />
            <span>записувати розмови</span>
          </label>
          {appError && (
            <p className="chat-status" role="alert">
              {appError}
            </p>
          )}
          {appNotice && <p className="chat-status">{appNotice}</p>}
          <div className="acts" style={{ marginTop: 10 }}>
            <button type="button" className="btn xs" onClick={() => onNavigate('know')}>
              Відкрити знання
            </button>
          </div>
        </>
      )}
    </div>
  )
}

/** Журнал: the first screen this rebuild puts on top of Store.conversation_
 * rows and Store.usage_rows - both existed since well before this task,
 * neither had a route. Everything below is one of exactly four real
 * things: the recorded conversations themselves (surface, role, turn
 * count, a real excerpt of what was said), a questions-per-day chart
 * bucketed from those same rows (see questionsPerDay's own comment on why
 * its window can be shorter than a week and never is padded to look like
 * one), a running total of tokens logged per session (deliberately not
 * rendered as a fraction of Groq's 6000-tokens-per-minute ceiling - that
 * ceiling is a rate that resets every minute, and usage_rows is one total
 * per session, so no honest percentage of it can be computed from these
 * rows), and the retention setting itself, editable here. If both
 * endpoints come back empty, none of that is shown as zeroes - the section
 * says plainly that nothing has been recorded yet. */
function Journal({ onNavigate, notifyUnauthorized }: JournalProps) {
  const [conversations, setConversations] = useState<ConversationRow[] | null>(null)
  const [usage, setUsage] = useState<UsageRow[] | null>(null)
  const [appSettings, setAppSettings] = useState<AppSettings | null>(null)
  const [loadError, setLoadError] = useState<string | null>(null)

  const [appBusy, setAppBusy] = useState(false)
  const [appError, setAppError] = useState<string | null>(null)
  const [appNotice, setAppNotice] = useState<string | null>(null)
  const [retentionDraft, setRetentionDraft] = useState<string | null>(null)

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

  const reloadConversations = useCallback(async () => {
    try {
      setConversations(await journalApi.conversations())
    } catch (err) {
      onUnauthorizedOr(err, setLoadError, 'Не вдалося завантажити розмови.')
    }
  }, [onUnauthorizedOr])

  const reloadUsage = useCallback(async () => {
    try {
      setUsage(await journalApi.usage())
    } catch (err) {
      onUnauthorizedOr(err, setLoadError, 'Не вдалося завантажити використання.')
    }
  }, [onUnauthorizedOr])

  const reloadAppSettings = useCallback(async () => {
    try {
      setAppSettings(await appSettingsApi.get())
    } catch (err) {
      onUnauthorizedOr(err, setLoadError, 'Не вдалося завантажити налаштування запису.')
    }
  }, [onUnauthorizedOr])

  useEffect(() => {
    // useEffect's own callback cannot be async; the nested async function is
    // this app's standing idiom for "fire independent GETs on mount" (see
    // Devices.tsx/Personality.tsx's own identical comment).
    async function load() {
      await Promise.all([reloadConversations(), reloadUsage(), reloadAppSettings()])
    }
    load()
  }, [reloadConversations, reloadUsage, reloadAppSettings])

  const handleAppSettingsChange = async (patch: Partial<AppSettings>) => {
    setAppBusy(true)
    setAppError(null)
    setAppNotice(null)
    try {
      setAppSettings(await appSettingsApi.set(patch))
      setAppNotice('Збережено.')
    } catch (err) {
      onUnauthorizedOr(err, setAppError, 'Не вдалося зберегти.')
      // The control the person just touched otherwise looks like it took
      // even though the server rejected it - re-read to show the truth.
      await reloadAppSettings()
    } finally {
      setAppBusy(false)
    }
  }

  const commitRetention = () => {
    if (retentionDraft === null) return
    const raw = Number(retentionDraft)
    const days = Number.isFinite(raw) ? Math.max(0, Math.round(raw)) : 0
    setRetentionDraft(null)
    if (appSettings && days === appSettings.retention_days) return
    void handleAppSettingsChange({ retention_days: days })
  }

  const days = useMemo(() => questionsPerDay(conversations ?? []), [conversations])
  const daysShown = days.length
  const maxValue = Math.max(1, ...days.map((d) => d.questions))
  const totalQuestions = days.reduce((sum, d) => sum + d.questions, 0)
  const totalConvInWindow = days.reduce((sum, d) => sum + d.conversations, 0)
  // A day counts as "quiet" only once it is over - the last column is
  // always today, still in progress, so a zero there is not evidence of a
  // quiet day yet.
  const quietDay = days.slice(0, -1).find((d) => d.questions === 0)

  const totalPromptTokens = useMemo(
    () => (usage ?? []).reduce((sum, u) => sum + u.prompt_tokens, 0),
    [usage],
  )
  const totalCompletionTokens = useMemo(
    () => (usage ?? []).reduce((sum, u) => sum + u.completion_tokens, 0),
    [usage],
  )
  const totalTokens = totalPromptTokens + totalCompletionTokens

  const loaded = conversations !== null && usage !== null
  const nothingAtAll = loaded && conversations.length === 0 && usage.length === 0

  return (
    <section className="view">
      <div className="wrap">
        <div className="head">
          <div className="hl">
            <p className="lab">Журнал · розмови, питання й токени</p>
            <h1 style={{ fontSize: 18, marginTop: 2 }}>Скільки я працювала.</h1>
            <p className="prose" style={{ fontSize: 13 }}>
              Розмови, які справді записано, і токени, які на них пішли. Тут я не округляю на
              свою користь.
            </p>
          </div>
        </div>

        {loadError && (
          <p className="chat-status" role="alert">
            {loadError}
          </p>
        )}

        {!loaded ? (
          <p className="prose">Завантажую…</p>
        ) : nothingAtAll ? (
          <>
            <div className="empty">
              <p className="prose">
                Ще жодної розмови не записано, і жодної сесії не пораховано. Спитай мене голосом
                чи текстом — і перший запис зʼявиться тут.
              </p>
            </div>
            <div style={{ maxWidth: 420, marginTop: 20 }}>
              <RetentionPanel
                appSettings={appSettings}
                appBusy={appBusy}
                appError={appError}
                appNotice={appNotice}
                retentionDraft={retentionDraft}
                onRetentionDraft={setRetentionDraft}
                onCommitRetention={commitRetention}
                onToggleRecording={(checked) => handleAppSettingsChange({ store_conversations: checked })}
                onNavigate={onNavigate}
              />
            </div>
          </>
        ) : (
          <div className="grid2">
            <div className="stack">
              <div className="panel">
                <div className="ph">
                  <h2>Питань за день</h2>
                  <span className="cnt">
                    {daysShown >= 7 ? 'останні 7 днів' : `${daysShown} ${ukCount(daysShown, 'день', 'дні', 'днів')} запису`}
                  </span>
                </div>
                <div className="pad">
                  {daysShown === 0 ? (
                    <div className="empty" style={{ padding: '4px 0' }}>
                      <p className="prose" style={{ fontSize: 12 }}>
                        Ще немає жодної розмови — графік зʼявиться, коли буде що рахувати.
                      </p>
                    </div>
                  ) : (
                    <>
                      <div className="chartrow">
                        <div className="chart">
                          <div className="yax" aria-hidden="true">
                            <span style={{ bottom: '100%' }}>{maxValue}</span>
                            <span style={{ bottom: '50%' }}>{Math.round(maxValue / 2)}</span>
                            <span style={{ bottom: 0 }}>0</span>
                          </div>
                          <div style={{ minWidth: 0 }}>
                            <div className="plot">
                              <span className="gl" style={{ bottom: '100%' }} />
                              <span className="gl" style={{ bottom: '50%' }} />
                              <div
                                className="cols"
                                role="img"
                                aria-label={`Питань за день: ${days
                                  .map((d, i) => `${i === days.length - 1 ? 'сьогодні' : shortDate(d.date)} — ${d.questions}`)
                                  .join(', ')}`}
                              >
                                {days.map((d, i) => {
                                  const isToday = i === days.length - 1
                                  const isZero = d.questions === 0
                                  return (
                                    <span
                                      key={d.key}
                                      className={`col${isZero ? ' zero' : ''}${isToday ? ' now' : ''}`}
                                    >
                                      <b className="v">{d.questions}</b>
                                      {isZero ? <i /> : <i style={{ height: `${(d.questions / maxValue) * 100}%` }} />}
                                    </span>
                                  )
                                })}
                              </div>
                            </div>
                            <div className="xax">
                              {days.map((d, i) => {
                                const isToday = i === days.length - 1
                                return (
                                  <span key={d.key} className={isToday ? 'now' : undefined}>
                                    {WEEKDAYS_UK[d.date.getDay()]}
                                    <em>{isToday ? 'сьогодні' : shortDate(d.date)}</em>
                                  </span>
                                )
                              })}
                            </div>
                          </div>
                        </div>
                        <div className="kv bare" style={{ gridTemplateColumns: 'minmax(0,1fr)', alignSelf: 'start' }}>
                          <div>
                            <span className="k">
                              {daysShown >= 7 ? 'за тиждень' : `за ${daysShown} ${ukCount(daysShown, 'день', 'дні', 'днів')}`}
                            </span>
                            <span className="v">
                              {totalQuestions} {ukCount(totalQuestions, 'питання', 'питання', 'питань')}
                            </span>
                          </div>
                          <div>
                            <span className="k">розмов</span>
                            <span className="v">{totalConvInWindow}</span>
                          </div>
                          {quietDay && (
                            <div>
                              <span className="k">тихий день</span>
                              <span className="v no">{shortDate(quietDay.date)} — 0</span>
                            </div>
                          )}
                        </div>
                      </div>
                      {daysShown < 7 && (
                        <p className="prose" style={{ fontSize: 12, marginTop: 10 }}>
                          Записую питання лише {daysShown} {ukCount(daysShown, 'день', 'дні', 'днів')} — почала{' '}
                          {shortDate(days[0].date)}. Коли назбирається більше, графік покаже цілий тиждень, а не
                          дні, домальовані нулями.
                        </p>
                      )}
                    </>
                  )}
                </div>
              </div>

              <div className="grp" style={{ marginTop: 20 }}>
                <div className="grph">
                  <h2>Останні розмови</h2>
                  <span className="cnt">
                    {conversations.length} {ukCount(conversations.length, 'розмова', 'розмови', 'розмов')}
                    {appSettings
                      ? ` · зберігаються ${appSettings.retention_days === 0 ? 'назавжди' : `${appSettings.retention_days} ${ukCount(appSettings.retention_days, 'день', 'дні', 'днів')}`}`
                      : ''}
                  </span>
                </div>
                {conversations.length > 0 ? (
                  <div className="rows">
                    {conversations.map((conv) => (
                      <div className="row" key={conv.id}>
                        <span className="date">{shortDate(new Date(conv.started_at))}</span>
                        <span className="txt g">
                          {surfaceLabel(conv.surface)}
                          <span className="sub">
                            {timeOf(conv.started_at)} · {excerptOf(conv)}
                          </span>
                        </span>
                        <span className="pill">
                          {conv.turns} {ukCount(conv.turns, 'репліка', 'репліки', 'реплік')}
                        </span>
                      </div>
                    ))}
                  </div>
                ) : (
                  <div className="empty">
                    <p className="prose" style={{ fontSize: 12 }}>
                      Ще жодної розмови не записано.
                    </p>
                  </div>
                )}
              </div>
            </div>

            <div className="stack">
              <div className="grp">
                <p className="lab" style={{ marginBottom: 8 }}>
                  Токени проти безкоштовного ліміту
                </p>
                {usage.length === 0 ? (
                  <div className="empty" style={{ padding: '4px 0' }}>
                    <p className="prose" style={{ fontSize: 12 }}>
                      Ще жодної сесії не пораховано — нічого підсумовувати.
                    </p>
                  </div>
                ) : (
                  <>
                    <div className="kv bare" style={{ gridTemplateColumns: 'repeat(2,minmax(0,1fr))' }}>
                      <div>
                        <span className="k">токенів разом</span>
                        <span className="v">{totalTokens.toLocaleString('uk-UA')}</span>
                      </div>
                      <div>
                        <span className="k">сесій пораховано</span>
                        <span className="v">{usage.length}</span>
                      </div>
                    </div>
                    <p className="prose" style={{ fontSize: 12, marginTop: 8 }}>
                      {totalPromptTokens.toLocaleString('uk-UA')} запит + {totalCompletionTokens.toLocaleString('uk-UA')}{' '}
                      відповідь, за весь час, відколи рахую. Ліміт Groq — 6000 токенів на хвилину: це швидкість,
                      яка щохвилини обнуляється, а не денний бюджет. Ці рядки підсумовані за сесію, а не за
                      хвилину, тож чесно порахувати частку від хвилинного ліміту з них не вийде — я скажу,
                      коли справді впрусь у стелю, а не намалюю відсоток, який нічого не означає.
                    </p>
                  </>
                )}
              </div>

              <RetentionPanel
                appSettings={appSettings}
                appBusy={appBusy}
                appError={appError}
                appNotice={appNotice}
                retentionDraft={retentionDraft}
                onRetentionDraft={setRetentionDraft}
                onCommitRetention={commitRetention}
                onToggleRecording={(checked) => handleAppSettingsChange({ store_conversations: checked })}
                onNavigate={onNavigate}
              />
            </div>
          </div>
        )}
      </div>
    </section>
  )
}

export default Journal
