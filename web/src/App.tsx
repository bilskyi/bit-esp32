import { useCallback, useRef, useState } from 'react'
import type { ReactNode } from 'react'
import './app.css'
import { SessionContext, useSession, useSessionState } from './api'
import Chat from './Chat.tsx'
import Connections from './Connections.tsx'
import Devices from './Devices.tsx'
import Home from './Home.tsx'
import Journal from './Journal.tsx'
import Knowledge from './Knowledge.tsx'
import Login from './Login.tsx'
import Personality from './Personality.tsx'
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

// Rendered only once the session is signed in, and torn down the moment it
// is not. useTurn() is called here - one level above the sections - rather
// than inside whichever one is showing, so switching sections never drops
// the socket or the transcript, and signing out actually closes the socket
// instead of leaving it open against a cookie that no longer exists.
//
// Default section is 'home' - the client's own decision (see Task 6's
// brief): the app opens on the home that greets you, not straight into a
// transcript. Task 2 had briefly moved this to 'talk' because Головна had
// no composer of its own yet to satisfy the shots harness's own wait for
// one; Task 6 gave it a real one (Home.tsx), so this reverts that.
function SignedIn() {
  const [section, setSection] = useState<Section>('home')
  const turn = useTurn()
  const { notifyUnauthorized } = useSession()
  // Home.tsx mounts this input; Shell.tsx's ⌘K handler and its own "cmdk"
  // launcher button have no access to Home's DOM, so App.tsx - the one
  // place both are reachable from - hands them a way to reach it instead of
  // either keeping a second, disconnected field of its own (Task 1's own
  // stand-in) or Home reaching up into Shell.
  const homeAskInputRef = useRef<HTMLInputElement>(null)

  const focusHomeAsk = useCallback(() => {
    setSection('home')
    // Home may not be mounted yet this tick (switching away from another
    // section unmounts it entirely - see the ternary below), so the ref is
    // not necessarily attached the instant this runs. A short delay after
    // the section switch is exactly what the approved mockup's own
    // focusAsk() does for the same handoff (60ms, after its own show()).
    setTimeout(() => homeAskInputRef.current?.focus(), 60)
  }, [])

  return (
    <Shell section={section} onSectionChange={setSection} connection={turn.connection} state={turn.state} onFocusAsk={focusHomeAsk}>
      {section === 'talk' ? (
        <Chat turn={turn} onNavigate={setSection} />
      ) : section === 'know' ? (
        <Knowledge notifyUnauthorized={notifyUnauthorized} />
      ) : section === 'pers' ? (
        <Personality notifyUnauthorized={notifyUnauthorized} />
      ) : section === 'conn' ? (
        <Connections />
      ) : section === 'dev' ? (
        <Devices turn={turn} notifyUnauthorized={notifyUnauthorized} />
      ) : section === 'act' ? (
        <Journal onNavigate={setSection} notifyUnauthorized={notifyUnauthorized} />
      ) : (
        <Home turn={turn} onNavigate={setSection} notifyUnauthorized={notifyUnauthorized} askInputRef={homeAskInputRef} />
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
