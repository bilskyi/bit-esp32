# Design brief — voice companion playground

Paste this whole file as the prompt. It is written to be handed to a fresh
session or another tool with no knowledge of the project.

---

You are the design lead for a small studio. Produce **three genuinely
different visual directions** for the web app described below, then build each
one as a real, self-contained HTML page I can open — not a description, not a
mood board. The client has already rejected the current design in these words:
*"too minimalistic and childish, like a simple kid's project"*, and asked for
**side navigation** and something **more modern**. Take that seriously: the
brief is not "tidy up", it is "give this a point of view".

## What the product actually is

A voice companion. A physical ESP32-C3 device sits on a desk: a microphone, a
speaker, a button, and a **128×64 one-bit OLED showing a pair of animated
eyes** — its face. You hold the button, speak, and it answers aloud in
Ukrainian, Russian or English, streaming each sentence as soon as it is ready
because latency is the whole engineering argument of the project. It remembers
durable facts about its owner in a database and ranks them by embedding
similarity to whatever you just asked.

This web app is the **playground** for that device: where you talk to the same
assistant by typing, watch what it is doing internally, and configure its
personality. The app and the device are deliberately *one assistant* sharing
one memory — not two products.

The audience is exactly one person: the engineer who built the device. They
are technical, they read numbers, and they want to see the machine working.
This is an instrument, not a consumer chat app — but it should feel like a
well-made instrument, not a debug dump.

## The screens, and the real content in them

**1. Sign in.** One account. Username, password, one generic failure line
(the server deliberately does not say which field was wrong).

**2. Chat** — the main surface.
- A transcript of turns. A question from the person, then the assistant's
  reply. Replies arrive **one sentence at a time**, visibly. Text is usually
  Ukrainian or Russian, sometimes English, 1–6 sentences.
- **The device's face**, live: the same pixel eyes the OLED shows, driven by
  the connection state (`idle` / `listening` / `thinking` / `speaking`) and by
  an emotion the model tags every reply with (one of nine: neutral, happy,
  excited, curious, confused, surprised, sad, annoyed, sleepy).
- **An inspector under every answer** — the reason this app exists. Collapsed,
  it is one line of figures: `emotion happy · 335 ms reply · 330 tok in ·
  6 facts`. Expanded it shows:
  - the **retrieved facts with their cosine similarity scores**, best first,
    e.g. `Саша — 0.2410`, `Uses a mock LLM for local testing — 0.9227`. The
    *gap* between scores is the informative thing.
  - the **fully assembled system prompt** actually sent to the model —
    typically 300–500 words of rules plus remembered facts. Long by design.
  - the rest: role name, surface, tokens in and out, speech-to-text
    milliseconds when the question was spoken.
- A composer: a text box, a **press-and-hold "speak" control** mirroring the
  device's own button, and send.

**3. Settings.**
- **Roles.** The assistant's personality as editable records. Each has a name,
  a persona prompt (up to 2000 chars), a max-sentence count, whether markdown
  is allowed, which of three languages it may use, and an optional **pinned
  mood** from those same nine. Two roles are built in and cannot be renamed or
  deleted. Each of the two surfaces — the device, and this browser — points at
  one role, so they can differ.
- **Memory.** A list of what the assistant remembers. Each row: the text, a
  tag saying whether the assistant extracted it itself or the person typed it
  as a standing instruction, and the date it arrived. Rows can be deleted.
  Two destructive actions behind typed confirmation.
- **Recording.** Whether conversations are stored, and for how many days.

## Hard constraints — a direction that breaks one of these is unusable

1. **Cyrillic is mandatory.** Most of the content is Ukrainian and Russian.
   Every typeface you choose must have real Cyrillic coverage — this rules out
   a great many display faces, and it is not negotiable. Name the fonts and
   confirm their Cyrillic support.
2. **Light and dark, both first-class.** Not an inverted afterthought.
3. **320px to 1680px.** The client uses it on a phone. Nothing may overflow
   horizontally at 320px. Side navigation must have an honest answer for
   phone width — say what it is.
4. **The pixel face stays pixel-accurate.** It is generated from the
   firmware's own C so the browser and the panel cannot disagree: a 128×64
   one-bit bitmap, scaled up with hard pixel edges. You may design the housing
   it sits in, the scale, the glow, the framing — you may not redraw the eyes,
   smooth them, or replace them with an icon or an emoji.
5. **Numbers must stay readable as numbers.** Scores, milliseconds and token
   counts want tabular alignment. A proportional font that makes `0.2410` and
   `0.9227` hard to compare defeats the inspector.
6. **The long prompt needs somewhere to live.** 300–500 words of monospaced
   rules has to be inspectable without wrecking the layout.
7. Stack is React + Vite with hand-written CSS. You may propose a CSS
   framework or a component library, but say what it buys and what it costs.

## What is being rejected, specifically

The current version is: everything monospace, a near-white or near-black flat
field, hairline borders, two tabs as a thin strip under a plain header, and
very little else. It reads as unfinished and generic. In particular —

- **no visual hierarchy**: a remembered fact, a reply and a token count all
  have nearly the same weight;
- **no depth or material**: flat panels, one hairline, no sense of surface;
- monospace *everywhere*, including paragraphs of Cyrillic prose, which is
  both tiring to read and the main reason it looks like a terminal toy;
- the navigation is an afterthought;
- a lot of empty grey on a wide screen.

Keep only this, and only if you want it: the current palette was derived from
the hardware itself — *"an SSD1306 is near-black glass emitting a cool white,
so the neutrals carry a blue bias and the only warm thing is the amber that
means running"*. That is a real idea about the subject. You may build on it,
or deliberately reject it and say why.

## What I want back

For each of the **three directions**:

1. **A name and a one-paragraph thesis** — what this direction believes about
   the product, and who it would feel right to.
2. **Tokens**: 5–7 named colours as hex, for light and dark; the typeface
   pairing with roles (display / prose / numeric) and confirmed Cyrillic
   coverage; a type scale; spacing and radius decisions.
3. **The navigation answer**, including phone width.
4. **One signature element** — the single thing this design is remembered by.
   It should come from the subject's own world: a desk device with pixel eyes,
   speech in three languages, similarity scores, latency in milliseconds. Not
   a gradient, not a glow, not a floating card.
5. **A working page**: one self-contained HTML file per direction, inline CSS,
   no build step, showing the **Chat screen with a real conversation in
   Ukrainian, an expanded inspector with three scored facts and a visible
   prompt excerpt, the pixel face, and the composer** — plus the Settings
   screen's role editor. Use realistic content, not lorem ipsum. Include a
   light/dark toggle. It must survive being resized to 320px.

Make the three directions actually different — different structural idea,
not one layout with three palettes. If two of them could be swapped by
changing hex values, you have produced one direction.

## Avoid these, they are the current defaults of AI-generated design

Do not hand me: a warm cream background with a high-contrast serif and a
terracotta accent; a near-black page with a single acid-green or vermilion
accent; a broadsheet of hairline rules with zero border-radius and dense
newspaper columns; glassmorphism; a purple-to-blue gradient hero. If a
direction lands on one of these, say so and justify it against this brief
specifically, or change it.

Before you build, state your three theses in a few sentences each and let me
pick — or say plainly that you are confident enough to build all three.
