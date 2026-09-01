import { useState } from 'react'
import type { FormEvent } from 'react'
import { useSession } from './api'

// The server deliberately returns one generic 401 for both an unknown
// username and a wrong password (see server/accounts.py: it hashes a
// throwaway password even when the username doesn't exist, so the two
// cases cost the same time). Showing a field-level guess here would undo
// that on the UI side, so every failed attempt - wrong password, unknown
// user, or the request not completing at all - gets this one line.
const FAILURE_MESSAGE = 'That username and password did not match.'

function Login() {
  const { notice, signIn } = useSession()
  const [username, setUsername] = useState('')
  const [password, setPassword] = useState('')
  const [submitting, setSubmitting] = useState(false)
  const [error, setError] = useState<string | null>(null)

  const handleSubmit = async (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault()
    setSubmitting(true)
    setError(null)
    try {
      await signIn(username, password)
      // On success useSession()'s state flips to 'in' and App.tsx swaps
      // this view out from under us - nothing left to do here.
    } catch {
      setError(FAILURE_MESSAGE)
    } finally {
      setSubmitting(false)
    }
  }

  return (
    <main className="login-page">
      <form className="login-card" onSubmit={handleSubmit} noValidate>
        <div>
          <p className="login-kicker">sign in</p>
          <h1 className="login-title">Voice companion</h1>
        </div>

        {notice && <p className="login-notice">{notice}</p>}

        <div className="field">
          <label htmlFor="login-username">Username</label>
          <input
            id="login-username"
            name="username"
            type="text"
            autoComplete="username"
            autoCapitalize="off"
            autoCorrect="off"
            spellCheck={false}
            required
            value={username}
            onChange={(event) => setUsername(event.target.value)}
            disabled={submitting}
          />
        </div>

        <div className="field">
          <label htmlFor="login-password">Password</label>
          <input
            id="login-password"
            name="password"
            type="password"
            autoComplete="current-password"
            required
            value={password}
            onChange={(event) => setPassword(event.target.value)}
            disabled={submitting}
          />
        </div>

        {error && (
          <p className="login-error" role="alert">
            {error}
          </p>
        )}

        <button type="submit" disabled={submitting}>
          {submitting ? 'Signing in…' : 'Sign in'}
        </button>
      </form>
    </main>
  )
}

export default Login
