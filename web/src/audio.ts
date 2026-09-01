// Capture and playback for the voice control (Mic.tsx) - the browser half of
// the exact protocol the ESP32 speaks, so the browser is another device on
// one protocol rather than a second one: 16 kHz, 16-bit signed, mono,
// little-endian raw PCM in both directions (see README.md's protocol table
// and server/config.py's sample_rate). No JSX here on purpose, same
// reasoning as api.ts and useTurn.ts: none of this needs it.
//
// Two independent pieces live here. Capture turns the microphone into the
// binary chunks useTurn.ts's sendVoiceChunk() streams out while the button is
// held. Playback turns the binary chunks useTurn.ts's onVoiceReply() delivers
// back into sound, starting before the reply is finished - see createPlayer's
// header for why that part is the one that actually matters.

export const SAMPLE_RATE = 16000

// ------------------------------------------------------------------ shared

/** Float32 samples in [-1, 1] to 16-bit signed little-endian - taken
 * verbatim from the task brief, unchanged. Clips rather than wraps at the
 * boundary: a mic sample (or a decoding artefact) can exceed ±1, and
 * wrapping would turn a loud moment into noise instead of a flat top. */
export function floatTo16LE(input: Float32Array): ArrayBuffer {
  const out = new DataView(new ArrayBuffer(input.length * 2))
  for (let i = 0; i < input.length; i++) {
    const s = Math.max(-1, Math.min(1, input[i]))
    out.setInt16(i * 2, s < 0 ? s * 0x8000 : s * 0x7fff, true)
  }
  return out.buffer
}

/** 16-bit signed little-endian back to Float32 samples in [-1, 1] - the
 * inverse of floatTo16LE, used by createPlayer() to turn a reply's PCM
 * frames into something an AudioBuffer can hold.
 *
 * No explicit return type here on purpose: TypeScript 5.7+ parameterises
 * TypedArrays over their backing buffer, and `new Float32Array(n)` infers
 * the specific `Float32Array<ArrayBuffer>` that AudioBuffer.copyToChannel
 * below requires - annotating this as the bare `Float32Array` alias would
 * widen it back to `Float32Array<ArrayBufferLike>` and fail that call. */
function int16LEToFloat(chunk: ArrayBuffer) {
  const view = new DataView(chunk)
  const sampleCount = Math.floor(chunk.byteLength / 2)
  const out = new Float32Array(sampleCount)
  for (let i = 0; i < sampleCount; i++) {
    const s = view.getInt16(i * 2, true)
    out[i] = s < 0 ? s / 0x8000 : s / 0x7fff
  }
  return out
}

/** Resamples by averaging whole groups of source samples into one output
 * sample, never by picking every Nth one - that aliases audibly (see the
 * task brief). `inputRate` is whatever the AudioContext actually granted -
 * Chrome commonly refuses a 16000 Hz request and hands back 48000 - and
 * `outputRate` is always SAMPLE_RATE. A no-op, returning `input` itself
 * unchanged, when the rates already match, which is what happens on any
 * browser that does honour the request. */
export function downsample(input: Float32Array, inputRate: number, outputRate: number): Float32Array {
  if (inputRate === outputRate) return input
  const ratio = inputRate / outputRate
  const outLength = Math.max(1, Math.round(input.length / ratio))
  const output = new Float32Array(outLength)
  for (let i = 0; i < outLength; i++) {
    const start = Math.floor(i * ratio)
    const end = Math.max(start + 1, Math.min(input.length, Math.floor((i + 1) * ratio)))
    let sum = 0
    for (let j = start; j < end; j++) sum += input[j]
    output[i] = sum / (end - start)
  }
  return output
}

// ------------------------------------------------------------------- errors

/** Turns a getUserMedia (or AudioContext/AudioWorklet) failure into a line
 * telling the person how to fix it, never a dead button - see Mic.tsx. The
 * DOMException names are the ones the spec defines for getUserMedia; a
 * secure-context failure (mic APIs simply absent - see startCapture below)
 * is reported the same way via a synthetic NotSupportedError so it gets the
 * one message a plain "could not access the microphone" would hide. */
export function describeMicError(err: unknown): string {
  const name = err instanceof DOMException ? err.name : ''
  switch (name) {
    case 'NotAllowedError':
    case 'SecurityError':
      return 'Microphone access is blocked. Allow it for this site in your browser settings, then try again.'
    case 'NotFoundError':
    case 'OverconstrainedError':
      return 'No microphone was found. Plug one in and try again.'
    case 'NotReadableError':
      return 'The microphone could not be started - another app may be using it.'
    case 'NotSupportedError':
      return 'Voice needs a secure connection: open this page over HTTPS, or use localhost, then try again.'
    default:
      return 'Could not access the microphone. Check your browser and system permissions and try again.'
  }
}

// ------------------------------------------------------------------ capture

export interface CaptureController {
  /** Tears down the whole capture chain: stops the worklet, releases the
   * mic (so the browser's recording indicator goes away), closes the
   * AudioContext. Idempotent-ish in practice - Mic.tsx only ever calls it
   * once per startCapture(), but every teardown step tolerates being called
   * on an already-stopped node. */
  stop: () => void
}

export interface CaptureCallbacks {
  /** One 16 kHz PCM16LE chunk, ready to hand straight to
   * useTurn.ts's sendVoiceChunk(). */
  onChunk: (chunk: ArrayBuffer) => void
  /** The worklet itself failed after capture had already started - a line
   * for Mic.tsx to show, same as a startCapture() rejection would produce. */
  onError: (message: string) => void
}

const WORKLET_NAME = 'voice-capture-processor'

// An AudioWorkletProcessor has to live in its own module, loaded via
// audioContext.audioWorklet.addModule(url) - it runs on the audio rendering
// thread, not this one. Built as a Blob URL rather than a static file under
// web/public so there is nothing else for the build to know about; the URL
// is created once and reused across every recording session; addModule()
// itself still has to be called per AudioContext; source, id est
// registerProcessor - each new context has its own worklet registry.
const WORKLET_SOURCE = `
class VoiceCaptureProcessor extends AudioWorkletProcessor {
  process(inputs) {
    const channel = inputs[0] && inputs[0][0]
    if (channel && channel.length > 0) {
      // process() reuses its buffers on the next call, so the data has to be
      // copied before it is handed across to the main thread.
      this.port.postMessage(channel.slice(0))
    }
    return true
  }
}
registerProcessor('${WORKLET_NAME}', VoiceCaptureProcessor)
`

let workletModuleUrl: string | null = null

async function ensureWorkletModule(context: AudioContext): Promise<void> {
  if (!workletModuleUrl) {
    workletModuleUrl = URL.createObjectURL(
      new Blob([WORKLET_SOURCE], { type: 'application/javascript' }),
    )
  }
  await context.audioWorklet.addModule(workletModuleUrl)
}

/** Starts capturing the microphone as 16 kHz PCM16LE chunks. An AudioWorklet,
 * not the deprecated ScriptProcessorNode - see the task brief. Throws (never
 * calls onError) for a failure that means recording never started at all -
 * a permission denial, no device, no secure context - so Mic.tsx's own
 * try/catch around the call is the one place that decides what the person
 * sees for that. onError is only for a failure once capture is already
 * under way. */
export async function startCapture({ onChunk, onError }: CaptureCallbacks): Promise<CaptureController> {
  if (!navigator.mediaDevices?.getUserMedia) {
    // A browser hides mediaDevices entirely off HTTPS/localhost, rather
    // than exposing it and failing the call - which otherwise reads as a
    // generic "could not access the microphone" with no way to tell it
    // apart from a real permission denial. See describeMicError's
    // NotSupportedError case.
    throw new DOMException(
      'getUserMedia requires a secure context (HTTPS or localhost)',
      'NotSupportedError',
    )
  }

  const stream = await navigator.mediaDevices.getUserMedia({
    audio: { channelCount: 1, echoCancellation: true, noiseSuppression: true },
  })

  let context: AudioContext
  let source: MediaStreamAudioSourceNode
  let node: AudioWorkletNode
  try {
    // A request, not a guarantee - see downsample() above for why every
    // chunk is resampled against whatever the browser actually granted
    // rather than trusted to already be 16000.
    context = new AudioContext({ sampleRate: SAMPLE_RATE })
    await ensureWorkletModule(context)
    source = context.createMediaStreamSource(stream)
    node = new AudioWorkletNode(context, WORKLET_NAME)
  } catch (err) {
    stream.getTracks().forEach((track) => track.stop())
    throw err
  }

  const actualRate = context.sampleRate

  node.port.onmessage = (event: MessageEvent<Float32Array>) => {
    try {
      onChunk(floatTo16LE(downsample(event.data, actualRate, SAMPLE_RATE)))
    } catch {
      onError('The microphone stopped unexpectedly. Try again.')
    }
  }
  node.onprocessorerror = () => {
    onError('The microphone stopped unexpectedly. Try again.')
  }

  source.connect(node)
  // Deliberately not connected to context.destination: the point is to
  // capture the mic, never to loop it back through the speakers.

  return {
    stop: () => {
      node.port.onmessage = null
      node.onprocessorerror = null
      source.disconnect()
      node.disconnect()
      stream.getTracks().forEach((track) => track.stop())
      void context.close()
    },
  }
}

// ----------------------------------------------------------------- playback

export interface Player {
  /** One binary frame of the reply, PCM16LE at SAMPLE_RATE - see
   * server/session.py's `render`. Schedules it right after whatever is
   * already queued and starts the AudioContext playing immediately if
   * nothing was queued yet, which is the entire point: the first frame
   * plays before the reply has finished streaming, rather than waiting for
   * `done`. */
  push: (chunk: ArrayBuffer) => void
  /** The reply's `done` frame arrived - no more chunks are coming for it.
   * Whatever is already scheduled keeps playing on its own; this exists so
   * a caller can tell "finished normally" apart from "cut off", not to stop
   * anything. */
  finish: () => void
  /** Cuts off playback immediately - an interrupt, not a graceful end. Used
   * for "a new question just cancelled this reply", never for the ordinary
   * end of one. Closes the AudioContext; this Player is spent afterward. */
  stop: () => void
  /** Whether any scheduled audio has not finished playing yet. */
  isPlaying: () => boolean
}

/** Queues 16 kHz mono PCM16LE frames and plays them back to back through the
 * Web Audio API, one AudioBuffer per frame scheduled immediately after the
 * one before it. Starting the first frame before the rest of the reply has
 * even been generated is the entire latency argument of this project - see
 * server/session.py's playback_lead_s pacing, which exists to keep the
 * server just ahead of what this can have played by now, not further.
 *
 * A gap between frames arriving does not end playback early: the next
 * frame is simply scheduled to start whenever it arrives (immediately, if
 * that time has already passed - see source.start()'s own handling of a
 * past start time), never treated as "the reply must be over". Only stop()
 * (an interrupt) or the frames actually running out ends it.
 *
 * Each AudioBuffer is created with SAMPLE_RATE explicitly, independent of
 * whatever rate this AudioContext itself runs at - the Web Audio API
 * resamples a buffer to the context's rate during playback on its own, so
 * nothing here has to do that math the way capture's downsample() must. */
export function createPlayer(): Player {
  const context = new AudioContext()
  // A browser can hand back an AudioContext already 'suspended' unless its
  // autoplay heuristic is satisfied - this one is created asynchronously
  // (on the first reply chunk, not inside the button press itself), so it
  // is not guaranteed to qualify on its own. resume() is a no-op if the
  // context is already running, and Chrome's own "sticky user activation"
  // (the mic press moments earlier) is normally enough for this to succeed
  // without prompting anything further - but calling it costs nothing
  // either way and closes the gap if it is not.
  if (context.state === 'suspended') void context.resume()
  let nextStartTime = 0
  const active = new Set<AudioBufferSourceNode>()

  function push(chunk: ArrayBuffer): void {
    if (chunk.byteLength < 2) return // an empty frame has nothing to schedule
    const samples = int16LEToFloat(chunk)
    const buffer = context.createBuffer(1, samples.length, SAMPLE_RATE)
    buffer.copyToChannel(samples, 0)

    const source = context.createBufferSource()
    source.buffer = buffer
    source.connect(context.destination)
    source.onended = () => active.delete(source)

    const startAt = Math.max(nextStartTime, context.currentTime)
    source.start(startAt)
    nextStartTime = startAt + buffer.duration
    active.add(source)
  }

  function finish(): void {
    // Nothing to do: whatever is scheduled already plays itself out.
  }

  function stop(): void {
    for (const source of active) {
      try {
        source.stop()
      } catch {
        // Already stopped, or never actually started - harmless either way.
      }
    }
    active.clear()
    nextStartTime = 0
    void context.close()
  }

  return { push, finish, stop, isPlaying: () => active.size > 0 }
}
