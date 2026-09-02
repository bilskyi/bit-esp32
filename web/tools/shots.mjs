// Screenshot the app at several viewports, and measure the things a
// screenshot alone will not tell you.
//
// This exists because two rounds of visual bugs shipped past a green test
// suite, a clean typecheck and a lint pass: a composer whose input collapsed
// to one character wide below ~600px, and a grid that handed the face the
// wide column and the conversation a 15rem sidebar. Neither is expressible
// as a unit test and both are obvious in a picture.
//
//   npm --prefix web run shots           # against a server you already run
//   BASE=http://127.0.0.1:8000 npm --prefix web run shots
//
// Needs an account: create one with
//   uv run python scripts/create_account.py --username shot --password shotpass123
// or pass USER=/PASS=.
import { chromium } from 'playwright'
import { mkdir } from 'node:fs/promises'

const BASE = process.env.BASE ?? 'http://127.0.0.1:8000'
const USER = process.env.USER_NAME ?? 'shot'
const PASS = process.env.PASS ?? 'shotpass123'
const OUT = process.env.OUT ?? 'shots'

// The eyes are fetched lazily, so "is the canvas in the DOM" is not the same
// question as "has it drawn anything".
const litNow = () => {
  const c = document.querySelector('.face-canvas')
  if (!c) return false
  const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data
  for (let i = 0; i < d.length; i += 4) {
    if (d[i + 3] > 0 && d[i] + d[i + 1] + d[i + 2] > 200) return true
  }
  return false
}

const VIEWPORTS = [
  ['phone-320', 320, 640],
  ['phone', 390, 844],
  ['tablet', 768, 1024],
  ['desktop', 1280, 800],
  ['wide', 1680, 1050],
]

await mkdir(OUT, { recursive: true })
const browser = await chromium.launch()
let problems = 0

for (const scheme of ['light', 'dark']) {
  for (const [name, width, height] of VIEWPORTS) {
    const page = await browser.newPage({
      viewport: { width, height }, deviceScaleFactor: 2, colorScheme: scheme,
    })
    try {
      await page.goto(BASE, { waitUntil: 'networkidle' })
      await page.fill('#login-username', USER)
      await page.fill('#login-password', PASS)
      await page.click('form button[type=submit]')
      await page.waitForSelector('.chat-composer', { timeout: 20000 })
      await page.waitForFunction(litNow, { timeout: 25000 })

      const m = await page.evaluate(() => {
        const r = (s) => { const e = document.querySelector(s); return e ? e.getBoundingClientRect() : null }
        const send = [...document.querySelectorAll('.chat-composer button')].pop()
        const input = r('.chat-input'), face = r('.face-stage'), chat = r('.chat')
        return {
          overflow: document.documentElement.scrollWidth > window.innerWidth,
          inputWidth: input && Math.round(input.width),
          sendInside: send ? Math.round(send.getBoundingClientRect().right) <= window.innerWidth : null,
          faceWidth: face && Math.round(face.width),
          chatWidth: chat && Math.round(chat.width),
        }
      })

      // The two failures that actually happened, asserted rather than eyeballed.
      const bad = []
      if (m.overflow) bad.push('page scrolls horizontally')
      if (m.inputWidth !== null && m.inputWidth < 140) bad.push(`input collapsed to ${m.inputWidth}px`)
      if (m.sendInside === false) bad.push('Send is off the edge')
      if (m.faceWidth && m.chatWidth && m.faceWidth > m.chatWidth) {
        bad.push(`face (${m.faceWidth}px) is wider than the conversation (${m.chatWidth}px)`)
      }
      problems += bad.length
      const flag = bad.length ? `  <-- ${bad.join('; ')}` : ''
      console.log(`  ${scheme} ${name} ${width}x${height}: input=${m.inputWidth} face=${m.faceWidth} chat=${m.chatWidth}${flag}`)
      await page.screenshot({ path: `${OUT}/${scheme}-${name}.png` })
    } catch (err) {
      problems += 1
      console.log(`  ${scheme} ${name}: FAILED - ${String(err.message).split('\n')[0]}`)
    } finally {
      await page.close()
    }
  }
}
await browser.close()
console.log(problems ? `\n${problems} problem(s) found` : '\nno layout problems found')
process.exit(problems ? 1 : 0)
