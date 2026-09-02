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

## What the product actually is — read this twice

**This is one person's personal assistant.** Not a dashboard for a gadget.
The owner calls it his JARVIS and means it: a single assistant he talks to
from wherever he is, that knows him, remembers what he has told it, can read
documents he gives it, can reach into services he has connected, and speaks
through whatever hardware happens to be nearby.

**An earlier version of this brief got this wrong and produced three rejected
designs.** It said "this web app is the playground for that device", so all
three made a 128×64 OLED the hero of the page and the assistant a feature of
it. The correction, in the client's words: *"this should be a full ecosystem
for everything, not just the ESP32. Yes, I must be able to add devices and
configure that, but first and foremost this is my personal JARVIS."*

So: **the assistant is the subject. Devices are a managed resource** — one
section among several, the way a phone's Bluetooth screen is a section and
not the phone's identity.

### The assistant

It has a personality you can edit and switch (several saved personas, one
active per surface, an optional pinned mood). It answers in Ukrainian,
Russian or English, detecting which you used. It remembers durable facts
about you and retrieves them by embedding similarity, and it shows its work:
which facts it reached for, how strongly, and the exact prompt it assembled.
Answers stream a sentence at a time.

### What it reaches into — most of this is not built yet, and the design must
### leave room for it rather than bolt it on later

- **Memory** — facts it extracted from conversations, plus standing
  instructions the owner typed. Exists today.
- **Documents** — the owner wants to hand it PDFs, notes, exported chats
  with friends, and have it answer from them. Real retrieval over uploaded
  material. Not built.
- **Connections** — MCP servers and integrations, added by the owner with a
  connect button and configured per connection: a calendar it can schedule
  into, whatever else he wires up. Not built. This is the largest missing
  piece and the design must have an honest home for it.
- **Devices** — today one ESP32-C3 with a mic, speaker, button and a 128×64
  one-bit OLED showing animated eyes; the owner wants to add more and
  configure each. A device has a name, a state, health, an assigned
  personality, and its own screen. Not a fleet console — a small number of
  personal objects.
- **Activity** — who has been talking to it, what was asked, cost against a
  free-tier budget, latency. Conversations are recorded; nothing charts them
  yet.

### The surfaces

The same assistant, one memory, several ways in: this web app on a laptop and
a phone, and the desk device by voice. A person switching between them is
continuing one conversation with one assistant, not using two products.

## What the front door should feel like

Opening this app should feel like arriving at *your assistant* — able to talk
to it immediately — with everything it can do and everything it knows one
move away. It should not feel like a settings screen for a microcontroller,
and it should not feel like a generic AI chat wrapper either: the reason this
exists is that it is *his*, it knows him, and he can see and change how it
thinks.

## Two decisions the client has made — do not relitigate them

**The front door is a home that greets you.** Opening the app shows the
assistant as *present and already running*: a greeting that knows the time
and the person, what it has done since they last spoke, anything waiting, and
a place to talk right there. Talking is one keystroke away, but the first
thing on screen is that it exists and has been working. The client picked
this over opening straight into a transcript and over a command bar. Their
chosen sketch:

    ┌──────────────────────────────────────────────┐
    │  Доброго ранку, Саша.          ● на звʼязку  │
    │                                              │
    │  Since we last spoke I learned 2 things      │
    │  about you and answered 6 questions.         │
    │                                              │
    │  ┌────────────────────────────────────────┐  │
    │  │ Ask me anything…                    ▸  │  │
    │  └────────────────────────────────────────┘  │
    │                                              │
    │  KNOWS 47        CONNECTED 3      DEVICES 1  │
    │  12 documents    calendar, …      desk ● idle│
    └──────────────────────────────────────────────┘

Treat that as the *idea*, not the layout: a greeting, a status, an immediate
way to talk, and the ecosystem present as live state rather than as a menu.
Compose it properly.

**The character is a presence with a cockpit behind it.** The assistant should
feel like *someone*: warm surfaces, its own voice in the copy, personality
visible up front. The instrumentation — retrieval scores, the assembled
prompt, latency, cost — is one deliberate move away and done seriously when
you get there. Not a cold cockpit throughout (that is what was just
rejected), and not so ambient that the internals disappear, because those
internals are the best thing about the product.

## The sections

One navigation, six places. Devices is one of them, not the frame.

| Section | What lives there |
|---|---|
| **Talk** | The conversation. Chat and voice. Answers carry "why this answer?". |
| **Knowledge** | Everything it knows: facts it extracted, standing instructions typed by the owner, and uploaded documents. One place — to the assistant they are all retrieval sources. |
| **Connections** | MCP servers and integrations. Connect buttons, per-connection configuration. Mostly empty today, and it must look deliberately empty rather than broken. |
| **Devices** | The desk device today, more later. Each one a personal object: name, state, health, which personality it runs. Plus adding one. |
| **Personality** | Saved personas, the active one per surface, pinned mood. |
| **Activity** | Conversations, what was asked, cost against the free tier, latency. |

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
