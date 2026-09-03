// The data layer for updating the device's firmware. Kept out of JSX for the
// same reason settingsApi.ts is: none of it needs a component to exist, and
// the header check in particular is logic three languages have to agree on.
//
// The shape of the feature, which explains why there is a GitHub call in
// here at all: the device never talks to GitHub. Releases live there, this
// page fetches the *release metadata* to say what the latest one is, the
// person downloads the .bin with an ordinary link, and then hands that file
// to push() - which sends it to our own server, which relays it to the
// device down the WebSocket the device already holds. See
// docs/superpowers/specs/2026-09-03-ota-firmware-update-design.md.

import { api } from './api'

// ------------------------------------------------------------ the image

/** How much of a file is needed to judge it. Same constant as the
 * firmware's OL_HEADER_MIN and server/firmware.py's HEADER_MIN. */
export const HEADER_MIN = 144

/** From project() in firmware/CMakeLists.txt. */
export const PROJECT_NAME = 'voice_capture'

/** esp_chip_id_t: ESP_CHIP_ID_ESP32C3. */
export const CHIP_ID_ESP32C3 = 5

/**
 * Which GitHub repository holds the releases, as "owner/name".
 *
 * Empty on purpose, and it is not a placeholder to be guessed at: this
 * project has no remote yet. While it is empty the panel says so rather than
 * showing a release row it cannot fill, because a page that reports on a
 * repository that does not exist is worse than one that admits it has
 * nothing to report. Fill this in when the repository is published; nothing
 * else has to change.
 */
export const GITHUB_REPO = ''

/**
 * Whether these bytes begin a firmware image for this project on this chip.
 * Null means valid; anything else is a reason fit to show a person.
 *
 * The same four offsets as ol_check_image() in the firmware and
 * check_image() in server/firmware.py - read out of a real
 * firmware/build/voice_capture.bin rather than inferred. This copy exists
 * only to fail fast before a megabyte goes up the wire; the server's is the
 * one that actually guards the device, and the firmware's is the one that
 * guards the flash.
 */
export function checkImage(head: Uint8Array): string | null {
  if (head.byteLength < HEADER_MIN) return 'too short to be firmware'

  const view = new DataView(head.buffer, head.byteOffset, head.byteLength)

  if (head[0x00] !== 0xe9) return 'not an ESP firmware image'
  if (view.getUint16(0x0c, true) !== CHIP_ID_ESP32C3) return 'built for another chip'
  if (view.getUint32(0x20, true) !== 0xabcd5432) return 'no application descriptor'

  // Over the terminator, so a name that merely starts with ours -
  // "voice_capture2" is a different project - does not pass.
  for (let i = 0; i < PROJECT_NAME.length; i++) {
    if (head[0x50 + i] !== PROJECT_NAME.charCodeAt(i)) return 'firmware for another project'
  }
  if (head[0x50 + PROJECT_NAME.length] !== 0) return 'firmware for another project'

  return null
}

// ------------------------------------------------------------ the release

export interface LatestRelease {
  tag: string
  /** Where a person downloads it from. An ordinary link, which is the whole
   * reason the browser does not fetch the bytes itself: GitHub's asset
   * downloads redirect to objects.githubusercontent.com, which does not
   * reliably allow cross-origin reads. A link is not subject to CORS. */
  url: string
  published: string
}

interface ReleaseAsset {
  name?: unknown
  browser_download_url?: unknown
}

/** Pulls the tag and the .bin out of a GitHub release body. Exported for
 * its own test: the shape is GitHub's, not ours, and it is worth pinning. */
export function latestFromRelease(body: unknown): LatestRelease | null {
  const release = body as
    | { tag_name?: unknown; published_at?: unknown; assets?: unknown }
    | null
    | undefined
  if (!release || typeof release.tag_name !== 'string') return null

  const assets: ReleaseAsset[] = Array.isArray(release.assets) ? release.assets : []
  const bin = assets.find(
    (asset) => typeof asset?.name === 'string' && asset.name.endsWith('.bin'),
  )
  if (!bin || typeof bin.browser_download_url !== 'string') return null

  return {
    tag: release.tag_name,
    url: bin.browser_download_url,
    published: typeof release.published_at === 'string' ? release.published_at : '',
  }
}

// ------------------------------------------------------------- the device

export interface DeviceFirmware {
  online: boolean
  version: string | null
}

// ------------------------------------------------------------- the upload

/**
 * Where the bytes are, and it matters which.
 *
 * `uploading` is the file leaving this browser for the server. `relaying`
 * is the server feeding it to the device. They are different distances over
 * different links, and neither of them is "the update worked" - that only
 * happens when the device reboots and reports a new version.
 */
export interface PushProgress {
  phase: 'uploading' | 'relaying'
  sent: number
  total: number
}

export interface PushOutcome {
  /** `ota_ready`, `ota_failed`, `timeout` or `error` - server/firmware.py's
   * own wording, passed through rather than translated, because a reason
   * this layer invented would be a reason nobody can act on. */
  outcome: string
  reason: string
}

function detailFrom(text: string, fallback: string): string {
  try {
    const body: unknown = JSON.parse(text)
    const detail = (body as { detail?: unknown } | null)?.detail
    if (typeof detail === 'string') return detail
  } catch {
    // Not JSON - a proxy's error page, say.
  }
  return text.trim() || fallback
}

/**
 * Sends the file to the server, which relays it to the device.
 *
 * XMLHttpRequest rather than fetch, for one reason: `upload.onprogress` is
 * the only way a browser will say how much of a megabyte has actually left,
 * and this is a megabyte over a home connection. fetch has no equivalent.
 *
 * The response is newline-delimited JSON that arrives as the push proceeds,
 * so it is read incrementally out of responseText. Only whole lines are
 * parsed - a chunk boundary lands in the middle of an object often enough
 * that parsing whatever has arrived would throw.
 */
function push(file: File, onProgress: (progress: PushProgress) => void): Promise<PushOutcome> {
  return new Promise<PushOutcome>((resolve, reject) => {
    const xhr = new XMLHttpRequest()
    xhr.open('POST', '/firmware/push')

    let consumed = 0
    let outcome: PushOutcome | null = null

    const handle = (line: string) => {
      let payload: unknown
      try {
        payload = JSON.parse(line)
      } catch {
        return // a line we cannot read is not a reason to fail the update
      }
      const record = payload as { sent?: unknown; total?: unknown; done?: unknown; outcome?: unknown; reason?: unknown }
      if (record.done === true) {
        outcome = {
          outcome: typeof record.outcome === 'string' ? record.outcome : 'unknown',
          reason: typeof record.reason === 'string' ? record.reason : '',
        }
        return
      }
      if (typeof record.sent === 'number' && typeof record.total === 'number') {
        onProgress({ phase: 'relaying', sent: record.sent, total: record.total })
      }
    }

    const drain = () => {
      const text = xhr.responseText
      for (;;) {
        const newline = text.indexOf('\n', consumed)
        if (newline === -1) return
        const line = text.slice(consumed, newline).trim()
        consumed = newline + 1
        if (line) handle(line)
      }
    }

    xhr.upload.onprogress = (event: ProgressEvent) => {
      if (event.lengthComputable) {
        onProgress({ phase: 'uploading', sent: event.loaded, total: event.total })
      }
    }

    xhr.onprogress = drain

    xhr.onload = () => {
      drain()
      if (xhr.status >= 400) {
        reject(new Error(detailFrom(xhr.responseText, `the server refused it (${xhr.status})`)))
        return
      }
      if (outcome === null) {
        // A 200 whose stream never carried a done line means the generator
        // died mid-flight. Resolving here would tell somebody an update
        // landed when nobody knows whether it did.
        reject(new Error('The server finished without saying what happened.'))
        return
      }
      resolve(outcome)
    }

    xhr.onerror = () => {
      reject(new Error('Could not reach the server.'))
    }

    xhr.send(file)
  })
}

export const firmwareApi = {
  device: (): Promise<DeviceFirmware> => api.get('/firmware/device'),

  /**
   * The newest release on GitHub, or null - which is the answer for a
   * repository that is not configured, has no releases yet, publishes no
   * .bin, or cannot be reached. None of those is an error worth a banner,
   * and a fresh fork is in three of them at once.
   *
   * No credentials go with this: it is cross-origin, so the browser sends
   * none by default, and it must stay that way.
   */
  latest: async (repo: string = GITHUB_REPO): Promise<LatestRelease | null> => {
    if (!repo) return null
    try {
      const res = await fetch(`https://api.github.com/repos/${repo}/releases/latest`, {
        headers: { Accept: 'application/vnd.github+json' },
      })
      if (!res.ok) return null
      return latestFromRelease(await res.json())
    } catch {
      return null
    }
  },

  push,
}
