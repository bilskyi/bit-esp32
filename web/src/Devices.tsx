import { useCallback, useEffect, useState } from 'react'
import { ApiError, Unauthorized } from './api'
import Face from './Face.tsx'
import Firmware from './Firmware.tsx'
import { DEVICE_ID, rolesApi, surfacesApi } from './settingsApi'
import type { Role, Surface } from './settingsApi'
import type { Turn, TurnValue } from './useTurn.ts'

interface DevicesProps {
  /** The one useTurn() instance App.tsx keeps alive for the whole signed-in
   * session - passed through so the live screen reflects the real,
   * shared conversation state, exactly as Chat.tsx's own "Голос" panel
   * does. Devices.tsx owns none of the socket itself. */
  turn: TurnValue
  notifyUnauthorized: () => void
}

function describeError(err: unknown, fallback: string): string {
  return err instanceof ApiError ? err.detail : fallback
}

/** The most recent emotion any turn actually got tagged with - the same
 * search Chat.tsx runs (see its own lastEmotion for why: a fresh question
 * starts with emotion: null until its own frame lands, and the face should
 * hold the previous mood through listening/thinking rather than snapping to
 * neutral). Duplicated rather than imported because Chat.tsx does not
 * export it and this task does not touch Chat.tsx. */
function lastEmotion(turns: Turn[]): string | null {
  for (let i = turns.length - 1; i >= 0; i--) {
    const emotion = turns[i].emotion
    if (emotion) return emotion
  }
  return null
}

/** The address the firmware's own SERVER_URI would need to point at, derived
 * the same way useTurn.ts derives its own socket URL
 * (`location.origin.replace(/^http/, 'ws') + '/ws'`) - a real, live value
 * read from the page you are looking at, not a placeholder. */
function serverSocketUrl(): string {
  if (typeof location === 'undefined') return ''
  return `${location.origin.replace(/^http/, 'ws')}/ws`
}

/** Пристрої: the one device this app knows about, told exactly as far as
 * the server can back it up and no further. What is real: `device_id`
 * ("default"), the persona its esp32 surface runs (GET/PUT
 * /settings/surfaces, the same call Personality.tsx's "Яка де" uses), the
 * live screen - Face.tsx playing the same state/emotion stream Chat.tsx
 * drives its own preview from, decoded from the same face-frames.json the
 * firmware's own eyes are exported from - and, since over-the-air updates
 * landed, whether the device is connected and what firmware version it
 * reports. Those last two come from GET /firmware/device, backed by the
 * `hello` frame the firmware now sends on every connect.
 *
 * What is still not real, and does not appear here: free heap, uptime, the
 * time it was last seen, a device registry that outlives a connection, or
 * an add-device flow that could register anything - see the section below
 * the fold for why "add a device" is prose, not a form. */
function Devices({ turn, notifyUnauthorized }: DevicesProps) {
  const [roles, setRoles] = useState<Role[] | null>(null)
  const [surfaces, setSurfaces] = useState<Record<Surface, Role> | null>(null)
  const [loadError, setLoadError] = useState<string | null>(null)

  const [personaBusy, setPersonaBusy] = useState(false)
  const [personaNotice, setPersonaNotice] = useState<string | null>(null)
  const [personaError, setPersonaError] = useState<string | null>(null)

  const onUnauthorizedOr = useCallback(
    (err: unknown, setter: (message: string) => void, fallback: string) => {
      if (err instanceof Unauthorized) {
        notifyUnauthorized()
        return
      }
      setter(describeError(err, fallback))
    },
    [notifyUnauthorized],
  )

  const reloadRoles = useCallback(async () => {
    try {
      setRoles(await rolesApi.list())
    } catch (err) {
      onUnauthorizedOr(err, setLoadError, 'Не вдалося завантажити персони.')
    }
  }, [onUnauthorizedOr])

  const reloadSurfaces = useCallback(async () => {
    try {
      setSurfaces(await surfacesApi.get())
    } catch (err) {
      onUnauthorizedOr(err, setLoadError, 'Не вдалося завантажити налаштування поверхонь.')
    }
  }, [onUnauthorizedOr])

  useEffect(() => {
    // useEffect's own callback cannot be async; the standard shape for
    // "fire two independent GETs on mount" is a nested async function,
    // invoked once, right here.
    async function load() {
      await Promise.all([reloadRoles(), reloadSurfaces()])
    }
    load()
  }, [reloadRoles, reloadSurfaces])

  const handlePersonaChange = async (roleId: number) => {
    setPersonaBusy(true)
    setPersonaError(null)
    setPersonaNotice(null)
    try {
      await surfacesApi.set('esp32', roleId)
      await reloadSurfaces()
      setPersonaNotice('Збережено — діє з наступного питання.')
    } catch (err) {
      if (err instanceof Unauthorized) {
        notifyUnauthorized()
      } else {
        setPersonaError(describeError(err, 'Не вдалося змінити.'))
      }
    } finally {
      setPersonaBusy(false)
    }
  }

  const online = turn.connection.status === 'open'
  const wsUrl = serverSocketUrl()

  return (
    <section className="view">
      <div className="wrap">
        <div className="head">
          <div className="hl">
            <p className="lab">Пристрої · 1 · керований ресурс, а не я сама</p>
            <h1 style={{ fontSize: 18, marginTop: 2 }}>Через що я говорю.</h1>
            <p className="prose" style={{ fontSize: 13 }}>
              Один пристрій на столі, і місце для наступних. Памʼять одна — хто б не спитав, я
              відповідаю з того самого.
            </p>
          </div>
        </div>

        {loadError && (
          <p className="chat-status" role="alert">
            {loadError}
          </p>
        )}

        <div className="grid2">
          <div className="stack">
            <div className="panel lg">
              <div className="ph">
                <h2>Робочий стіл</h2>
                <span className="pill">ESP32-C3</span>
                {/* Deliberately not driven by `online` below. That is this
                    browser's own socket, not the device's, and a pill here
                    saying "на звʼязку" because the *browser* is connected
                    would be the page's most confident lie. Whether the
                    device is connected is a real answer now, and it is in
                    the Прошивка panel, from GET /firmware/device. */}
                <span className="pill" title="Стан пристрою - у панелі «Прошивка» нижче">
                  <span className="dot off" aria-hidden="true" />
                  див. Прошивку
                </span>
                <span className="cnt">device_id: {DEVICE_ID}</span>
              </div>
              <div className="pad" style={{ display: 'grid', gap: 16 }}>
                <div className="dev-two" style={{ display: 'grid', gap: 16, gridTemplateColumns: 'minmax(0,1fr)' }}>
                  <div style={{ minWidth: 0 }}>
                    <Face state={turn.state} emotion={lastEmotion(turn.turns)} online={online} />
                    <p className="cap">
                      Живий екран · 128×64 · 1 біт — той самий стан і настрій, що й у Розмові,
                      намальовані тим самим кодом, що й на панелі. Усього девʼять станів.
                    </p>
                  </div>

                  <div style={{ minWidth: 0 }}>
                    <div className="kv bare" style={{ gridTemplateColumns: 'repeat(2,minmax(0,1fr))' }}>
                      <div>
                        <span className="k">device_id</span>
                        <span className="v mono">{DEVICE_ID}</span>
                      </div>
                      <div>
                        <span className="k">persona</span>
                        <span className="v">{surfaces ? surfaces.esp32.name : '…'}</span>
                      </div>
                    </div>

                    <div className="surf" style={{ padding: '10px 0 0' }}>
                      <span className="sl">Змінити persona</span>
                      <select
                        aria-label="Персона для пристрою"
                        value={surfaces?.esp32.id ?? ''}
                        onChange={(event) => handlePersonaChange(Number(event.target.value))}
                        disabled={!roles || !surfaces || personaBusy}
                      >
                        {(roles ?? []).map((role) => (
                          <option key={role.id} value={role.id}>
                            {role.name}
                          </option>
                        ))}
                      </select>
                    </div>
                    {personaError && (
                      <p className="chat-status" role="alert">
                        {personaError}
                      </p>
                    )}
                    {personaNotice && <p className="chat-status">{personaNotice}</p>}

                    <div className="locked">
                      <span className="dot acc" aria-hidden="true" />
                      <p>
                        Зміниш persona — вона доїде до пристрою <b>з наступним питанням</b>.
                        Перезавантажувати нічого не треба.
                      </p>
                    </div>

                    <Firmware notifyUnauthorized={notifyUnauthorized} />

                    <div className="grp" style={{ marginTop: 12 }}>
                      <p className="lab" style={{ marginBottom: 2 }}>
                        Що є в пристрою
                      </p>
                      <div className="rows">
                        <div className="row">
                          <span className="nm" style={{ width: 44, flex: 'none' }}>
                            мік
                          </span>
                          <span className="txt g dim" style={{ fontSize: 12 }}>
                            кнопка натиснута — я слухаю
                          </span>
                        </div>
                        <div className="row">
                          <span className="nm" style={{ width: 44, flex: 'none' }}>
                            спік
                          </span>
                          <span className="txt g dim" style={{ fontSize: 12 }}>
                            відповідь озвучується вголос
                          </span>
                        </div>
                        <div className="row">
                          <span className="nm" style={{ width: 44, flex: 'none' }}>
                            OLED
                          </span>
                          <span className="txt g dim" style={{ fontSize: 12 }}>
                            128×64, один біт, очі за станом
                          </span>
                        </div>
                      </div>
                    </div>
                  </div>
                </div>
              </div>
            </div>
          </div>

          <div className="stack">
            <div className="panel note pad">
              <p className="lab" style={{ color: 'var(--accent)', marginBottom: 6 }}>
                Як приєднати ще один пристрій
              </p>
              <p className="prose" style={{ fontSize: 12 }}>
                Приєднання стається в прошивці, не тут: у ній зашиті адреса сервера й спільний
                DEVICE_TOKEN, обидва компілюються в <code className="mono">firmware/main/secrets.h</code>{' '}
                під час збирання. З браузера це не міняється, і кнопки, яка б це зробила, тут
                немає навмисно.
              </p>
              <div className="field">
                <label className="flab" htmlFor="dev-ws-url">
                  <span>адреса, на яку дивиться прошивка</span>
                  <span className="c">з цієї сторінки</span>
                </label>
                <input id="dev-ws-url" type="text" value={wsUrl} readOnly spellCheck={false} />
              </div>
              <ol className="steps" style={{ marginTop: 12 }}>
                <li>Впиши цю адресу й спільний DEVICE_TOKEN у secrets.h нового пристрою.</li>
                <li>Перепрошy його — прошивка сама відкриє звʼязок із цим сервером.</li>
                <li>
                  Реєстру пристроїв немає: він розділить ту саму persona esp32 і ту саму памʼять,
                  а не отримає власний рядок на цій сторінці.
                </li>
              </ol>
            </div>
            <div className="grp" style={{ marginTop: 4 }}>
              <p className="lab" style={{ marginBottom: 6 }}>
                Скільком пристроям я відповідаю
              </p>
              <p className="prose" style={{ fontSize: 12 }}>
                Пристроїв може бути скільки завгодно — памʼять лишиться одна. Сьогодні я знаю про
                дві поверхні: стіл (esp32) і цей браузер (web).
              </p>
            </div>
          </div>
        </div>
      </div>
    </section>
  )
}

export default Devices
