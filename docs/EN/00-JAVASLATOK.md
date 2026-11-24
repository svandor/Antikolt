# 00 — Proposals: a review of the Gemini plan

This document reviews the `docs/forras/gemini-terv-v1.odt` plan: what in it is good, what is wrong,
what is missing, and what I propose instead. The proposals already take into account your answers to the design
questions.

## 0. Your decisions (the plan builds on these)

| Question | Decision |
|---|---|
| Developer | **the assistant (AI agent)**; the main criteria are **stability and low resource usage** (Q1) |
| Target machine | **VECTOR**: Intel Core Ultra 9 275HX (24 threads), 32 GB RAM, RTX 5080 Laptop 16 GB + Intel Arc iGPU, 2 displays, 1 TB SSD — `09-CELHARDVER.md` |
| Engine | **Native Rust core + Qt 6 shell + CEF**, with an engine-adapter layer for future engines |
| Engine coupling | Windows: native window; Linux/Wayland: OSR — `11-OSR.md` |
| Resource behavior | **profiles** (Fast / Balanced / Economical / Ultra / Manual), not a single switch (Q4) |
| When leaving a card | **Disk hibernation**: save everything, but nothing in RAM; tens of GB on disk is fine |
| 10 minutes | The engine shuts down (the card merges back into the home page) |
| 24 hours | **`keep` / `sweep` / `wipe` policy**: the login **stays by default**, only by-products are deleted; full deletion is an option — `10-MUNKAMENET.md` |
| Home page | Simple card grid **and** a freely editable web page into which cards can be inserted; the home page can look like anything |
| v1 scope | card grid + preview + search; live view lifecycle; containers; vault + TOTP + autofill; **light / gray / dark** themes; memory measurement and CI gate |
| Dark mode | One method is not enough: a set of **predefined darkening techniques** is needed |

---

## 1. What is good in the Gemini plan, and stays

1. **The "single live view" principle.** This is the core of the plan, and it is correct. Most of the memory is not in the browser
   itself, but in the live renderers: a page can be between 80 MB (static article) and 500 MB (video SPA).
   One live engine at a time is the single biggest memory win.
2. **Identity containers.** Correct model: an isolated storage partition per account. This is the only correct solution for multi-account use
   (not incognito windows).
3. **SQLite + on-disk preview images.** A good base; to be extended (see P11).
4. **System-level encryption for the vault** (Windows DPAPI / Linux Secret Service). Good direction; the details
   are missing, however (P9).
5. **Three-zone interface** (left navigation, top control bar, central workspace). Reasonable; the top bar should
   be kept to a minimum, because every pixel takes away from the content.

### 1.1. What we deliberately left out of the Gemini plan

Not every difference is a mistake: the parts below were **replaced** by a better solution, and I record that here so that
later it does not look as if we forgot them.

| Part of the Gemini plan | Why it did not stay | Instead |
|---|---|---|
| Two-level preload (browser UI ↔ main process, page watcher) | Electron-specific concept; in the native shell the boundary is the FFI between the Qt shell and the Rust core, and the engine's isolated world | Rust core + FFI + isolated world (`02` 9.2.) |
| "Screenshot + scroll" restoration | loses forms, SPA state, loaded content | disk hibernation MHTML + meta (`02` 5–6.) |
| The "a closed card occupies zero bytes" claim | inaccurate (disk, cache, GPU texture remain) | "occupies zero RAM" (`00` P10) |
| Immediate cross-platform (Windows + Linux from day one) | Wayland embedding is a separate project; it would scatter the effort | Windows in v1, Linux from v0.3, but with portable code (`ADR-006`) |
| "Automatic login everywhere" as a given | not achievable without OAuth/passkey/magic link | login runbook + vault + TOTP + popup handling (`02` 8.5., 9.5.) |

---

## 2. What needs to be fixed

### P1 — The chosen base (Electron) does not reach the goal on its own

**The plan says:** Electron + one live `WebContentsView`.
**Problem:** Electron always brings with it a Node main process **and** its own UI renderer
(Chromium), on top of which come the page renderer and the GPU, network and utility processes. An "empty" Electron
window realistically sits at ~250–350 MB of private memory, and this baseline is **there even when no page
is open at all**. In other words, the "1 GB, and meanwhile the grid view is cheap" promise cannot be kept under Electron.
**Proposal:** Your decision is correct: **native shell (Qt 6) + CEF**. The Qt shell is ~60–100 MB, the Rust core ~10–20 MB,
and there is no separate Node main process. With this, the card-grid view is ~350–470 MB with a warm engine, and **~120–180 MB
when the engine is fully stopped** (ultra-low mode); with an average page ~450–620 MB, with a heavy video
page ~800–1000 MB — the 1 GB is achievable. This **must be measured in the first week of code** (see `03-MEMORIA.md`).

### P2 — The "snapshot + scroll" restoration is an illusion (the plan's biggest professional error)

**The plan says:** on freeze, save the scroll position and a screenshot; on return
reload the page and scroll there.
**Problem:** This is exactly Chrome's "Memory Saver" behavior, which users hate. What it loses:
- text typed into a form but not yet submitted (long comment, e-mail, note);
- SPA state (YouTube playlist, Gmail unread view, Twitter scrolled feed);
- with infinite scroll, the loaded content (the `scrollTo(0, 15000)` lands on an empty page);
- video position, pause/play state;
- session tokens held in JS memory (in some SPAs this looks like "logging out").
**Proposal:** **Disk hibernation** as you decided: when leaving a card, a complete snapshot is written
to disk — MHTML (the full DOM, with embedded resources) + storage (cookie exception: those stay in the container)
+ scroll + form state + media player position. On return, an immediate, offline-readable
restoration, then a live reload on demand. Detailed specification: `02-ARCHITEKTURA.md` chapter 5
(5.1–5.4) and chapter 6.
**What, however, physically CANNOT be written to disk** (this must be stated both in the plan and in the
user promise):
- live JavaScript runtime state (React/Vue component memory, event-handler references);
- open WebSocket/WebRTC/SSE connections;
- in-progress, server-side transactions;
- running animations, `setInterval` counters.
Therefore the correct promise is: **"the visible and filled-in state survives, the running connections do not"**.

### P3 — The 24-hour *full* deletion contradicts keeping logins alive

**The plan says:** after 24 hours, full data deletion = logout.
**Problem:** this would delete exactly what you want to keep (the Google/Facebook/own-site
login), and would bring daily re-login, CAPTCHA, and occasionally account lockouts. Moreover,
the login **is not a RAM matter**: the cookie is in the engine's on-disk partition, so keeping it alive
is free in terms of memory (see `10-MUNKAMENET.md`).
**Proposal (refining the requirement):** the 24 hours should be a **three-level policy**, per card and
per container:
1. **`keep`** — deletes nothing (only the LRU cache): the login stays. *Recommended default for
   important services (Google, Facebook, own site).*
2. **`sweep`** *(container-based)* — cache, `sessionStorage`, Service Worker registrations, code cache
   are deleted; cookies and stored data remain → **stays logged in**.
3. **`wipe`** — full origin deletion = logout. Remains **as an option** (bank, health, work).
The reasoning behind the decision and the measurement (what we lose in which case) are in `10-MUNKAMENET.md`.
**Risk that must be accepted:** if someone chooses `wipe`, daily re-login +
CAPTCHA is real there; the UI signals this, it does not hide it.
Note on your "not sleeping, not disconnected" requirement: the plan fulfills this **literally** —
there is no half-dead, sleeping tab; the card **either lives or is on disk**, between the two there is only a short transition
(`02-ARCHITEKTURA.md` chapter 4).

### P4 — Login handling (rephrased: session management)

**The plan says:** password vault + TOTP + form watcher.
**Problem:** (a) "automatic login everywhere" cannot be fully implemented, and it is not even the
real need; (b) nobody measures the **cost** of kept-alive sessions; (c) the login path
is missing three important pieces:
- **OAuth/SSO popups** ("Sign in with Google") — a **real, separate window** is needed, in the same
  partition, otherwise they do not inherit the cookies;
- **passkey / WebAuthn** — detailed analysis: `12-PASSKEY.md` (one of the biggest
  open risks of the CEF-based browser);
- **magic-link login** (link sent by e-mail) — a clickable link catcher or IMAP (v2);
- **credential import**: cookie import from Chrome **does not work in 2026** (Chrome 127+ "app-bound"
  cookie encryption; decrypting the cookies requires an elevated service that verifies Chrome's signature, which
  a third party cannot legally imitate). From Firefox/text export, however, it works.
**Proposal:** The "login runbook" concept: per card/container, a
declarative sequence of steps (`url → fill field → TOTP → expected element`), which the browser executes, and which
hands over to the user on failure. This is what can be implemented from the "automatic login everywhere"
promise. Details: `02-ARCHITEKTURA.md` chapter 9.

### P5 — The multi-engine requirement was left out of the plan

The Gemini plan builds on Electron, and Electron is **only Chromium**. Your "selectable, optional
browser engines" requirement is thus not met. The plan's answer: the engine is a **separate process tree**, behind a
narrow adapter interface (7 primitives: create, navigate, snapshot, script execution in an isolated
world, scroll, image, destroy). The first adapter is CEF; the second candidate is **WebKitGTK** (on Linux it
lives on a significantly smaller footprint and can be a good "light mode"), the third is **WebView2** (Windows, shipped with the
system). Gecko cannot be embedded, Servo/Ladybird are not yet production-ready in 2026 — the plan excludes these.
Details: `02-ARCHITEKTURA.md` chapter 16 + ADR-002.

### P6 — Preview refresh contradicts the "no background engine" principle

**The plan says:** the card grid always shows the page's last state.
**Problem:** if a card is not opened for weeks, its preview goes stale — the "modern, up-to-date preview"
promise, however, requires a live load, which starts an engine. The plan does not solve this.
**Proposal:** a **preview scheduler** that reuses the same single engine, never starts a second one:
only on an idle machine, only on wall power (optional), max. 1 refresh / 7 days per card, one card
at a time, an 8-second time limit, then immediate hibernation. Even with ~500 cards this is at most a few
seconds of engine use per day. In addition, opening a card always refreshes the preview (for free).

### P7 — Screenshot-based preview is not enough for a "web page format" library

**The plan says:** compressed static preview image.
**Problem:** the image is static and unreadable; the "modern bookmark library" experience needs text, title, icon,
tags, a note, and what adds the most: **search within the content of saved pages** — this is
achievable for free with FTS5 on the text extracted from the disk-hibernated MHTML.
**Proposal:** per card: WebP preview (small, fast) + MHTML snapshot (offline reading) +
extracted text (search) + metadata. These three together give the "actually usable" library.

### P8 — A single dark-mode method is not enough (and gray mode is missing entirely)

**The plan says:** native `prefers-color-scheme` signal + style injection.
**Problem:** in reality ~40–60% of pages are not good enough in dark mode, injection breaks in many places
(shadow DOM, canvas, cross-origin stylesheets, image inversion).
**Proposal:** a **darkening-technique ladder** (not one method):
`1` native signal → `2` generated stylesheet (Dark Reader-like engine) → `3` inversion + image re-inversion →
`4` forced contrast (`forced-colors`) → `5` dimming layer (brightness) → `6` gray/colorless "reader" mode.
The ladder's operation must be **measured, not believed**: from every card's preview we compute the average brightness
(luminance), and if the page is still bright after "dark" mode, the browser steps up to the next technique.
This is self-checking, learns per page, and gives exactly what you asked for: predefined techniques, not
a compromise. **Gray mode** (one of the three base schemes) can be applied beyond the UI to the pages too
(decolorization + slight muting) — this is the most effective "tuned for reading" appearance.
Details: `02-ARCHITEKTURA.md` chapter 10.

### P9 — The vault's security is under-specified

**The plan says:** DPAPI / KDE Wallet + SQLite.
**Problem:** (a) DPAPI is tied to the **Windows user**, not to the application — another program can
decrypt it under the same user; (b) there is no master password, so the vault is not portable and there is no
protection in case the user profile is compromised; (c) there is no key rotation; (d) the
fill interface and the password's path to the page are not designed.
**Proposal:** a two-level key: **DEK** (data key, random) + **KEK** (key-encrypting key) from the system
keychain, with an **optional master password** (Argon2id, RFC 9106) — the latter makes the vault portable and
exportable. The password never enters the main world: it only appears in an isolated world, in a closed Shadow DOM
filler, and the page's JS cannot read it. Details: `02-ARCHITEKTURA.md` chapter 9.

### P10 — The claim that "a closed card occupies zero bytes" is false

The memory is indeed freed, but: the snapshot written to disk, the V8 code cache, Chromium's own
caches and part of the GPU textures remain. The correct statement: **"the card occupies zero RAM,
and ~1–10 MB on disk"**. This must be phrased this way in both the documentation and the marketing, otherwise
the user will measure, and it will not match.

### P11 — The data model's four tables are not enough

The plan's four entities (workspace, container, card, credential) do not cover real operation.
Still needed: **snapshot** (versioned, content-addressed), **page recipe** (darkening technique,
permissions, zoom, exceptions), **tag**, **login runbook**, **log/event**, **media**,
**setting**, **schema version**. Detailed DDL: `02-ARCHITEKTURA.md` chapter 7.

### P12 — There is no measurement plan and no acceptance criterion

The memory target is a metric, so it needs a measurement method (private working set, not RSS sum — the
RSS counts shared pages multiple times), an automated test bench, and a **CI gate** that fails the
build if the budget is exceeded. Without this, the target can no longer be held to account in three months. Details:
`03-MEMORIA.md`.

### P13 — Wayland embedding is not handled (Linux risk)

Embedding a native window (a foreign process's window inside our own window) is routine on Windows and X11
(`SetParent`, and `XReparentWindow`, respectively), **under Wayland, however, only with the `xdg-foreign` protocol**
does it work, and not every compositor supports it equally. Therefore the plan has **two coupling modes**:
native window embedding (fast, Windows/X11) and **OSR** (offscreen rendering, we composite the frames ourselves
— Wayland-safe, but more expensive in CPU). This is also the common path for multi-engine coupling: the engine process
is always separate, and we get the image either as a window or through a frame channel.

### P14 — Missing desktop browser basics

Without these, the browser cannot be used as a daily tool, and the plan mentions none of them: download manager and
download history; find in page (Ctrl+F); zoom; print/PDF export; PDF viewing;
picture-in-picture (PiP) video; popup and `target=_blank` handling; crash recovery; keyboard
navigation and screen-reader support; permission management (camera, microphone, notifications) per container;
proxy/VPN per container; ad and tracking blocking; automatic updates; import/export (bookmarks,
sessions, passwords); search engine choice; private card. The plan assigns these to phases in `04-UTEMTERV.md`
so that they cannot be "forgotten".

### P15 — The planned interface (card grid) itself can be a memory trap

500 cards with a 400×250 decoded preview = ~200 MB from the images alone (in the naive, non-virtualized
implementation). The plan does not take this into account.
**Proposal:** a virtualized grid (only the visible cards in the DOM), small (e.g. 260×150) WebP previews,
`loading="lazy"`, a decoded-bitmap ceiling, and the home-page renderer is **the same single renderer** as the
live page — since only one runs at a time anyway.

---

## 3. What is the biggest risk (and what to watch)

| # | Risk | Impact | Mitigation |
|---|---|---|---|
| R1 | The CEF + Qt + Rust integration takes several months until a visible result | Momentum breaks | Phase 0: a 2–4 week time-trial prototype that already measures RAM; then a decision |
| R2 | A heavy page (YouTube 1080p) is ~500 MB by itself | The 1 GB is hard to keep | Media-quality limit, budget controller, "heavy page" indicator in the UI |
| R3 | Daily (24-hour) data deletion brings CAPTCHA and re-login | Annoying experience | Per-card/per-container override + vault-driven re-login (P3) |
| R4 | Injected dark mode and autofill break per page | Continuous maintenance | Technique ladder + luminance check + regression test page set (P8) |
| R5 | Disk hibernation can be slow on large pages | Slow card switching | Asynchronous save behind an animation, incremental/accelerated save, fast restoration from preview |
| R6 | For a personal project the full vision is 2–3 years | Running dry | Phases, each phase usable on its own; "usable as one's own daily browser" already at the end of phase 2 |

---

## 4. What I do not recommend, and why

**"Option 0": do not build a browser.** Honestly: 80% of your vision can be obtained with Firefox + Multi-Account Containers
+ Auto Tab Discard + a custom home page combination, with zero development. I do not write this
to talk you out of it, but so that you know: **your own browser wins when it can do something Firefox cannot** —
namely the disk-hibernated, offline-readable card life, the built-in multi-account vault and the
editable home page in one. The plan therefore optimizes for these distinguishing capabilities, and
does not compete with Chromium's speed or web-page compatibility.

**Do not start with multi-engine support.** The adapter interface should exist from day one, but the second
engine should only be added once the first (CEF) is stable — otherwise two unstable integrations must be
maintained at once.

**No extension system in v1.** Extensions (Chrome extension API) bring memory and maintenance
costs; instead user scripts + rules per card, which can be decided later.
