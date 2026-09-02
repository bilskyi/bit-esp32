// Головна: the front door. Everything here is read-only except the ask
// control itself (which hands off to Розмова via the shared useTurn()
// instance) - there is no delete, no confirm, no PUT on this screen. What
// it shows is composed entirely from four real GETs (memory, conversations,
// usage, surfaces) plus useSession()'s own username; nothing here is drawn
// because the mockup drew it. See the plan's honest-data table and
// Знання/Зʼєднання/Пристрої's own comments for the sections this screen
// has to agree with rather than contradict.
import { useCallback, useEffect, useMemo, useState } from 'react'
import type { FormEvent, RefObject } from 'react'
import { ApiError, Unauthorized, useSession } from './api'
import Face from './Face.tsx'
import Mic from './Mic.tsx'
import { journalApi, memoryApi, surfacesApi } from './settingsApi'
import type { ConversationRow, Fact, Role, Surface, UsageRow } from './settingsApi'
import type { Section } from './Shell.tsx'
import type { Turn, TurnValue } from './useTurn.ts'
import type { ReactNode } from 'react'

interface HomeProps {
  /** The one useTurn() instance App.tsx keeps alive for the whole signed-in
   * session - same instance Chat.tsx and Devices.tsx already share. Asking
   * from here sends on that same socket, so the reply is already streaming
   * by the time onNavigate lands the person on Розмова. */
  turn: TurnValue
  onNavigate: (section: Section) => void
  notifyUnauthorized: () => void
  /** App.tsx's own ref to this screen's ask input, so ⌘K (handled in
   * Shell.tsx, which has no access to this component's DOM) can focus it
   * after switching here - see App.tsx's focusHomeAsk for the handoff. */
  askInputRef: RefObject<HTMLInputElement | null>
}

function describeError(err: unknown, fallback: string): string {
  return err instanceof ApiError ? err.detail : fallback
}

/** Ukrainian has three plural forms where English has two - duplicated from
 * Journal.tsx/Knowledge.tsx rather than shared, same call those two already
 * made about their own small pure helpers. */
function ukCount(n: number, one: string, few: string, many: string): string {
  const mod10 = n % 10
  const mod100 = n % 100
  if (mod10 === 1 && mod100 !== 11) return one
  if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) return few
  return many
}

function timeOf(iso: string): string {
  const d = new Date(iso)
  if (Number.isNaN(d.getTime())) return iso
  return `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`
}

function shortDate(d: Date): string {
  const m = String(d.getMonth() + 1).padStart(2, '0')
  const day = String(d.getDate()).padStart(2, '0')
  return `${day}.${m}`
}

/** The most recent emotion any turn actually got tagged with - the same
 * search Chat.tsx and Devices.tsx already run (duplicated rather than
 * imported, same call they made: a fresh question starts with
 * emotion: null until its own frame lands, and the face should hold the
 * previous mood through listening/thinking rather than snap to neutral). */
function lastEmotion(turns: Turn[]): string | null {
  for (let i = turns.length - 1; i >= 0; i--) {
    const emotion = turns[i].emotion
    if (emotion) return emotion
  }
  return null
}

function sameDay(a: Date, b: Date): boolean {
  return a.getFullYear() === b.getFullYear() && a.getMonth() === b.getMonth() && a.getDate() === b.getDate()
}

/** A parseable time, or null - every timestamp on this screen comes from the
 * server, but a comparison against an unparseable one must lose that
 * candidate rather than poison a Math.min/max with NaN. */
function parseTime(iso: string): number | null {
  const t = new Date(iso).getTime()
  return Number.isNaN(t) ? null : t
}

/** Доброго ранку / дня / вечора / ночі - picked from the browser's own
 * clock, the same clock the person is reading this screen by. */
function greeting(hour: number, name: string): string {
  const part = hour >= 5 && hour < 12 ? 'ранку' : hour >= 12 && hour < 18 ? 'дня' : hour >= 18 && hour < 23 ? 'вечора' : 'ночі'
  return `Доброго ${part}, ${name}.`
}

// ------------------------------------------------------------- figures line

interface Figures {
  /** "сьогодні, 14:02" if the most recent usage row is from today, else
   * "28.08, 09:15" - always real, always the newest row that exists. */
  pillLabel: string
  /** Only ever a real, non-zero total across every usage row logged on the
   * same calendar day as the most recent one - never a zero standing in for
   * "nothing happened", which is instead simply left out of `parts`. */
  parts: string[]
}

/** «Відколи ми говорили»: what changed around the most recent time this
 * assistant was actually used. Usage rows are the source, not conversation
 * rows - session.py logs one on every session regardless of whether
 * recording is on (see Journal.tsx's own comment: usage "has been logging
 * since long before recording did"), so this stays honest even with
 * recording switched off, when conversation rows would not exist at all.
 *
 * The window is "every usage row from the same calendar day as the latest
 * one", not a fixed "since the previous session" delta: a delta would need
 * a second, earlier usage row to subtract from, is fragile the moment facts
 * and usage rows land within milliseconds of each other at the end of the
 * very same session (see finish() in session.py - both are written in the
 * same call), and cannot explain a day with several short sessions the way
 * the approved mockup's own two-conversation example does. A calendar day
 * has none of those problems and reads exactly as "since we last spoke"
 * when the last session was today.
 *
 * "перша відповідь за 335 ms" from the mockup is dropped entirely and on
 * purpose: no endpoint this screen may call persists a latency figure
 * anywhere outside a single live Trace (useTurn.ts's own, gone the moment
 * the turn scrolls out of memory) - there is nothing honest to show here.
 */
function computeFigures(usage: UsageRow[], facts: Fact[]): Figures | null {
  if (usage.length === 0) return null
  const sorted = [...usage]
    .map((u) => ({ row: u, at: parseTime(u.created_at) }))
    .filter((u): u is { row: UsageRow; at: number } => u.at !== null)
    .sort((a, b) => b.at - a.at)
  if (sorted.length === 0) return null
  const last = sorted[0]
  const lastAt = new Date(last.at)
  const now = new Date()
  const pillLabel = sameDay(lastAt, now) ? `сьогодні, ${timeOf(last.row.created_at)}` : `${shortDate(lastAt)}, ${timeOf(last.row.created_at)}`

  const dayRows = sorted.filter((u) => sameDay(new Date(u.at), lastAt)).map((u) => u.row)
  const questions = dayRows.reduce((sum, u) => sum + u.turns, 0)
  const tokens = dayRows.reduce((sum, u) => sum + u.prompt_tokens + u.completion_tokens, 0)
  const newFacts = facts.filter((f) => {
    const at = parseTime(f.created_at)
    return at !== null && sameDay(new Date(at), lastAt)
  }).length

  const parts: string[] = []
  if (newFacts > 0) parts.push(`${newFacts} ${ukCount(newFacts, 'новий факт', 'нові факти', 'нових фактів')}`)
  if (questions > 0) parts.push(`${questions} ${ukCount(questions, 'питання', 'питання', 'питань')}`)
  if (tokens > 0) parts.push(`${tokens.toLocaleString('uk-UA')} токенів`)

  return { pillLabel, parts }
}

// ------------------------------------------------------------- waiting item

interface Waiting {
  count: number
  baselineLabel: string
}

/** «Одне чекає на тебе»: auto-extracted facts older than the earliest
 * conversation this app has a record of.
 *
 * The baseline is the earliest CONVERSATION row, and deliberately not the
 * earliest usage row. Usage is logged unconditionally for every session -
 * including the sessions of the strangers who found the deployed URL before
 * DEVICE_TOKEN was set - so the earliest usage row is the earliest activity
 * by *anyone*, which on the real volume predates every fact and makes this
 * card mathematically unable to ever fire. Checked against production: usage
 * reaches back past 27 Aug, the suspect facts are 27-28 Aug, so a
 * usage-based baseline flagged nothing. Conversation recording only began on
 * 2 Sep, so "older than the earliest conversation on record" is a statement
 * this app can actually stand behind.
 *
 * Only source: "auto" facts are candidates. A source: "user" row exists
 * because somebody with this account's credentials typed it, which is not
 * the story this card tells.
 *
 * It says what it measured rather than who it blames: the app cannot know
 * whose a fact is - device_id is "default" for everyone - so the copy states
 * the date basis and leaves the judgement to the person. With no
 * conversations recorded at all there is no "before" and the card does not
 * appear.
 */
function computeWaiting(facts: Fact[], conversations: ConversationRow[]): Waiting | null {
  const times = conversations
    .map((c) => parseTime(c.started_at))
    .filter((t): t is number => t !== null)
  if (times.length === 0) return null
  const earliest = Math.min(...times)

  const stray = facts.filter((f) => {
    if (f.source !== 'auto') return false
    const at = parseTime(f.created_at)
    return at !== null && at < earliest
  })
  if (stray.length === 0) return null
  return { count: stray.length, baselineLabel: shortDate(new Date(earliest)) }
}

// -------------------------------------------------------------- talked line

/** The most recent conversation per surface, phrased the way a person would
 * say it out loud - the one quiet line this screen keeps pointed at
 * Журнал. Real conversation rows only (recording can be off, in which case
 * this is simply absent - never backfilled from usage rows, which do not
 * carry a surface). */
function latestBySurface(conversations: ConversationRow[], surface: Surface): ConversationRow | null {
  let best: ConversationRow | null = null
  let bestAt = -Infinity
  for (const c of conversations) {
    if (c.surface !== surface) continue
    const at = parseTime(c.started_at)
    if (at !== null && at > bestAt) {
      best = c
      bestAt = at
    }
  }
  return best
}

function Home({ turn, onNavigate, notifyUnauthorized, askInputRef }: HomeProps) {
  const { username } = useSession()
  const [facts, setFacts] = useState<Fact[] | null>(null)
  const [conversations, setConversations] = useState<ConversationRow[] | null>(null)
  const [usage, setUsage] = useState<UsageRow[] | null>(null)
  const [surfaces, setSurfaces] = useState<Record<Surface, Role> | null>(null)
  const [loadError, setLoadError] = useState<string | null>(null)
  const [waitingDismissed, setWaitingDismissed] = useState(false)

  const [draft, setDraft] = useState('')

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

  useEffect(() => {
    let alive = true
    async function load() {
      const results = await Promise.allSettled([
        memoryApi.list(),
        journalApi.conversations(),
        journalApi.usage(),
        surfacesApi.get(),
      ])
      if (!alive) return
      const [factsRes, convRes, usageRes, surfacesRes] = results
      if (factsRes.status === 'fulfilled') setFacts(factsRes.value)
      if (convRes.status === 'fulfilled') setConversations(convRes.value)
      if (usageRes.status === 'fulfilled') setUsage(usageRes.value)
      if (surfacesRes.status === 'fulfilled') setSurfaces(surfacesRes.value)
      const firstError = results.find((r) => r.status === 'rejected') as PromiseRejectedResult | undefined
      if (firstError) onUnauthorizedOr(firstError.reason, setLoadError, 'Не вдалося завантажити головну.')
    }
    load()
    return () => {
      alive = false
    }
  }, [onUnauthorizedOr])

  const loaded = facts !== null && conversations !== null && usage !== null && surfaces !== null

  const autoFacts = useMemo(() => facts?.filter((f) => f.source === 'auto') ?? [], [facts])
  const userFacts = useMemo(() => facts?.filter((f) => f.source === 'user') ?? [], [facts])
  const knownAnything = autoFacts.length > 0 || userFacts.length > 0

  const figures = useMemo(() => (facts && usage ? computeFigures(usage, facts) : null), [facts, usage])
  const waiting = useMemo(
    () => (facts && conversations ? computeWaiting(facts, conversations) : null),
    [facts, conversations, usage],
  )
  const showWaiting = waiting !== null && !waitingDismissed

  const webConv = conversations ? latestBySurface(conversations, 'web') : null
  const deviceConv = conversations ? latestBySurface(conversations, 'esp32') : null

  const disabled = turn.connection.status !== 'open'

  const submit = (event: FormEvent) => {
    event.preventDefault()
    const text = draft.trim()
    if (!text || disabled) return
    // ask() can still refuse (see its comment in useTurn.ts) - leave the
    // draft in place rather than clear a question that never sent.
    if (!turn.ask(text)) return
    setDraft('')
    onNavigate('talk')
  }

  const applyChip = (text: string) => {
    setDraft(text)
    askInputRef.current?.focus()
  }

  const hour = new Date().getHours()
  const name = username ?? ''

  const herLine = !loaded
    ? 'Секунду — дивлюсь, що нового.'
    : !knownAnything
      ? 'Я ще нічого про тебе не знаю — жодного факту, жодної інструкції. Спитай щось, і почнемо.'
      : showWaiting
        ? 'Я не спала. Дещо запам’ятала — і є одна річ, яку хочу спитати, перш ніж вважати її твоєю.'
        : 'Я не спала. Памʼятаю все, про що ми говорили, і нічого нового не чекає на перевірку.'

  const memorySub = !knownAnything
    ? 'поки нічого не знаю'
    : [
        autoFacts.length > 0 ? `${autoFacts.length} ${ukCount(autoFacts.length, 'факт', 'факти', 'фактів')}` : null,
        userFacts.length > 0 ? `${userFacts.length} ${ukCount(userFacts.length, 'інструкція', 'інструкції', 'інструкцій')}` : null,
      ]
        .filter(Boolean)
        .join(' · ')

  const talkedParts: string[] = []
  if (webConv) talkedParts.push(`${timeOf(webConv.started_at)} у браузері`)
  if (deviceConv) talkedParts.push(`${timeOf(deviceConv.started_at)} голосом зі столу`)

  // Sibling <span>s, matching the mockup's own flat structure exactly, so
  // .since's flex `gap` puts even space around every value and separator -
  // nesting the "·" inside each value's own span would halve the gaps
  // meant to surround it.
  const figureNodes: ReactNode[] = []
  figures?.parts.forEach((part, i) => {
    if (i > 0) figureNodes.push(<span key={`sep-${i}`} className="s">·</span>)
    figureNodes.push(
      <span key={`val-${i}`}>
        <b>{part}</b>
      </span>,
    )
  })

  return (
    <section className="view" id="v-home">
      <div className="wrap">
        <div className="head">
          <div className="hl">
            <h1>{greeting(hour, name)}</h1>
            <p className="prose">{herLine}</p>
          </div>
          <div className="acts">
            <button type="button" className="btn" onClick={() => onNavigate('talk')}>
              Відкрити розмову
            </button>
          </div>
        </div>

        {figures && figures.parts.length > 0 && (
          <div className="since">
            <span className="pill mono">
              <span className="dot on" aria-hidden="true" />
              {figures.pillLabel}
            </span>
            <span>відколи ми говорили:</span>
            {figureNodes}
          </div>
        )}

        <form className="ask" id="askForm" autoComplete="off" onSubmit={submit}>
          <span className="pre" aria-hidden="true">
            &gt;
          </span>
          <input
            ref={askInputRef}
            type="text"
            id="askInput"
            className="home-ask-input"
            value={draft}
            onChange={(event) => setDraft(event.target.value)}
            placeholder="Запитай про будь-що…"
            aria-label="Запитати асистента"
            disabled={disabled}
          />
          <span className="kbd" aria-hidden="true" style={{ marginRight: 2 }}>
            ⌘K
          </span>
          <Mic turn={turn} />
          <button type="submit" className="btn pri" disabled={disabled || !draft.trim()} aria-label="Надіслати питання">
            Спитати
          </button>
        </form>
        {disabled && <p className="chat-status">{turn.connection.reason}</p>}
        <div className="chips">
          <button type="button" className="chip" onClick={() => applyChip('Що ти про мене знаєш?')}>
            Що ти про мене знаєш?
          </button>
          <button type="button" className="chip" onClick={() => applyChip('Розкажи щось коротке про космос.')}>
            Розкажи щось коротке про космос.
          </button>
        </div>

        {loadError && (
          <p className="chat-status" role="alert">
            {loadError}
          </p>
        )}

        {!loaded ? (
          <p className="prose" style={{ marginTop: 24 }}>
            Завантажую…
          </p>
        ) : (
          <div className={`home-two${showWaiting ? '' : ' no-note'}`}>
            {showWaiting && waiting && (
              <div className="panel note pad16">
                <p className="lab" style={{ color: 'var(--accent)', marginBottom: 6 }}>
                  Одне чекає на тебе
                </p>
                <p className="prose" style={{ fontSize: 13 }}>
                  {waiting.count} {ukCount(waiting.count, 'автоматично витягнутий факт', 'автоматично витягнуті факти', 'автоматично витягнутих фактів')}{' '}
                  старші за {waiting.baselineLabel} — за найдавнішу розмову, яку я записала. Звідки вони, я
                  сказати не можу: до того дня я нічого не зберігала. Подивишся?
                </p>
                <div className="acts" style={{ marginTop: 10 }}>
                  <button type="button" className="btn pri" onClick={() => onNavigate('know')}>
                    Показати {waiting.count}
                  </button>
                  <button type="button" className="btn" onClick={() => setWaitingDismissed(true)}>
                    Це моє, лишай
                  </button>
                </div>
              </div>
            )}

            <button type="button" className="grp live home-live" onClick={() => onNavigate('dev')} aria-label="Робочий стіл: живий екран пристрою">
              <p className="lab" style={{ marginBottom: 6 }}>
                Живий екран
              </p>
              <Face state={turn.state} emotion={lastEmotion(turn.turns)} online={turn.connection.status === 'open'} />
            </button>

            <div className="grp">
              <div className="grph">
                <h2>Куди я дістаю</h2>
                <span className="cnt">памʼять одна на всі поверхні</span>
              </div>
              <div className="rows">
                <button type="button" className="row" onClick={() => onNavigate('know')}>
                  <span className="g">
                    <span className="txt">Памʼять</span>
                    <span className="sub">{memorySub}</span>
                  </span>
                  {knownAnything ? (
                    <span className="pill ok">
                      <span className="dot on" aria-hidden="true" />у пошуку
                    </span>
                  ) : (
                    <span className="pill">
                      <span className="dot off" aria-hidden="true" />
                      порожньо
                    </span>
                  )}
                </button>
                <button type="button" className="row" onClick={() => onNavigate('know')}>
                  <span className="g">
                    <span className="txt">Документи</span>
                    <span className="sub">не приймаю файлів</span>
                  </span>
                  <span className="pill warn">поки недоступно</span>
                </button>
                <button type="button" className="row" onClick={() => onNavigate('conn')}>
                  <span className="g">
                    <span className="txt">Зʼєднання</span>
                    <span className="sub">нічого не підключено</span>
                  </span>
                  <span className="pill">0</span>
                </button>
                <button type="button" className="row" onClick={() => onNavigate('dev')}>
                  <span className="g">
                    <span className="txt">Робочий стіл</span>
                    <span className="sub">ESP32-C3 · persona {surfaces ? surfaces.esp32.name : '…'}</span>
                  </span>
                  <span className="pill" title="Немає heartbeat - нічого ще не звітує стан">
                    <span className="dot off" aria-hidden="true" />
                    стан невідомий
                  </span>
                </button>
              </div>
              <div className="foot">
                <span>{talkedParts.length > 0 ? <>говорили {talkedParts.join(', ')}</> : 'ще жодної розмови не записано'}</span>
                <span className="acts">
                  <button type="button" className="btn xs ghost" onClick={() => onNavigate('act')}>
                    Журнал
                  </button>
                </span>
              </div>
            </div>
          </div>
        )}
      </div>
    </section>
  )
}

export default Home
