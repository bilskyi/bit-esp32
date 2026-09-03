import { useCallback, useEffect, useRef, useState } from 'react'
import { ApiError, Unauthorized } from './api'
import { GITHUB_REPO, checkImage, firmwareApi } from './firmwareApi'
import type { DeviceFirmware, LatestRelease, PushProgress } from './firmwareApi'

/** How long to keep asking whether the device came back, and how often.
 *
 * The device's own rollback deadline is ten minutes (OTA_DEADLINE_MS in
 * firmware/main/ota.h), but a healthy restart takes seconds - it reboots,
 * joins WiFi and reopens the socket. Two minutes is generous for that and
 * short enough that the panel stops claiming to be watching something it
 * gave up on. Running out of time is not a verdict: it says so. */
const WATCH_FOR_MS = 120000
const WATCH_EVERY_MS = 2000

/** Firmware, on the Пристрої page.
 *
 * Split out of Devices.tsx rather than added to it: that file was already
 * doing persona, the live screen and the joining instructions, and this
 * carries a file picker, two kinds of progress and a poll loop of its own.
 *
 * The two phases are the whole point of the layout. "Передано" is cheap and
 * proves very little. "Запустилось" means the device rebooted, came back and
 * reported a *different* version - which is simultaneously the only proof
 * that the rollback in ota.c did not fire. They are never merged into one
 * tick. */
function Firmware({ notifyUnauthorized }: { notifyUnauthorized: () => void }) {
  const [device, setDevice] = useState<DeviceFirmware | null>(null)
  const [latest, setLatest] = useState<LatestRelease | null>(null)
  const [file, setFile] = useState<File | null>(null)
  const [fileError, setFileError] = useState<string | null>(null)
  const [progress, setProgress] = useState<PushProgress | null>(null)
  const [busy, setBusy] = useState(false)
  const [sent, setSent] = useState<string | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [watching, setWatching] = useState(false)
  const [restarted, setRestarted] = useState<string | null>(null)
  const [gaveUp, setGaveUp] = useState(false)

  // Cleared on unmount so a poll loop cannot outlive the panel and call
  // setState on something that is gone.
  const alive = useRef(true)
  useEffect(() => {
    alive.current = true
    return () => {
      alive.current = false
    }
  }, [])

  const reload = useCallback(async () => {
    try {
      const fresh = await firmwareApi.device()
      if (alive.current) setDevice(fresh)
      return fresh
    } catch (err) {
      if (err instanceof Unauthorized) notifyUnauthorized()
      return null
    }
  }, [notifyUnauthorized])

  useEffect(() => {
    reload()
    // Null for a repository that is not configured, has no releases, or
    // cannot be reached - none of which is worth a banner.
    firmwareApi.latest().then((release) => {
      if (alive.current) setLatest(release)
    })
  }, [reload])

  const pick = async (chosen: File | null) => {
    setFile(null)
    setFileError(null)
    setError(null)
    if (!chosen) return

    // Judged here so a wrong file costs nothing. The server checks the same
    // four offsets, and so does the firmware - this one only saves the
    // person a megabyte and a wait.
    const head = new Uint8Array(await chosen.slice(0, 144).arrayBuffer())
    const reason = checkImage(head)
    if (reason) {
      setFileError(`Це не прошивка для цього пристрою: ${reason}.`)
      return
    }
    setFile(chosen)
  }

  /** Ask, repeatedly, whether the device came back on a different version. */
  const watchForTheRestart = async (before: string | null) => {
    setWatching(true)
    setGaveUp(false)
    const until = Date.now() + WATCH_FOR_MS

    while (Date.now() < until) {
      await new Promise((resolve) => setTimeout(resolve, WATCH_EVERY_MS))
      if (!alive.current) return
      const fresh = await firmwareApi.device()
      if (!alive.current) return
      setDevice(fresh)
      if (fresh.online && fresh.version && fresh.version !== before) {
        setRestarted(fresh.version)
        setWatching(false)
        return
      }
    }
    setWatching(false)
    setGaveUp(true)
  }

  const update = async () => {
    if (!file) return
    const before = device?.version ?? null

    setBusy(true)
    setError(null)
    setSent(null)
    setRestarted(null)
    setGaveUp(false)
    setProgress(null)

    try {
      const outcome = await firmwareApi.push(file, setProgress)
      if (outcome.outcome !== 'ota_ready') {
        // The bytes arrived and the device would not take them. Its own
        // wording, because a reason invented here is a reason nobody can act
        // on.
        setError(
          outcome.reason
            ? `Пристрій відмовився: ${outcome.reason}.`
            : `Пристрій відповів «${outcome.outcome}».`,
        )
        return
      }
      setSent(`${file.name} — ${file.size.toLocaleString('uk-UA')} байт`)
      await watchForTheRestart(before)
    } catch (err) {
      if (err instanceof Unauthorized) {
        notifyUnauthorized()
      } else {
        setError(err instanceof ApiError ? err.detail : (err as Error).message)
      }
    } finally {
      setBusy(false)
      setProgress(null)
    }
  }

  const percent = progress && progress.total > 0
    ? Math.round((progress.sent * 100) / progress.total)
    : null

  return (
    <div className="grp" style={{ marginTop: 12 }}>
      <p className="lab" style={{ marginBottom: 2 }}>
        Прошивка
      </p>

      <div className="kv bare" style={{ gridTemplateColumns: 'repeat(2,minmax(0,1fr))' }}>
        <div>
          <span className="k">на пристрої</span>
          <span className="v mono">
            {device === null ? '…' : device.online ? (device.version ?? 'не звітує') : 'офлайн'}
          </span>
        </div>
        <div>
          <span className="k">останній реліз</span>
          {/* Only a real version gets the mono slot. "не налаштовано" is
              prose, and in mono beside a version it wrapped onto two lines
              and pulled the two-column grid apart. */}
          {latest ? (
            <span className="v mono">{latest.tag}</span>
          ) : (
            <span className="v">{GITHUB_REPO === '' ? 'не налаштовано' : 'релізів немає'}</span>
          )}
        </div>
      </div>

      {latest && (
        <p className="prose" style={{ fontSize: 12, marginTop: 6 }}>
          <a href={latest.url} rel="noreferrer">
            Завантажити {latest.tag}
          </a>{' '}
          — і перетягни файл сюди. Пристрій сам на GitHub не ходить: качає браузер.
        </p>
      )}

      {/* Two version strings side by side, and no comparison between them.
          `git describe` output has no total order - v0.3.0-2-gabc1234-dirty
          is not comparable with v0.3.0 - so a "нова версія доступна" badge
          would be a guess dressed as a fact. The person decides. */}
      <div className="field" style={{ marginTop: 10 }}>
        <span className="flab">
          <span>файл прошивки</span>
          <span className="c">.bin</span>
        </span>
        {/* The input carries the label association and the focus; the <label>
            is what anyone actually sees. See .filepick in app.css for why it
            is done this way round rather than with ::file-selector-button. */}
        <div className="filepick">
          <input
            id="fw-file"
            type="file"
            accept=".bin"
            disabled={busy}
            onChange={(event) => pick(event.target.files?.[0] ?? null)}
          />
          <label className="pickbtn" htmlFor="fw-file">
            Вибрати файл
          </label>
          <span className="pickname">
            {file ? `${file.name} · ${file.size.toLocaleString('uk-UA')} байт` : 'файл не вибрано'}
          </span>
        </div>
      </div>

      {fileError && (
        <p className="chat-status" role="alert">
          {fileError}
        </p>
      )}

      <button
        type="button"
        className="btn pri"
        disabled={busy || !file || !device?.online}
        onClick={update}
        style={{ marginTop: 8 }}
      >
        {busy ? 'Оновлюю…' : 'Оновити'}
      </button>

      {!device?.online && device !== null && (
        <p className="prose" style={{ fontSize: 12, marginTop: 6 }}>
          Пристрій не на звʼязку, тож надіслати нічого не можна. Коли сервер недосяжний зовсім —
          прошивку можна залити з точки доступу самого пристрою, через ту саму сторінку
          налаштувань.
        </p>
      )}

      {progress && percent !== null && (
        <p className="chat-status">
          {progress.phase === 'uploading' ? 'Надсилаю на сервер' : 'Сервер передає пристрою'} —{' '}
          {percent}%
        </p>
      )}

      {/* Phase one and phase two, never merged. */}
      {sent && (
        <div className="rows" style={{ marginTop: 8 }}>
          <div className="row">
            <span className="nm" style={{ width: 96, flex: 'none' }}>
              передано
            </span>
            <span className="txt g dim" style={{ fontSize: 12 }}>
              {sent}
            </span>
          </div>
          <div className="row">
            <span className="nm" style={{ width: 96, flex: 'none' }}>
              запустилось
            </span>
            <span className="txt g dim" style={{ fontSize: 12 }}>
              {restarted
                ? `так — пристрій повернувся на ${restarted}`
                : watching
                  ? 'чекаю, поки пристрій перезавантажиться і повернеться…'
                  : gaveUp
                    ? 'за дві хвилини пристрій не повернувся з новою версією. Це ще не означає, що'
                      + ' оновлення провалилось — але й не означає, що спрацювало. Дивись на екран.'
                    : '—'}
            </span>
          </div>
        </div>
      )}

      {error && (
        <p className="chat-status" role="alert">
          {error}
        </p>
      )}

      <div className="empty" style={{ padding: '8px 0', marginTop: 8 }}>
        <p className="prose" style={{ fontSize: 12 }}>
          Аптайм, вільну память і час останнього виходу на звʼязок пристрій і далі не звітує —
          heartbeat немає. Версію прошивки тепер звітує, бо надсилає її щоразу, коли відкриває
          звʼязок.
        </p>
      </div>
    </div>
  )
}

export default Firmware
