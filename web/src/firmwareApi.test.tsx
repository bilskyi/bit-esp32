// Narrow on purpose, same reasoning as settingsApi.test.tsx: this is logic a
// click-through cannot falsify. A 144-byte header check, a GitHub response
// shape, and an XHR that has to report two different kinds of progress are
// all invisible in the UI until they are wrong.
import { afterEach, describe, expect, it, vi } from 'vitest'
import { checkImage, firmwareApi, latestFromRelease } from './firmwareApi'

// The same four offsets the firmware's ol_check_image() reads and
// server/firmware.py's check_image() reads. Read out of a real
// firmware/build/voice_capture.bin, not inferred - so if this helper is
// wrong, all three copies are wrong together and the C host test says so.
function firmware(size = 400, version = 'v0.2.1'): Uint8Array<ArrayBuffer> {
  const buffer = new ArrayBuffer(size)
  const bytes = new Uint8Array(buffer)
  const view = new DataView(buffer)
  bytes[0x00] = 0xe9
  view.setUint16(0x0c, 5, true) // chip_id: ESP32-C3
  view.setUint32(0x20, 0xabcd5432, true) // esp_app_desc magic
  for (let i = 0; i < version.length; i++) bytes[0x30 + i] = version.charCodeAt(i)
  const name = 'voice_capture'
  for (let i = 0; i < name.length; i++) bytes[0x50 + i] = name.charCodeAt(i)
  return bytes
}

describe('checkImage', () => {
  it('accepts a real header', () => {
    expect(checkImage(firmware())).toBeNull()
  })

  it('rejects a buffer too short to judge', () => {
    expect(checkImage(firmware().slice(0, 143))).toMatch(/short/)
  })

  it('rejects a zip', () => {
    const bad = firmware()
    bad[0] = 'P'.charCodeAt(0)
    expect(checkImage(bad)).toMatch(/ESP/)
  })

  it('rejects a build for another chip', () => {
    const bad = firmware()
    new DataView(bad.buffer).setUint16(0x0c, 9, true)
    expect(checkImage(bad)).toMatch(/chip/)
  })

  it('rejects a file with no application descriptor', () => {
    const bad = firmware()
    new DataView(bad.buffer).setUint32(0x20, 0, true)
    expect(checkImage(bad)).toMatch(/descriptor/)
  })

  it('rejects firmware from another project', () => {
    const bad = firmware()
    bad.fill(0, 0x50, 0x70)
    const other = 'other_app'
    for (let i = 0; i < other.length; i++) bad[0x50 + i] = other.charCodeAt(i)
    expect(checkImage(bad)).toMatch(/another project/)
  })

  it('rejects a name that only starts with ours', () => {
    const bad = firmware()
    const longer = 'voice_capture2'
    for (let i = 0; i < longer.length; i++) bad[0x50 + i] = longer.charCodeAt(i)
    expect(checkImage(bad)).toMatch(/another project/)
  })
})

describe('latestFromRelease', () => {
  it('picks the tag and the .bin asset', () => {
    const release = {
      tag_name: 'v0.3.0',
      published_at: '2026-09-03T10:00:00Z',
      assets: [
        { name: 'notes.txt', browser_download_url: 'https://example.test/notes.txt' },
        { name: 'voice_capture.bin', browser_download_url: 'https://example.test/fw.bin' },
      ],
    }
    expect(latestFromRelease(release)).toEqual({
      tag: 'v0.3.0',
      url: 'https://example.test/fw.bin',
      published: '2026-09-03T10:00:00Z',
    })
  })

  it('returns null when the release carries no .bin', () => {
    expect(
      latestFromRelease({ tag_name: 'v0.3.0', published_at: '', assets: [] }),
    ).toBeNull()
  })

  it('returns null for a body that is not a release at all', () => {
    expect(latestFromRelease(null)).toBeNull()
    expect(latestFromRelease({ message: 'Not Found' })).toBeNull()
  })
})

describe('firmwareApi.latest', () => {
  afterEach(() => {
    vi.unstubAllGlobals()
  })

  it('returns null without asking anything when no repository is configured', async () => {
    const fetchSpy = vi.fn()
    vi.stubGlobal('fetch', fetchSpy)
    expect(await firmwareApi.latest('')).toBeNull()
    expect(fetchSpy).not.toHaveBeenCalled()
  })

  it('returns null rather than throwing when the repository has no releases', async () => {
    // A fresh fork has no releases, and that is not an error worth a banner.
    vi.stubGlobal(
      'fetch',
      vi.fn().mockResolvedValue({ ok: false, status: 404, json: async () => ({}) }),
    )
    expect(await firmwareApi.latest('someone/voice')).toBeNull()
  })

  it('returns null rather than throwing when GitHub cannot be reached', async () => {
    vi.stubGlobal('fetch', vi.fn().mockRejectedValue(new TypeError('offline')))
    expect(await firmwareApi.latest('someone/voice')).toBeNull()
  })

  it('reads a release when there is one', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn().mockResolvedValue({
        ok: true,
        status: 200,
        json: async () => ({
          tag_name: 'v1.0.0',
          published_at: '2026-09-01T00:00:00Z',
          assets: [{ name: 'app.bin', browser_download_url: 'https://example.test/app.bin' }],
        }),
      }),
    )
    expect(await firmwareApi.latest('someone/voice')).toEqual({
      tag: 'v1.0.0',
      url: 'https://example.test/app.bin',
      published: '2026-09-01T00:00:00Z',
    })
  })
})

// A stand-in for the browser's own XHR. push() uses XMLHttpRequest rather
// than fetch for one reason - upload.onprogress is the only way to see a
// megabyte leaving the browser - and that is exactly the part a fetch mock
// could not exercise.
class FakeXHR {
  static last: FakeXHR | null = null
  method = ''
  url = ''
  status = 0
  responseText = ''
  upload = { onprogress: null as ((e: ProgressEvent) => void) | null }
  onprogress: (() => void) | null = null
  onload: (() => void) | null = null
  onerror: (() => void) | null = null
  sent: unknown = null

  constructor() {
    FakeXHR.last = this
  }

  open(method: string, url: string) {
    this.method = method
    this.url = url
  }

  send(body: unknown) {
    this.sent = body
  }

  /** Pretend the browser finished sending `loaded` of `total` bytes. */
  uploaded(loaded: number, total: number) {
    this.upload.onprogress?.({ lengthComputable: true, loaded, total } as ProgressEvent)
  }

  /** Pretend another line of the ndjson progress stream arrived. */
  streamed(line: string) {
    this.responseText += line + '\n'
    this.onprogress?.()
  }

  finish(status = 200) {
    this.status = status
    this.onload?.()
  }
}

describe('firmwareApi.push', () => {
  afterEach(() => {
    vi.unstubAllGlobals()
    FakeXHR.last = null
  })

  function start(file: Blob) {
    vi.stubGlobal('XMLHttpRequest', FakeXHR)
    const seen: { phase: string; sent: number; total: number }[] = []
    const promise = firmwareApi.push(file as File, (p) => seen.push({ ...p }))
    return { promise, seen, xhr: FakeXHR.last as FakeXHR }
  }

  it('posts the file itself as the body', () => {
    const file = new Blob([firmware()])
    const { xhr } = start(file)
    expect(xhr.method).toBe('POST')
    expect(xhr.url).toBe('/firmware/push')
    expect(xhr.sent).toBe(file)
  })

  it('reports the upload and the relay as two different phases', async () => {
    const { promise, seen, xhr } = start(new Blob([firmware()]))

    xhr.uploaded(50, 100)
    xhr.streamed(JSON.stringify({ sent: 4096, total: 8192 }))
    xhr.streamed(JSON.stringify({ sent: 8192, total: 8192 }))
    xhr.streamed(JSON.stringify({ done: true, outcome: 'ota_ready', reason: '' }))
    xhr.finish()

    await expect(promise).resolves.toEqual({ outcome: 'ota_ready', reason: '' })

    // "The bytes reached the server" and "the device took them" are not the
    // same claim, and the panel shows them separately.
    expect(seen).toEqual([
      { phase: 'uploading', sent: 50, total: 100 },
      { phase: 'relaying', sent: 4096, total: 8192 },
      { phase: 'relaying', sent: 8192, total: 8192 },
    ])
  })

  it('tolerates a partial line, since the stream is read as it arrives', async () => {
    const { promise, seen, xhr } = start(new Blob([firmware()]))

    // Two chunks that split a JSON object down the middle. Parsing whatever
    // responseText happens to hold would throw here.
    xhr.responseText = '{"sent": 4096, "to'
    xhr.onprogress?.()
    expect(seen).toEqual([])
    xhr.streamed('tal": 8192}')
    expect(seen).toEqual([{ phase: 'relaying', sent: 4096, total: 8192 }])

    xhr.streamed(JSON.stringify({ done: true, outcome: 'ota_ready', reason: '' }))
    xhr.finish()
    await promise
  })

  it('resolves with the device refusal rather than throwing', async () => {
    const { promise, xhr } = start(new Blob([firmware()]))
    xhr.streamed(JSON.stringify({ done: true, outcome: 'ota_failed', reason: 'busy talking' }))
    xhr.finish()
    await expect(promise).resolves.toEqual({ outcome: 'ota_failed', reason: 'busy talking' })
  })

  it('rejects with the server message when the request itself fails', async () => {
    const { promise, xhr } = start(new Blob([firmware()]))
    xhr.responseText = JSON.stringify({ detail: 'not an ESP firmware image' })
    xhr.finish(400)
    await expect(promise).rejects.toThrow(/not an ESP firmware image/)
  })

  it('rejects when the connection drops', async () => {
    const { promise, xhr } = start(new Blob([firmware()]))
    xhr.onerror?.()
    await expect(promise).rejects.toThrow(/reach/)
  })

  it('rejects a 200 that never carried an outcome', async () => {
    // The stream ending without a done line means the generator died
    // mid-flight. Resolving as success there would tell somebody an update
    // landed when nobody knows whether it did.
    const { promise, xhr } = start(new Blob([firmware()]))
    xhr.streamed(JSON.stringify({ sent: 10, total: 20 }))
    xhr.finish(200)
    await expect(promise).rejects.toThrow(/without saying/)
  })
})
