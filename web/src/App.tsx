import { useState } from 'react'
import type { ReactNode } from 'react'
import './app.css'
import { SessionContext, useSession, useSessionState } from './api'
import Login from './Login.tsx'
import Shell from './Shell.tsx'
import type { Tab } from './Shell.tsx'
import Chat from './Chat.tsx'
import { useTurn } from './useTurn.ts'

/** Installs the session state machine (useSessionState, in api.ts) into
 * context so useSession() works from any component below - Login, Shell,
 * and whatever Task 5 adds for Settings. */
function SessionProvider({ children }: { children: ReactNode }) {
  const value = useSessionState()
  return <SessionContext.Provider value={value}>{children}</SessionContext.Provider>
}

// Settings is a placeholder in this task; Task 5 replaces it with the real
// view. Tab state lives here, not in a router - see the task notes on why a
// client route named /settings would 404 on reload.
function Placeholder({ label }: { label: string }) {
  return <p style={{ margin: 0, color: 'var(--ink-2)' }}>{label} is not built yet.</p>
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
      {tab === 'chat' ? <Chat turn={turn} /> : <Placeholder label="Settings" />}
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
