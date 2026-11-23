# Antikolt — English summary

**Antikolt** is a card-based, container-based, low-RAM desktop browser for Windows 11 (first) and
Fedora/Nobara + KDE Wayland (later). Open source, **MPL-2.0** ([`LICENSE`](../../LICENSE)).
Status: **v0.8 — runnable, measured**. The repo is not a design document any more: it builds two native
binaries (a Qt 6 shell and a CEF engine host) and a Rust core, and a gate proves the behaviour.

The authoritative plan, the decision log (ADR-001…ADR-081) and the development log are Hungarian under
[`docs/HU/`](../HU/); this page is the condensed English version, and
[`docs/HU/21-ALLAPOT.md`](../HU/21-ALLAPOT.md) is the measured "where the product stands" page.

## What is proven today

| Area | How it is measured |
|---|---|
| Grid and hibernation | `--gridscaletest`: 503 cards, **13** rendered DOM elements, 20 ms to first paint; leaving a card returns **61 MB** of RAM and restores it offline (`scrollY=600`, `fields=18`) |
| Search | `--searchtest`, `--tagtest`: accent-insensitive across title, URL, note, tag and captured page text |
| Containers | `--containertest`: colour dot, per-card selector, legend + filter, table in the core, create/rename/delete, delete-with-move; each container carries its own proxy and permission policy, and every card gets its own Chromium profile (`--partitiontest`, ADR-071) |
| Workspaces | `--workspacetest`: the tab row is the core's table, a card's workspace is a stored field, tab lifecycle (`＋`, double click, `×`) |
| Cards | `--addcardtest`: add a card with the `＋` form, rename the title and the URL in place, delete — each read back from the core |
| Portable profile | `--profiletest`: export the profile to one versioned JSON bundle and import it into a fresh profile, from the page itself |
| Storage | Rust tests + `db_status`: all four stores are SQLite rows (`user_version = 11`, WAL) — cards, containers, workspaces, sessions, plus a single-row `user_prefs` table for user preferences — and the four `*.json` documents are derived exports; the report proves every export agrees with the rows, and a pre-switch profile is adopted once per store (ADR-069/070/072) |
| Gate | `scripts/verify.ps1`: **68 self tests** + `check-repo` (SPDX, links, JS parse, release files, `cargo fmt`, `clippy -D warnings`, 93 + 67 + 11 + 1 + 15 Rust tests) |

## The idea

No tab strip, no bookmark toolbar. Saved pages live as **cards** on a **home page that is itself a
real, freely editable web page** (cards are inserted as `<antikolt-card>` elements). It replaces "keep 12
tabs open" with "glance at a live-looking dashboard".

Three pillars:

1. **Session management.** Per-identity **containers** (isolated cookie/storage partitions), and the
   important logins (Google, Facebook, your own site) are **kept alive at ~0 RAM**: cookies live on disk,
   not in memory. A measurable **session ledger** shows what is alive, how much disk it costs, how much
   RAM it costs (≈0 while dormant) and what you would lose by wiping it. Built-in vault + TOTP + autofill.
2. **Pluggable engines.** The engine is a separate process tree behind a 7-primitive adapter contract.
   Engine #1 is Chromium (CEF); candidates later: WebKitGTK (Linux, small footprint), WebView2 (Windows).
3. **Disk hibernation.** When you leave a card, its **entire state is written to disk** (MHTML DOM
   snapshot + scroll/anchor + form state + sessionStorage + media position + preview + extracted text),
   and the engine view is destroyed. Returning is instant (preview → offline snapshot → live reload).
   Disk may use tens of GB; RAM may not.

## Key numbers (engineering estimates, to be validated in Phase 0)

| State | Target |
|---|---|
| Card grid, engine warm (T1) | 350–470 MB |
| Card grid, engine killed (ultra-low, T2) | 120–180 MB |
| One average page live | 450–620 MB |
| One heavy page (video/SPA) | 800–1000 MB — memory governor intervenes |
| Hard invariant | ≤ 1 live web renderer at any time |

## Architecture in one paragraph

`antikolt-shell` (C++20 + Qt 6/QML) renders the chrome and the card grid; `antikolt-core` (Rust) owns the
SQLite database, the vault, the card lifecycle state machine, the content-addressed snapshot store and
search; `antikolt-engine-host` (CEF, separate process) owns the page. They talk over a versioned
MessagePack IPC on a local socket. On Windows the engine surface is embedded as a native child window;
on Wayland it is composited from offscreen frames (OSR). Snapshots use CDP `Page.captureSnapshot`
(MHTML), storage clearing uses `Storage.clearDataForOrigin`, and theme emulation uses
`Emulation.setEmulatedMedia`.

## Card lifecycle

`saved → warming → live → freezing → saved`; after **10 minutes** of inactivity (input-reset, suspended
while audible/pinned/downloading) the engine is destroyed and the card returns to the grid. After
**24 hours** the session policy applies inside the container — `keep` (nothing), `sweep` (cache,
sessionStorage, service workers; **you stay logged in**) or `wipe` (full origin wipe = logout, for banks).
The snapshot, notes and search index always survive. The measured cost of each choice is shown in the
session ledger (see `HU/10-MUNKAMENET.md`).

## What is *not* preserved (stated honestly)

Live JS runtime state, open WebSockets/WebRTC, running timers/animations, in-progress server transactions.
Everything *visible and filled in* is preserved.

## Themes

Light / **grey** / dark / AMOLED for the app UI. For pages, a six-step **darkening ladder**
(native `prefers-color-scheme` → Chromium auto-dark → generated stylesheet → inversion → forced colors →
dim overlay → reader mode) with **verification**: after each step a screenshot's average luminance is
measured; if the page is still bright, the next technique is applied. The winning technique is stored per
site, so it improves over time.

## Documents

| File | Content |
|---|---|
| [`HU/00-JAVASLATOK.md`](../HU/00-JAVASLATOK.md) | Review of the original (Gemini) plan: 18 findings + risks |
| [`HU/01-KERDESEK.md`](../HU/01-KERDESEK.md) | Open questions with recommended answers |
| [`HU/02-ARCHITEKTURA.md`](../HU/02-ARCHITEKTURA.md) | The plan: processes, lifecycle, snapshots, SQL schema, vault, themes |
| [`HU/03-MEMORIA.md`](../HU/03-MEMORIA.md) | Memory budget, measurement method, CI gates |
| [`HU/04-UTEMTERV.md`](../HU/04-UTEMTERV.md) | Phases, milestones, stopping criteria |
| [`HU/05-DONTESEK.md`](../HU/05-DONTESEK.md) | Architecture decision records (18 ADRs) |
| [`HU/06-OTLETEK.md`](../HU/06-OTLETEK.md) | Idea list beyond the requirements (time-travel through snapshots, offline reading, memory transparency, …) |
| [`HU/07-FOGALMAK.md`](../HU/07-FOGALMAK.md) | Hungarian–English glossary of the terminology used across the docs |
| [`HU/08-DIZAJN.md`](../HU/08-DIZAJN.md) | Design tokens, four themes, card anatomy |
| [`HU/09-CELHARDVER.md`](../HU/09-CELHARDVER.md) | Target hardware ("VECTOR": 24 threads, 32 GB, RTX 5080 16 GB + iGPU, dual display) and resource profiles |
| [`HU/10-MUNKAMENET.md`](../HU/10-MUNKAMENET.md) | Keeping logins alive at ~0 RAM, the measurable session ledger, and the explicit "what do we lose" table |
| [`HU/11-OSR.md`](../HU/11-OSR.md) | Native window embedding vs offscreen rendering, and why Wayland forces OSR |
| [`HU/12-PASSKEY.md`](../HU/12-PASSKEY.md) | Passkeys: CEF has no WebAuthn support — options, costs, and the software-authenticator plan |
| [`HU/13-LICENC.md`](../HU/13-LICENC.md) | MPL-2.0, Qt LGPL-3 obligations, CEF BSD, DCO, AI-authorship disclosure |
| [`HU/14-KODALAS.md`](../HU/14-KODALAS.md) | Code signing: SmartScreen reality, free OSS options, Ed25519 update signing |
| [`HU/15-HOZZAJARULAS.md`](../HU/15-HOZZAJARULAS.md) | Contribution model (DCO, no CLA) and security governance (protected areas, fuzzing, disclosure) |
| [`HU/16-FEJLESZTESI-NAPLO.md`](../HU/16-FEJLESZTESI-NAPLO.md) | Development log: what is built, how it was verified, what blocks |
| [`HU/17-ESZKOZLANC.md`](../HU/17-ESZKOZLANC.md) | Toolchain: Qt 6, CEF, MSVC, and the four traps that cost the most time |
| [`HU/18-F0-JELENTES.md`](../HU/18-F0-JELENTES.md) | The F0 measurement: what is measured, how much, what it means, with reservations |
| [`HU/19-FOLYTATAS.md`](../HU/19-FOLYTATAS.md) | Continuation plan: what is done, what comes next |
| [`HU/20-ALASAS.md`](../HU/20-ALASAS.md) | Code signing: Store and SignPath, costs, and the exact steps in the repo |
| [`HU/21-ALLAPOT.md`](../HU/21-ALLAPOT.md) | **Measured status**: what works and with which evidence, what is open, what needs a human |
| [`../proto/README.md`](../../proto/README.md) | Wire protocol v1 between the core and the engine host |
| [`forras/`](../forras/) | The original plan this work started from (ODT + text) and the raw research notes |

## Licensing note

MPL-2.0 ([`LICENSE`](../../LICENSE)). Qt is LGPL-3, so it must be **dynamically linked** and replaceable by the user;
CEF is BSD. Third-party components and the AI-assisted development disclosure are listed in
[`NOTICE`](../../NOTICE); the maintainer is Varga Sándor (`@svandor`), see [`AUTHORS`](../../AUTHORS).

