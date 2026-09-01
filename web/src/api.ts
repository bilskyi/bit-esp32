// Fetch wrapper plus the session state built on top of it.
//
// Kept in one .ts file (no JSX) on purpose: the only piece of the session
// mechanism that needs JSX is the <SessionContext.Provider> wrapper, and
// that lives in App.tsx. Everything else - the state machine, the context
// object, the `useSession()` hook - has no JSX in it and belongs here next
// to the requests it wraps.

import { createContext, useCallback, useContext, useEffect, useMemo, useState } from 'react'

// ------------------------------------------------------------------ errors

/** Any non-2xx response other than 401. Carries the server's own `detail`
 * string so a caller (a roles form, say) can show *why* a 409 or 422
 * happened instead of a generic "something went wrong". */
export class ApiError extends Error {
  readonly status: number
  readonly detail: string

  constructor(status: number, detail: string) {
    super(detail)
    this.name = 'ApiError'
    this.status = status
    this.detail = detail
  }
}

/** A 401, specifically. A subclass rather than a flag on ApiError so every
 * call site can tell "your session ended" apart from "that request was
 * invalid" with one `instanceof Unauthorized` check - the server
 * deliberately gives both /login and every protected route the same shape
 * of failure, and the two mean very different things to the UI. */
export class Unauthorized extends ApiError {
  constructor(detail: string) {
    super(401, detail)
    this.name = 'Unauthorized'
  }
}

async function readDetail(res: Response): Promise<string> {
  try {
    const body: unknown = await res.json()
    if (body && typeof body === 'object' && typeof (body as { detail?: unknown }).detail === 'string') {
      return (body as { detail: string }).detail
    }
  } catch {
    // Not JSON, or no body at all - fall through to the status line.
  }
  return res.statusText || `request failed with status ${res.status}`
}

// ------------------------------------------------------------------ fetch

async function request<T>(method: string, path: string, body?: unknown): Promise<T> {
  const res = await fetch(path, {
    method,
    credentials: 'same-origin',
    headers: { 'Content-Type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body),
  })

  if (res.status === 401) {
    throw new Unauthorized(await readDetail(res))
  }
  if (!res.ok) {
    throw new ApiError(res.status, await readDetail(res))
  }
  if (res.status === 204) {
    return null as T
  }
  const text = await res.text()
  return text ? (JSON.parse(text) as T) : (null as T)
}

export const api = {
  get: <T,>(path: string): Promise<T> => request<T>('GET', path),
  post: <T,>(path: string, body?: unknown): Promise<T> => request<T>('POST', path, body),
  put: <T,>(path: string, body?: unknown): Promise<T> => request<T>('PUT', path, body),
  del: <T,>(path: string): Promise<T> => request<T>('DELETE', path),
}

// ----------------------------------------------------------------- session

export type SessionState = 'loading' | 'in' | 'out'

export interface SessionValue {
  state: SessionState
  username: string | null
  /** A neutral line for the login view explaining *why* it is showing
   * again - a session that expired, a server that could not be reached.
   * Never the reason a login attempt itself failed; Login.tsx owns that
   * message and shows it on its own, separately from this one. */
  notice: string | null
  signIn: (username: string, password: string) => Promise<void>
  signOut: () => Promise<void>
  /** What every later call site reaches for after catching Unauthorized:
   * drop back to the login view through this state, not a page reload, so
   * whatever the person had typed elsewhere survives. Do not call this from
   * the login form's own failed attempt - that 401 means "wrong password",
   * not "your session ended", and the two must never look the same. */
  notifyUnauthorized: () => void
}

export const SessionContext = createContext<SessionValue | null>(null)

/** Read the session from context. Throws if used outside the provider that
 * App.tsx installs at the root - every screen in this app is inside it. */
export function useSession(): SessionValue {
  const value = useContext(SessionContext)
  if (!value) {
    throw new Error('useSession() called outside <SessionProvider>')
  }
  return value
}

interface MeResponse {
  username: string
}

/** The state machine behind useSession(). A plain hook, not a component -
 * App.tsx calls it once and hands the result to <SessionContext.Provider>. */
export function useSessionState(): SessionValue {
  const [state, setState] = useState<SessionState>('loading')
  const [username, setUsername] = useState<string | null>(null)
  const [notice, setNotice] = useState<string | null>(null)

  const probe = useCallback(async () => {
    try {
      const me = await api.get<MeResponse>('/me')
      setUsername(me.username)
      setState('in')
    } catch (err) {
      setUsername(null)
      setState('out')
      // A 401 here just means "not signed in yet" - no message needed.
      // Anything else (server unreachable, an unexpected 5xx) is worth a
      // line, since the person otherwise just sees a login form for no
      // apparent reason.
      if (!(err instanceof Unauthorized)) {
        setNotice('Could not reach the server. Check your connection and try again.')
      }
    }
  }, [])

  useEffect(() => {
    probe()
  }, [probe])

  const signIn = useCallback(
    async (loginUsername: string, password: string) => {
      // Let a failure propagate - Login.tsx is the one place that decides
      // what a failed sign-in looks like.
      await api.post('/login', { username: loginUsername, password })
      setNotice(null)
      await probe()
    },
    [probe],
  )

  const signOut = useCallback(async () => {
    try {
      await api.post('/logout')
    } catch {
      // Signing out was the person's own request - land back on the login
      // view regardless of whether the network call itself succeeded.
    }
    setUsername(null)
    setNotice(null)
    setState('out')
  }, [])

  const notifyUnauthorized = useCallback(() => {
    setUsername(null)
    setNotice('Your session ended. Sign in again.')
    setState('out')
  }, [])

  return useMemo(
    () => ({ state, username, notice, signIn, signOut, notifyUnauthorized }),
    [state, username, notice, signIn, signOut, notifyUnauthorized],
  )
}
