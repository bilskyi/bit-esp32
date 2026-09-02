import { useState } from 'react'
import type { ReactNode } from 'react'
import './app.css'
import { SessionContext, useSession, useSessionState } from './api'
import Login from './Login.tsx'
import Shell from './Shell.tsx'
import type { Section } from './Shell.tsx'
import { useTurn } from './useTurn.ts'

/** Installs the session state machine (useSessionState, in api.ts) into
 * context so useSession() works from any component below - Login, Shell,
 * and whatever each section renders. */
function SessionProvider({ children }: { children: ReactNode }) {
  const value = useSessionState()
  return <SessionContext.Provider value={value}>{children}</SessionContext.Provider>
}

const SECTION_LABEL: Record<Section, string> = {
  home: 'Головна',
  talk: 'Розмова',
  know: 'Знання',
  conn: 'Зʼєднання',
  dev: 'Пристрої',
  pers: 'Характер',
  act: 'Журнал',
}

/** Task 1 builds the shell only - every section is a placeholder until its
 * own task ports it for real (see the plan's per-task file list). This
 * shows the section's name and says, plainly, that it is not there yet; it
 * never invents content a later task would have to contradict. */
function SectionPlaceholder({ section }: { section: Section }) {
  return (
    <section className="view">
      <div className="wrap">
        <div className="head">
          <div className="hl">
            <h1>{SECTION_LABEL[section]}</h1>
            <p className="prose">Я ще переношу цей розділ у новий вигляд.</p>
          </div>
        </div>
      </div>
    </section>
  )
}

// Rendered only once the session is signed in, and torn down the moment it
// is not. useTurn() is called here - one level above the sections - rather
// than inside whichever one is showing, so switching sections never drops
// the socket or the transcript, and signing out actually closes the socket
// instead of leaving it open against a cookie that no longer exists. This
// is also why the status pill is honest even though Розмова is still a
// placeholder: the socket this task connects is the same one Task 2 wires
// Chat.tsx to, not a stand-in.
function SignedIn() {
  const [section, setSection] = useState<Section>('home')
  const { connection, state } = useTurn()

  return (
    <Shell section={section} onSectionChange={setSection} connection={connection} state={state}>
      <SectionPlaceholder section={section} />
    </Shell>
  )
}

function AppShell() {
  const { state } = useSession()

  if (state === 'loading') {
    return (
      <main className="app-loading">
        <p style={{ margin: 0 }}>Loading…</p>
      </main>
    )
  }

  if (state === 'out') {
    return <Login />
  }

  return <SignedIn />
}

function App() {
  return (
    <SessionProvider>
      <AppShell />
    </SessionProvider>
  )
}

export default App
