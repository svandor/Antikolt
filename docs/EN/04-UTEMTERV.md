# 04 — Roadmap, milestones, acceptance

> **Development model:** the code is written by the **assistant (AI agent)**, while the project owner decides, tests and
> dogfoods. This cycle accelerates writing, but **not** validation: at the end of every phase, measurement, testing and
> the owner's approval decide. Time estimates are therefore expressed in cycles, with ±40% uncertainty.


> **Checkbox status (2026-11-19):** the checkmarks track code and tests, not intent. An item is
> checked when there is **runnable evidence** for it (the named self-test), and for partial items the open part
> is there in line. The complete, always-current picture is in `19-FOLYTATAS.md` §12; this file is the milestone plan.

## Phases

| Phase | Content | Cycle | Output |
|---|---|---|---|
| **F0** | **De-risking week(s)**: 4 spikes (see below) + test bench | 2–3 weeks | `perf/phase0-report.md`, decision gate |
| **F1** | Skeleton: Qt shell + CEF engine housing + 1 card + disk hibernation + restore | 6–8 weeks | card: opens → freezes → restores, measured |
| **F2** | Home canvas + card grid + preview + search + workspaces | 6–8 weeks | daily browsing with cards |
| **F3** | Containers + popup/OAuth + permissions + proxy + **session ledger and panel** | 4–6 weeks | **M3: my own daily browser** |
| **F4** | Vault + autofill + TOTP + login scenario | 6–8 weeks | "no need to type passwords" |
| **F4b** | *(optional, in v1.1)* software passkey in the vault | 4–6 weeks | passkey-only accounts also work |
| **F5** | Theme engine (light/gray/dark/AMOLED + technique ladder) | 3–4 weeks | no blinding page |
| **F6** | Resource profiles + raster home + budget controller + tuning | 3–4 weeks | 120–180 MB idle state |
| **F7** | Desktop browser basics (download, Ctrl+F, zoom, PDF, PiP, printing, crash recovery, accessibility, HU/EN) | 4–6 weeks | **M7: public beta** |
| **F8** | Linux/Nobara + Wayland (OSR) + packaging + updater + signing | 4–6 weeks | **M8: 1.0** |
| **F9** | Second engine (WebKitGTK) — optional | 6–8 weeks | multi-engine promise |

**Total (without F4b and F9):** ~37–52 weeks. Therefore the phases yield **independently usable** states:
at the end of F3 you already have your own daily browser.

## F0 — the four spikes (this is the most important week)

The goal is not beauty, but that the **four biggest risks** surface before anything else is built.

| Spike | Question it answers | Done when |
|---|---|---|
| **S-A. Qt + CEF + measurement** | how much real memory with one page? (M0/M1/M2) | the test bench runs, the numbers for the three reference pages are in |
| **S-B. Disk hibernation** | does MHTML creation **and loading** work, how fast, how big? | a page's freeze→restore cycle < 2.5 s, content is readable |
| **S-C. OSR** | how much CPU cost and latency does OSR have on one page? | 30 fps scrolling, click works, measured |
| **S-D. Passkey (CDP)** | does the virtual authenticator work in a **production** environment, can the key be exported? | a `create`+`get` round runs on a test RP, the key can be exported |

If S-D does not work, passkey drops out of v1.1 (documented limitation), and the other phases do not stop.
If S-C is unacceptably expensive, Linux slips, Windows does not.
If S-B does not work, the foundation of the whole plan (disk hibernation) changes — **which is why it is first**.

**Decision gate:** if the real M0 > 600 MB or the heavy page > 1.3 GB, the plan must be revised
(native QML grid, more aggressive engine shutdown, or redefining the goal).

## Milestones

### M0 — "The proof works" (end of F0)
- [ ] Four spikes ran, report in `perf/`.
- [ ] Real memory numbers on the three reference pages (static article, YouTube 1080p, Gmail-like SPA).
- [ ] Decision: continue / adjust.

### M1 — "The card lives" (end of F1)
- [x] Card open, scrolling, freezing, restoring (R0→R1→R2). — `--opentest`, `--scrollzoomtest`, `--lifecycletest` (2026-11-19)
- [x] MHTML + meta + preview atomically on disk. — `--lifecycletest`, `--capturetest`, `--previewtest`
- [x] 10-minute rule (with sound/video/pin exceptions). — `--lifecycletest`; the media exception also in cleanup: `--mediacleantest` (ADR-048)
- [ ] Test bench: S3 and S6 green.

### M2 — "I use it" (end of F2)
- [x] Virtualized grid with 500 cards. — `--gridscaletest`: 503 cards, **13** rendered DOM elements, 20 ms to first render (2026-11-19)
- [x] Editable home: adding a card (`＋` + inline form) and deleting it. — `--addcardtest` (ADR-061); open: rewriting the title/web address in the UI, and delete confirmation
- [ ] `<antikolt-card>` as a separate package (today's card is the page's own component).
- [x] Search (title, URL, note, tag, page text; Hungarian diacritics). — `--searchtest` + `--tagtest`: diacritic-independent title/web address/note/tag, and page text is also searchable (`search_page_text=ok`, ADR-049/055)
- [x] Workspaces, tags, note, pinning; preview scheduler. — `--workspacetest` + `--tagtest` + `--notetest` + `--pintest`: the tab row comes from the core workspace table (today SQLite rows, ADR-070), the card's `workspace` field is stored data (instead of the old `index > 12` guess, ADR-056), the tab row has a lifecycle (`＋`/double click/`×`, ADR-057), and the card's workspace can be switched with its own selector (ADR-058), and it can also be dropped onto the tab with drag-and-drop (ADR-058 confirmed, `--workspacedragtest`)

### M3 — "All my accounts at once" (end of F3) → **the real product**
- [x] Containers with color coding, card-container binding, container switching. — `--containertest` (ADR-050…054): colored dot, selector on the card, chip legend + filter, table in the core, `＋`/double click/`×`, and deleting a used container with reassignment (all measured); open: per-container permission/proxy
- [ ] OAuth popup in the correct partition (max. 60 s, documented I1 exception).
- [x] **Sessions panel + ledger**: shows what is alive, how much RAM/disk, what we would lose. — `--sessionpanel`, `--scrollbook`
- [x] `keep`/`sweep`/`wipe` policy + origin-level enforcement. — `--wipetest`, `--cleanuptest`, `--mediacleantest`
- [ ] Permission handling and proxy per container.
- [x] Profile migration: one file, versioned bundle. — `--profiletest` (ADR-059): `profile_export`/`profile_import`; open: UI button and bundle encryption

### M4 — "I don't have to type passwords" (end of F4)
- [ ] Vault (DEK/KEK, optional master password), auto-lock, KDBX export. — the **encryption + storage** is done (AES-256-GCM + Argon2id + DPAPI, ADR-082/083, `secrets.rs` + `credentials.rs`, `vault_*` IPC); KDBX export and filling are open
- [ ] Filling in an isolated world, with origin binding, with a native setter.
- [ ] TOTP + clipboard cleanup. — the **TOTP code** is done (RFC 6238, ADR-084, `totp.rs` + `vault_totp`); clipboard cleanup is open
- [ ] Login scenario + manual handoff in the case of CAPTCHA.

### M4b — passkey (F4b, v1.1)
- [ ] Software authenticator in the vault, `create`/`get` round, with a test RP.
- [ ] Honest disclosure: no hardware protection; vault locking is the presence check.

### M5 — "Doesn't blind me at night" (end of F5)
- [x] Four UI themes (light/gray/dark/AMOLED), contrast test in CI. — `--themetest`: pixel measurement + WCAG AA for every theme (the lowest 5.6:1)
- [ ] Technique ladder + luminance check + per-page learning.
- [ ] Gray mode and reader mode.

### M6 — "Fits in 150 MB" (end of F6)
- [x] Five resource profiles, with automatic switching. — the profiles are done (`--prefstest`/`--memoryreport`), and the **automatic switching** too: `Auto` mode (`ProfileKind::Auto` + `resolve_profile` IPC, ADR-080, `--autoprofiltest`)
- [x] Budget controller with bars, with a media-quality limit. — the budget is in the profile (`memory_budget_mb`, ADR-074, `--prefstest`/`--memoryreport`), the media-quality limit `video_max_height` is a **display cap** (`set_video_cap`, ADR-076, `--videocaptest`); the decode resolution reduction is `not-measurable` (CEF provides no per-page limit)
- [ ] Raster home (engine stopped).
- [ ] Tuning report (which switches helped, measured).

### M7 — "Beta" (end of F7) — **closed (slice 165, v0.9)**: vault + passkey done, PiP postponed (ADR-095/096/097)
- [x] Download manager, Ctrl+F, PDF, PiP, printing. — **zoom** is done (`--scrollzoomtest`), the **printing** "save as PDF" path is done (`--printtest`, slice 129), **Ctrl+F** is done (`--findtest`, slice 130), **download** is done (`--downloadtest`, slice 131), **PDF** is done (`--pdftest`, slice 134), **PiP** is **postponed** (decision 2026-11-24: stays on the windowed path with the documented crash, ADR-095/096/097)
- [x] Recovery after crash. — done: the shell automatically reloads the crashed UI (`--crashtest`, slices 132–133)
- [ ] Keyboard navigation, screen-reader support. — the **named controls + Ctrl+F** are done (`--accessibilitytest`, slice 135), the full screen-reader integration is open
- [x] HU/EN UI; password and bookmark import. — the HU/EN UI is **done** (`ui/language` + `data-language` + `setStatus`→`t()`, ADR-078/079); **bookmark import** is done (Netscape-HTML → cards, ADR-081, `--bookmarkimporttest`); **password import** is done (CSV, ADR-085, `vault_import_csv`)

### M8 — "1.0" (end of F8)
- [ ] Linux (Nobara/KDE/Wayland) with OSR, RPM package.
- [ ] SignPath signing + Ed25519 update signing, installer + updater.
- [ ] License/NOTICE/AUTHORS/THIRD-PARTY files, HU/EN documentation, CONTRIBUTING (DCO). — the **files** are done (LICENSE/NOTICE/AUTHORS/CONTRIBUTING/THIRD-PARTY-NOTICES, the check-repo "Release files" guard keeps them); the complete HU→EN translation is open

## Risks and "stop" criteria

| Risk | If it occurs | Action |
|---|---|---|
| The CEF + Qt integration does not work within the planned time | F1 slips | Simplification: native window + minimal QML, the home later |
| The memory does not work out | the project loses its point | decision at the F0 gate: raster home, engineless mode, or redefining the goal |
| MHTML reload does not work | the foundation of hibernation is damaged | resource map + own request interceptor (see `02` 5.2. fallback) |
| OSR is expensive or fragile | Linux slips | Windows-first release, Linux later |
| Passkey cannot be solved with CEF | passkey-only accounts are excluded | documented limitation + researching the hardware path |
| Dark mode/autofill keeps breaking | maintenance burden | technique ladder + a 20-page regression set, automated |
| One person + one agent is not enough | bleeding out | stopping at M3: use as "own browser", the other phases optional |

## What I pin down up front (so it does not slip later)

- **No extension system in v1.**
- **One window, one card renderer** (I1) — the login popup is the documented exception.
- **No cloud, no telemetry.**
- **The home page gets no native access** — only the bridge API.
- **The vault format is versioned and exportable**, so it can still be opened 5 years from now.
- **Every claim is measurable**: at the end of every phase the `perf/` report and the `03-MEMORIA.md` budget are updated.
