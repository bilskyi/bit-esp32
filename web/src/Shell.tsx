// The app frame: the rail (seven sections, the "Поверхні" surfaces, the
// free-tier meter), and the top bar (breadcrumb, status pill, theme toggle,
// sign out). Section state is owned by App.tsx and handed down as a plain
// prop - there is no router here on purpose. server/main.py's SPA fallback
// returns a JSON 404 for any path whose first segment matches an API prefix
// ("settings", "roles", "memory", "conversations" all are one), so a client
// route at e.g. /roles would 404 on reload. Keeping the current section in
// React state instead of the URL sidesteps that entirely.
//
// The status pill reads useTurn()'s own `connection`/`state` - App.tsx is
// the one place that calls useTurn(), same as before this rebuild ("one
// socket owned above the tabs"), and just passes the two fields down. This
// file never invents a connection state of its own.

import { useCallback, useEffect, useRef, useState } from 'react'
import type { ReactNode } from 'react'
import { Unauthorized, useSession } from './api'
import { memoryApi, rolesApi } from './settingsApi'
import type { Connection, ConversationState } from './useTurn'

export type Section = 'home' | 'talk' | 'know' | 'conn' | 'dev' | 'pers' | 'act'

interface SectionInfo {
  id: Section
  label: string
  /** The letter that follows "g" in the g-then-key shortcut, and the second
   * kbd hint shown on hover - always the section label's first letter,
   * transliterated (к for Знання would collide with talk's "т", hence the
   * mockup's own h/t/k/c/d/p/a mapping, kept verbatim here). */
  key: string
}

// Order matches the rail, the bottom tab bar and the g-then-key map in the
// approved mockup exactly - h/t/k/c/d/p/a.
const SECTIONS: SectionInfo[] = [
  { id: 'home', label: 'Головна', key: 'h' },
  { id: 'talk', label: 'Розмова', key: 't' },
  { id: 'know', label: 'Знання', key: 'k' },
  { id: 'conn', label: 'Зʼєднання', key: 'c' },
  { id: 'dev', label: 'Пристрої', key: 'd' },
  { id: 'pers', label: 'Характер', key: 'p' },
  { id: 'act', label: 'Журнал', key: 'a' },
]

const SECTION_BY_KEY: Record<string, Section> = Object.fromEntries(
  SECTIONS.map((s) => [s.key, s.id]),
)

// ------------------------------------------------------------------ icons
// Inline, matching the mockup's paths exactly (viewBox 0 0 24 24, stroke
// currentColor) - one file, no icon package, so the set stays exactly the
// seven the design calls for and nothing else creeps in.

function IconBase({ children }: { children: ReactNode }) {
  return (
    <svg className="ic" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" aria-hidden="true">
      {children}
    </svg>
  )
}

function SectionIcon({ id }: { id: Section }) {
  switch (id) {
    case 'home':
      return (
        <IconBase>
          <circle cx="12" cy="12" r="2.1" />
          <path strokeLinecap="round" d="M8 8a5.6 5.6 0 0 0 0 8M16 16a5.6 5.6 0 0 0 0-8" />
          <path strokeLinecap="round" d="M4.7 4.7a10.3 10.3 0 0 0 0 14.6M19.3 19.3a10.3 10.3 0 0 0 0-14.6" />
        </IconBase>
      )
    case 'talk':
      return (
        <IconBase>
          <path strokeLinecap="round" strokeLinejoin="round" d="M3.5 5.5h17v9.5h-11l-6 4.5z" />
          <path strokeLinecap="round" d="M7.5 9.2h9M7.5 12h6" />
        </IconBase>
      )
    case 'know':
      return (
        <IconBase>
          <path strokeLinecap="round" strokeLinejoin="round" d="M12 3 3 7.4l9 4.4 9-4.4z" />
          <path strokeLinecap="round" strokeLinejoin="round" d="M3 12.3 12 16.7l9-4.4" />
          <path strokeLinecap="round" strokeLinejoin="round" d="M3 16.9 12 21.3l9-4.4" />
        </IconBase>
      )
    case 'conn':
      return (
        <IconBase>
          <circle cx="6" cy="6.4" r="2.3" />
          <circle cx="18" cy="9.4" r="2.3" />
          <circle cx="10.5" cy="18" r="2.3" />
          <path strokeLinecap="round" d="M8.2 7 15.8 8.9M9.2 15.9 16.4 11.3M6.9 8.6l2.7 7.2" />
        </IconBase>
      )
    case 'dev':
      return <DesktopIcon />
    case 'pers':
      return (
        <IconBase>
          <circle cx="12" cy="8.4" r="3.7" />
          <path strokeLinecap="round" d="M4.8 20.2a7.4 7.4 0 0 1 14.4 0" />
        </IconBase>
      )
    case 'act':
      return (
        <IconBase>
          <path strokeLinecap="round" d="M3.5 20h17" />
          <path strokeLinecap="round" d="M7 20v-5.5M11.7 20V6.5M16.3 20v-3.4M20.5 20v-8.6" />
        </IconBase>
      )
  }
}

function DesktopIcon() {
  return (
    <IconBase>
      <rect x="3" y="5.6" width="18" height="12.8" rx="2.2" strokeLinejoin="round" />
      <rect x="7.4" y="9.8" width="2.8" height="4.4" rx="1.2" fill="currentColor" stroke="none" />
      <rect x="13.8" y="9.8" width="2.8" height="4.4" rx="1.2" fill="currentColor" stroke="none" />
    </IconBase>
  )
}

function BrowserIcon() {
  return (
    <IconBase>
      <rect x="3" y="4.5" width="18" height="12" rx="1.8" />
      <path strokeLinecap="round" d="M8 20h8M12 16.5V20" />
    </IconBase>
  )
}

function SearchIcon() {
  return (
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" aria-hidden="true">
      <circle cx="10.5" cy="10.5" r="6.2" />
      <path d="M15.2 15.2 20 20" />
    </svg>
  )
}

function SunIcon() {
  return (
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" aria-hidden="true">
      <circle cx="12" cy="12" r="4.2" />
      <path d="M12 2.6v2.2M12 19.2v2.2M2.6 12h2.2M19.2 12h2.2M5.4 5.4l1.6 1.6M17 17l1.6 1.6M18.6 5.4 17 7M7 17l-1.6 1.6" />
    </svg>
  )
}

function MoonIcon() {
  return (
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" aria-hidden="true">
      <path d="M20 14.5A8.2 8.2 0 0 1 9.5 4 8.5 8.5 0 1 0 20 14.5z" />
    </svg>
  )
}

function LogoutIcon() {
  return (
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
      <path d="M15 4.5h-3.5A2.5 2.5 0 0 0 9 7v10a2.5 2.5 0 0 0 2.5 2.5H15" />
      <path d="M19 12H10.5M16 8.5 19.5 12 16 15.5" />
    </svg>
  )
}

// ------------------------------------------------------------------ counts
// Sidebar badges, wired only where an endpoint already exists and honestly
// answers the question. Зʼєднання, Пристрої and Журнал have no backend yet
// (see the plan's honest-data table) and Розмова has no meaningful count
// until a conversation can actually be started from this shell (Task 2/6),
// so all three stay unbadged rather than showing a placeholder zero.

interface Counts {
  know: number | null
  pers: number | null
}

function useCounts(notifyUnauthorized: () => void): Counts {
  const [know, setKnow] = useState<number | null>(null)
  const [pers, setPers] = useState<number | null>(null)

  useEffect(() => {
    let alive = true
    const onFail = (err: unknown) => {
      if (alive && err instanceof Unauthorized) notifyUnauthorized()
    }
    memoryApi.list().then((facts) => alive && setKnow(facts.length), onFail)
    rolesApi.list().then((roles) => alive && setPers(roles.length), onFail)
    return () => {
      alive = false
    }
  }, [notifyUnauthorized])

  return { know, pers }
}

// ------------------------------------------------------------------ theme

type Theme = 'light' | 'dark'
const THEME_KEY = 'laa-theme'

function readSavedTheme(): Theme | null {
  try {
    const saved = localStorage.getItem(THEME_KEY)
    return saved === 'light' || saved === 'dark' ? saved : null
  } catch {
    return null
  }
}

function systemPrefersDark(): boolean {
  return typeof window !== 'undefined' && typeof window.matchMedia === 'function'
    ? window.matchMedia('(prefers-color-scheme: dark)').matches
    : false
}

// ------------------------------------------------------------------ status

function pillFor(connection: Connection, state: ConversationState): { dot: string; label: string } {
  if (connection.status === 'unauthorized') return { dot: 'off', label: 'сесія завершилась' }
  if (connection.status === 'connecting') return { dot: 'idle', label: 'з’єднання…' }
  switch (state) {
    case 'listening':
      return { dot: 'acc', label: 'слухає' }
    case 'thinking':
      return { dot: 'acc', label: 'думає' }
    case 'speaking':
      return { dot: 'acc', label: 'говорить' }
    default:
      return { dot: 'on', label: 'на звʼязку' }
  }
}

// ------------------------------------------------------------------ shell

interface ShellProps {
  section: Section
  onSectionChange: (section: Section) => void
  connection: Connection
  state: ConversationState
  children: ReactNode
}

function Shell({ section, onSectionChange, connection, state, children }: ShellProps) {
  const { username, signOut, notifyUnauthorized } = useSession()
  const [signingOut, setSigningOut] = useState(false)
  const [theme, setTheme] = useState<Theme | null>(readSavedTheme)
  const counts = useCounts(notifyUnauthorized)
  const askRef = useRef<HTMLInputElement>(null)

  // The DOM attribute is the only thing this effect touches - no React
  // state is set here, so there is nothing for react/set-state-in-effect to
  // flag. tokens.css keys off this exact attribute: absent means "follow
  // the system", present always wins over it, in both directions.
  useEffect(() => {
    const root = document.documentElement
    if (theme) root.setAttribute('data-theme', theme)
    else root.removeAttribute('data-theme')
  }, [theme])

  const dark = theme ? theme === 'dark' : systemPrefersDark()

  const toggleTheme = useCallback(() => {
    const next: Theme = dark ? 'light' : 'dark'
    setTheme(next)
    try {
      localStorage.setItem(THEME_KEY, next)
    } catch {
      // Private browsing, storage disabled - the toggle still works for
      // this load, it just will not be remembered next time.
    }
  }, [dark])

  const focusAsk = useCallback(() => {
    const el = askRef.current
    if (!el) return
    el.focus()
    el.select()
  }, [])

  // ⌘K focuses the ask field; "g" then one of h/t/k/c/d/p/a switches
  // section - the same two shortcuts the mockup defines, ported as real
  // listeners rather than a command palette (none exists, none is implied).
  useEffect(() => {
    let armed = false
    let timer: ReturnType<typeof setTimeout> | undefined
    const onKeyDown = (event: KeyboardEvent) => {
      if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === 'k') {
        event.preventDefault()
        focusAsk()
        return
      }
      const tag = (event.target as HTMLElement | null)?.tagName?.toLowerCase()
      if (tag === 'input' || tag === 'textarea' || tag === 'select') return
      if (event.metaKey || event.ctrlKey || event.altKey) return
      const key = event.key.toLowerCase()
      if (armed) {
        const target = SECTION_BY_KEY[key]
        if (target) {
          event.preventDefault()
          onSectionChange(target)
        }
        armed = false
        clearTimeout(timer)
        return
      }
      if (key === 'g') {
        armed = true
        timer = setTimeout(() => {
          armed = false
        }, 1400)
      }
    }
    document.addEventListener('keydown', onKeyDown)
    return () => {
      document.removeEventListener('keydown', onKeyDown)
      clearTimeout(timer)
    }
  }, [focusAsk, onSectionChange])

  const handleSignOut = async () => {
    setSigningOut(true)
    try {
      await signOut()
    } finally {
      setSigningOut(false)
    }
  }

  const pill = pillFor(connection, state)
  const current = SECTIONS.find((s) => s.id === section)

  return (
    <div className="app">
      <aside className="rail">
        <div className="brand">
          <span className="mark" aria-hidden="true">Л</span>
          <span style={{ minWidth: 0 }}>
            <b>ЛАА</b> <span>· асистент {username ?? '…'}</span>
          </span>
        </div>

        <form className="cmdk" onSubmit={(event) => event.preventDefault()}>
          <SearchIcon />
          <input
            ref={askRef}
            type="text"
            placeholder="Спитати або знайти"
            aria-label="Спитати асистента"
            onKeyDown={(event) => {
              if (event.key === 'Escape') event.currentTarget.blur()
            }}
          />
          <span className="kbd">⌘K</span>
        </form>

        <nav className="nav" aria-label="Розділи">
          <p className="lab">Розділи</p>
          {SECTIONS.map((s) => {
            const count = s.id === 'know' ? counts.know : s.id === 'pers' ? counts.pers : null
            return (
              <button
                key={s.id}
                type="button"
                aria-current={section === s.id ? 'page' : undefined}
                onClick={() => onSectionChange(s.id)}
              >
                <SectionIcon id={s.id} />
                <span className="t">{s.label}</span>
                <span className="slot">
                  {count !== null && <span className="cnt">{count}</span>}
                  <span className="keys">
                    <span className="kbd">G</span>
                    <span className="kbd">{s.key.toUpperCase()}</span>
                  </span>
                </span>
              </button>
            )
          })}
        </nav>

        <nav className="nav" aria-label="Поверхні" style={{ marginTop: 12 }}>
          <p className="lab">Поверхні</p>
          <button type="button" onClick={() => onSectionChange('pers')}>
            <BrowserIcon />
            <span className="t">Цей браузер</span>
            <span className="slot">
              <span className="cnt" style={{ opacity: 1 }}>
                <span className="dot on" title="активний зараз" />
              </span>
            </span>
          </button>
          <button type="button" onClick={() => onSectionChange('dev')}>
            <DesktopIcon />
            <span className="t">Робочий стіл</span>
            <span className="slot">
              <span className="cnt" style={{ opacity: 1 }}>
                <span className="dot off" title="стан невідомий - нічого ще не звітує" />
              </span>
            </span>
          </button>
        </nav>

        <div className="rail-foot">
          <p className="lab">Безкоштовний ліміт</p>
          <span className="v dim">токени за хвилину: ще не рахуємо</span>
        </div>
      </aside>

      <div className="main">
        <header className="top">
          <div className="crumb">
            <span className="c1">ЛАА</span>
            <span className="r">/</span>
            <span className="c2">{current?.label ?? ''}</span>
          </div>
          <div className="top-r">
            <span className="who">{username ?? ''} · web</span>
            <div className="status" role="status" aria-live="polite" title={pill.label}>
              <span className={`dot ${pill.dot}`} aria-hidden="true" />
              <b>{pill.label}</b>
            </div>
            <button
              type="button"
              className="iconbtn"
              aria-label="Перемкнути тему"
              title={dark ? 'Світла тема' : 'Темна тема'}
              onClick={toggleTheme}
            >
              {dark ? <SunIcon /> : <MoonIcon />}
            </button>
            <button
              type="button"
              className="iconbtn"
              aria-label="Вийти"
              title="Вийти"
              disabled={signingOut}
              onClick={handleSignOut}
            >
              <LogoutIcon />
            </button>
          </div>
        </header>

        {children}
      </div>

      <nav className="tabs" aria-label="Розділи">
        {SECTIONS.map((s) => (
          <button
            key={s.id}
            type="button"
            aria-current={section === s.id ? 'page' : undefined}
            onClick={() => onSectionChange(s.id)}
          >
            <SectionIcon id={s.id} />
            <span>{s.label}</span>
          </button>
        ))}
      </nav>
    </div>
  )
}

export default Shell
