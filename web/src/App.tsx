import { useState } from 'react'
import type { ReactNode } from 'react'
import './app.css'
import { SessionContext, useSession, useSessionState } from './api'
import Login from './Login.tsx'
import Shell from './Shell.tsx'
import type { Tab } from './Shell.tsx'

/** Installs the session state machine (useSessionState, in api.ts) into
 * context so useSession() works from any component below - Login, Shell,
 * and whatever Tasks 3 and 5 add for Chat and Settings. */
function SessionProvider({ children }: { children: ReactNode }) {
  const value = useSessionState()
  return <SessionContext.Provider value={value}>{children}</SessionContext.Provider>
}

// Chat and Settings are placeholders in this task; Tasks 3 and 5 replace
// these with the real views. Tab state lives here, not in a router - see
// the task notes on why a client route named /settings would 404 on reload.
function Placeholder({ label }: { label: string }) {
  return <p style={{ margin: 0, color: 'var(--ink-2)' }}>{label} is not built yet.</p>
}

function AppShell() {
  const { state } = useSession()
  const [tab, setTab] = useState<Tab>('chat')

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

  return (
    <Shell activeTab={tab} onTabChange={setTab}>
      {tab === 'chat' ? <Placeholder label="Chat" /> : <Placeholder label="Settings" />}
    </Shell>
  )
}

function App() {
  return (
    <SessionProvider>
      <AppShell />
    </SessionProvider>
  )
}

export default App
