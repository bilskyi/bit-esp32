import { useState } from 'react'
import type { ReactNode } from 'react'
import './app.css'
import { SessionContext, useSession, useSessionState } from './api'
import Chat from './Chat.tsx'
import Knowledge from './Knowledge.tsx'
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
// instead of leaving it open against a cookie that no longer exists.
//
// Default section is 'talk' rather than 'home' for now: Розмова is the only
// section this rebuild has actually ported so far (Task 2), and the shots
// harness (web/tools/shots.mjs) logs in and immediately waits for
// `.chat-composer` - it would time out against Головна's placeholder, which
// has no composer until Task 6 gives it one. Task 6 is the right place to
// move this back to 'home', once landing there also has something real to
// show.
function SignedIn() {
  const [section, setSection] = useState<Section>('talk')
  const turn = useTurn()
  const { notifyUnauthorized } = useSession()

  return (
    <Shell section={section} onSectionChange={setSection} connection={turn.connection} state={turn.state}>
      {section === 'talk' ? (
        <Chat turn={turn} onNavigate={setSection} />
      ) : section === 'know' ? (
        <Knowledge notifyUnauthorized={notifyUnauthorized} />
      ) : (
        <SectionPlaceholder section={section} />
      )}
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
