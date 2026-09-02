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
//
// Task 6 moved the landing section from Розмова back to Головна (the
// client's own decision - see App.tsx's own comment on SignedIn). The app
// now lands on Home.tsx, not Chat.tsx, so this harness shoots *both*: Home
// first (its own composer is `#askInput`, not `.chat-input`), then clicks
// through to Розмова via Shell.tsx's own `data-section="talk"` nav button
// and re-runs the original composer-collapse / face-vs-conversation checks
// there, unweakened - those two are Chat.tsx's own layout, and Головна's
// ask control is a different element with a different width budget, so a
// failure of either still means what it always meant.
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

/** Runs the shared checks (overflow, an input not collapsed, its submit
 * button on-screen, and - only when both boxes are given - the face never
 * wider than the conversation) against whatever selectors the caller's
 * current view actually has, and screenshots it. */
async function shootView(page, { label, outPath, inputSel, formSel, faceSel, wideSel }) {
  const m = await page.evaluate(
    ({ inputSel, formSel, faceSel, wideSel }) => {
      const r = (s) => { const e = document.querySelector(s); return e ? e.getBoundingClientRect() : null }
      const send = formSel ? [...document.querySelectorAll(`${formSel} button`)].pop() : null
      const input = r(inputSel), face = faceSel ? r(faceSel) : null, wide = wideSel ? r(wideSel) : null
      return {
        overflow: document.documentElement.scrollWidth > window.innerWidth,
        inputWidth: input && Math.round(input.width),
        sendInside: send ? Math.round(send.getBoundingClientRect().right) <= window.innerWidth : null,
        faceWidth: face && Math.round(face.width),
        wideWidth: wide && Math.round(wide.width),
      }
    },
    { inputSel, formSel, faceSel, wideSel },
  )

  const bad = []
  if (m.overflow) bad.push('page scrolls horizontally')
  if (m.inputWidth !== null && m.inputWidth < 140) bad.push(`${label} input collapsed to ${m.inputWidth}px`)
  if (m.sendInside === false) bad.push(`${label} send is off the edge`)
  if (m.faceWidth && m.wideWidth && m.faceWidth > m.wideWidth) {
    bad.push(`${label} face (${m.faceWidth}px) is wider than its own column (${m.wideWidth}px)`)
  }
  console.log(`    ${label}: input=${m.inputWidth} face=${m.faceWidth} wide=${m.wideWidth}${bad.length ? `  <-- ${bad.join('; ')}` : ''}`)
  await page.screenshot({ path: outPath })
  return bad
}

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

      // Lands on Головна by default now - #askInput is Home.tsx's own ask
      // control (see its id, matching the approved mockup's own askInput).
      await page.waitForSelector('#askInput', { timeout: 20000 })
      // Wait for the socket, not just the markup. Without this every shot
      // catches "Connecting…" with the composer disabled and the face marked
      // OFFLINE, which looks exactly like a broken deploy and is not one.
      await page.waitForFunction(() => {
        const i = document.querySelector('#askInput')
        return i && !i.disabled
      }, { timeout: 30000 })
      await page.waitForFunction(litNow, { timeout: 25000 })

      console.log(`  ${scheme} ${name} ${width}x${height}:`)
      problems += (
        await shootView(page, {
          label: 'home', outPath: `${OUT}/${scheme}-${name}-home.png`,
          inputSel: '#askInput', formSel: '#askForm', faceSel: '.face-stage', wideSel: null,
        })
      ).length

      // Розмова: the original two checks this harness was built for (input
      // collapse, face vs. the conversation column) live here now, not on
      // Головна's own ask control - unchanged from before Task 6.
      //
      // Shell.tsx renders the same data-section="talk" button twice - once
      // in the sidebar `.nav`, once in the phone bottom bar `.tabs` - and
      // hides whichever one the viewport does not need with CSS, not by
      // removing it from the DOM. `:visible` picks whichever copy is
      // actually on screen instead of timing out on a hidden match.
      await page.click('[data-section="talk"]:visible')
      await page.waitForSelector('.chat-composer', { timeout: 20000 })
      await page.waitForFunction(() => {
        const i = document.querySelector('.chat-input')
        return i && !i.disabled
      }, { timeout: 30000 })
      await page.waitForFunction(litNow, { timeout: 25000 })

      problems += (
        await shootView(page, {
          label: 'talk', outPath: `${OUT}/${scheme}-${name}-talk.png`,
          inputSel: '.chat-input', formSel: '.chat-composer', faceSel: '.face-stage', wideSel: '.chat',
        })
      ).length
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
