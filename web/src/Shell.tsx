import { useState } from 'react'
import type { ReactNode } from 'react'
import { useSession } from './api'

export type Tab = 'chat' | 'settings'

interface ShellProps {
  activeTab: Tab
  onTabChange: (tab: Tab) => void
  children: ReactNode
}

/** Header (product name, username, sign out) plus the Chat/Settings tab
 * row, wrapping whichever tab App.tsx currently has active. Tab state
 * itself lives in App.tsx - Shell only renders the row and reports clicks. */
function Shell({ activeTab, onTabChange, children }: ShellProps) {
  const { username, signOut } = useSession()
  const [signingOut, setSigningOut] = useState(false)

  const handleSignOut = async () => {
    setSigningOut(true)
    try {
      await signOut()
    } finally {
      setSigningOut(false)
    }
  }

  return (
    <div className="shell">
      <header className="shell-header">
        <p className="shell-title">Voice companion</p>
        <div className="shell-user">
          <span className="shell-username">{username}</span>
          <button type="button" onClick={handleSignOut} disabled={signingOut}>
            {signingOut ? 'Signing out…' : 'Sign out'}
          </button>
        </div>
      </header>

      <nav className="shell-tabs" aria-label="Sections">
        <button
          type="button"
          className="shell-tab"
          aria-current={activeTab === 'chat' ? 'true' : undefined}
          onClick={() => onTabChange('chat')}
        >
          Chat
        </button>
        <button
          type="button"
          className="shell-tab"
          aria-current={activeTab === 'settings' ? 'true' : undefined}
          onClick={() => onTabChange('settings')}
        >
          Settings
        </button>
      </nav>

      <main className="shell-content">{children}</main>
    </div>
  )
}

export default Shell
