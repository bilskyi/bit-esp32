import { useState } from 'react'
import type { ReactNode } from 'react'
import './app.css'
import { SessionContext, useSession, useSessionState } from './api'
import Login from './Login.tsx'
import Shell from './Shell.tsx'
import type { Tab } from './Shell.tsx'
import Chat from './Chat.tsx'
import Settings from './Settings.tsx'
import { useTurn } from './useTurn.ts'

/** Installs the session state machine (useSessionState, in api.ts) into
 * context so useSession() works from any component below - Login, Shell,
 * and Settings. */
function SessionProvider({ children }: { children: ReactNode }) {
  const value = useSessionState()
  return <SessionContext.Provider value={value}>{children}</SessionContext.Provider>
}

// Rendered only once the session is signed in, and torn down the moment it
// is not. useTurn() is called here - one level above the tabs - rather than
// inside Chat itself, so switching to Settings and back never drops the
// socket or the transcript, and signing out actually closes the socket
// instead of leaving it open against a cookie that no longer exists.
function SignedIn() {
  const [tab, setTab] = useState<Tab>('chat')
  const turn = useTurn()

  return (
    <Shell activeTab={tab} onTabChange={setTab}>
      {tab === 'chat' ? <Chat turn={turn} /> : <Settings />}
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
