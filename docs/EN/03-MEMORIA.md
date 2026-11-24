# 03 — Memory budget and measurement

> The project's reason for existing stands or falls on memory. This document states **how much** fits,
> **how** we measure it, and **what** we do if we exceed it.

> The target machine's (VECTOR) data and the device profiles: `09-CELHARDVER.md`. The memory cost of logins:
> `10-MUNKAMENET.md` (in short: a kept-alive login = 0 RAM while the card is hibernated).
>
> **MEASURED RESULT (F0, 2026-11-16):** the engine's process tree is **177–185 MB** on an average article, **386–390 MB**
> on a heavy media page (YouTube), and **119 MB** on an empty page. The full browser together with the Qt shell
> is expected to be **270–510 MB** — the budget below is sustainable, and in fact stays well under it.
> Detailed measurement, method and reservations: [`18-F0-JELENTES.md`](18-F0-JELENTES.md).

## 1. Basic truths (what many people get wrong)

1. **RSS is not what you should add up.** RSS counts the shared memory pages (shared libraries, Chromium code)
   in every process, so the "RSS sum" overestimates by 2–3×. The correct metric is the
   **private working set** (private working set / private bytes), or on Linux the **PSS**
   (`/proc/<pid>/smaps_rollup`), which distributes the shared pages proportionally.
2. **Most of the memory is in the renderer, not in the browser.** A static article is 80–150 MB, a
   video SPA (YouTube 1080p) is 400–550 MB, and a very heavy admin interface can even be above 600 MB. That is why the
   "one live view" rule is the most important memory decision.
3. **The Chromium base cannot be made to disappear.** The engine consists of several processes: browser/main process, GPU,
   network service, storage/utility, renderer, possibly audio. Together these take ~180–280 MB,
   even when not a single page is loaded. That is why a full engine shutdown (T2 mode) is **required** for the
   truly low idle value.
4. **Our own UI also consumes.** The home page is a web renderer, so the card grid view is also
   ~60–120 MB extra. That is why the grid is virtualized, the previews are small, and the home page's complexity has
   an upper limit (see chapter 6).
5. **Disk is free, RAM is not.** By your decision we save everything to disk (even tens of GB), so
   the RAM budget only applies to *live* things.

## 2. Where memory goes — structural model

| Component | Idle | With one page | Note |
|---|---|---|---|
| Qt shell (QML, Qt libraries) | 60–100 MB | 60–100 MB | our own UI; does not grow per page |
| Rust core (DB, vault, index) | 10–20 MB | 15–30 MB | page cache grows with SQLite |
| Engine base (browser+GPU+network+utility) | 180–280 MB | 180–280 MB | fixed cost |
| Renderer — home page | 60–120 MB | — | simple local page |
| Renderer — average page | — | 80–200 MB | article, blog, documentation |
| Renderer — heavy page | — | 400–600 MB | video, large SPA, WebGL |
| Preview decoding (peak) | 5–10 MB | 5–10 MB | limited by a ceiling |

From these the **modes** are derived (the modes' values come from the sum of the component ranges, rounded):

| Mode | Description | Estimated private memory |
|---|---|---|
| **M0 — card grid, engine running (T1)** | default daily state | **350–470 MB** |
| **M0-light** | card grid, engine stopped, home page as a raster image (T2) | **120–180 MB** |
| **M1 — one average page** | reading, searching | **450–620 MB** |
| **M2 — one heavy page** | video, large SPA | **800–1000 MB** (this is where the budget governor kicks in) |
| **M3 — two pages (forbidden)** | only a brief transition during switching | 1000–1400 MB |

> The table is an **engineering estimate**, validated by the Phase 0 measurement bench (see chapter 7). The numbers are
> needed for decisions, not promises.

**Conclusion:** "staying under 1 GB" **is sustainable in daily use** (M0, M1); on heavy pages
(M2) the budget governor must intervene. The "150 MB idle" is only achievable by stopping the engine (M0-light)
— the UI must communicate this honestly ("Ultra-low mode: the card grid becomes live on the first
click").

## 3. Ultra-low mode (T2) and the raster home page

By stopping the engine, memory drops from 350–470 MB to 120–180 MB, but something still has to show the home page.
Solution: **raster home page**:

1. An image (possibly 2× resolution) is made of the home page's last state.
2. The engine stops, the shell displays this image, with an **interaction map** (card rectangles, buttons)
   on top of it.
3. On the first real interaction (click, scroll, typing, hover after 300 ms) the engine starts
   (~400–800 ms), and the home page becomes live — the image is still visible until then, so there is no blank screen.
4. Scrolling on the raster image: the image can be panned and the cards can be "popped out" as images — this is the idle
   browsing experience, with zero engine.

This is the only mode that delivers the "modern, 100 MB browser" promise. Risk: the switch between the raster image and the
live home page may be noticeable; therefore it only engages when the user requests it ("Ultra-low")
or after 2 minutes of idleness, and switching back is instant.

## 4. Budget governor (memory governor)

The core continuously (every 2 s) samples the process tree and acts according to bands. The governor works
**together with the profiles** (`09-CELHARDVER.md` chapter 3): the bands are interpreted relative to the
current profile.

| Band | Threshold (private) | Action |
|---|---|---|
| green | < 700 MB | no action |
| yellow | 700–850 MB | preview refresh pauses; cache cleanup; a note in the log |
| orange | 850–1000 MB | freeing the oldest background resources; enabling the media quality limit (e.g. 720p); suggestion to the UI: "heavy page" |
| red | > 1000 MB | deep clean: flushing the engine's cache, discarding the `jscode` cache, as a last resort stopping the home page renderer (raster mode) |

The governor **never interrupts the user's work** (it does not close the active page); it only frees
non-visible resources, or signals.

**Media quality limit:** on video pages, reducing the resolution (720p) is the biggest single-step gain
(M2 → ~600–700 MB). The UI shows this with a small indicator ("memory-saving video"), and it can be overridden with one
click.

## 5. Measurement method

### 5.1. What we measure

- **Windows:** the whole process tree's **private working set** (`Private Working Set`), per process and
  in total. Tool: `Get-Counter '\Process(*)\Working Set - Private'` filtered to the `antikolt*` processes,
  plus the core's own meter (`GetProcessMemoryInfo` / `K32GetProcessMemoryInfo`).
- **Linux:** `PSS` from `/proc/<pid>/smaps_rollup`, summed over the process tree.
- **Supplementary:** peak value, renderer process count (I1 invariant), engine house state (T1/T2),
  size of blobs on disk.

### 5.2. Measurement rules

- **5 measurements, median** (memory is noisy; the average is misleading).
- **Warm start** (the second start), because the first run has no cache.
- **Fixed scenario**, with the same pages, with the same viewport (1440×900, DPR 1.25).
- **Nothing else runs** on the machine during the measurement (the measurement bench warns).
- **Baseline committed**: the result of every measurement is `perf/baseline/<date>.json` + a markdown report.

### 5.3. Scenarios

| # | Scenario | What it proves |
|---|---|---|
| S0 | empty browser, card grid, 0 cards | base cost |
| S1 | card grid, 50 cards, 50 previews | I3, virtualization |
| S2 | card grid, 500 cards | large library |
| S3 | one static article opened | M1 |
| S4 | YouTube 1080p playback | M2, budget governor |
| S5 | Gmail-like SPA | heavy SPA |
| S6 | opening 10 cards one after another (switching) | leak check |
| S7 | 1 hour of use simulated (200 switches) | long-term stability |
| S8 | ultra-low mode (T2) idle | M0-light |
| S9 | 25 kept-alive logins, all dormant, cold engine | login RAM cost ≈ 0 |
| S10 | the same scenario with Fast / Balanced / Saver / Ultra profile | the real difference between profiles |

### 5.4. Acceptance criteria (the basis of the CI gate)

| Metric | Target | Hard maximum |
|---|---|---|
| S0 private memory | ≤ 380 MB | 450 MB |
| S1 private memory | ≤ 450 MB | 550 MB |
| S2 private memory | ≤ 500 MB | 600 MB |
| S3 private memory | ≤ 620 MB | 700 MB |
| S4 private memory | ≤ 1000 MB | 1100 MB |
| S8 (ultra-low) | ≤ 180 MB | 250 MB |
| S9 (25 dormant logins, cold engine) | ≤ 180 MB | 250 MB |
| S10 (profiles: Fast ↔ Ultra difference) | documented, measured | — |
| Renderer process count (I1) | 1 | 1 |
| At the end of S7, memory growth compared to S1 | ≤ 10% | 20% |
| Perceived card opening time (S3) | ≤ 600 ms | 900 ms |
| — of which the first frame (R0) appears | ≤ 50 ms | 100 ms |
| — with T2 (cold engine) the live content | ≤ 1200 ms | 1500 ms |
| Freezing (S3) | ≤ 800 ms | 1500 ms |
| Search on 10 000 cards | ≤ 50 ms | 120 ms |

## 6. The home page's memory discipline (mandatory rules)

1. **Virtualized grid:** max ~60 card-DOM at once.
2. **Small previews:** 260×150 WebP; decoded bitmap ceiling 24 images (after that `img` reload).
3. **No iframe, no live preview** on the home page (an iframe is a second renderer = violating I1).
4. **No large JS framework** on the home page; the built-in template stays under ~50 KB JS.
5. **The user's own home page** gets a "weight" indicator (how many MB of memory the renderer uses), and
   if it exceeds the limit, the browser warns (but does not forbid).
6. **Images and fonts** on the home page come from a local file (no network).

## 7. Phase 0 — the measurement week (the plan's first work item)

Before anything else is built, **in one week** a timing prototype must come together that:

1. is a Qt 6 window + CEF with one page, one partition;
2. measures and logs the process tree's private memory;
3. can stop the engine house (T2) and restart it;
4. provides the real numbers on the three key pages (static article, YouTube, Gmail);
5. compares them with the estimates above and **writes a report** (`perf/phase0-report.md`).

**Decision gate:** if the real M0 > 600 MB or M2 > 1.3 GB, then the plan must be revised (e.g. switching the home page
to native QML, or a more aggressive engine house shutdown). This decision cannot be dodged
with guessing — that is why the first week is this.

## 8. What we deliberately do NOT do for memory

| Idea | Why not |
|---|---|
| Disabling the sandbox | security suicide, and it does not gain significant memory anyway |
| Single-process engine (`--single-process`) | unstable, crashes, not production-ready |
| Software rendering by default | CPU explosion, video breaks (only for debugging) |
| Killing the GPU process | most pages get slower, not worth it |
| Aggressively limiting JS memory | pages crash; only in extreme cases (budget governor red band) |
| Supporting extensions | every extension is its own process/memory |

## 9. What is worth trying from engine tuning (measured in Phase 0)

These are Chromium/CEF switches, and they **must be measured**, because their effect is version-dependent:

- per-renderer heap ceiling (V8 `--max-old-space-size`) — to curb runaway pages;
- disabling the "spare renderer" (pre-warming of a spare renderer) — one fewer process;
- background throttling for non-visible views;
- disk cache size limit (less memory-mapped cache);
- disabling `BackForwardCache` (the back-forward cache keeps entire pages alive).

The result of every tried switch goes into the `perf/` report, and only stays in if it **measurably**
helps and does not harm compatibility.

## 10. Honest summary

- **Achievable:** daily use with 1 live page 450–620 MB, card grid 350–470 MB, ultra-low mode 120–180 MB,
  heavy video page ~800–1000 MB (handled by the budget governor).
- **Not achievable:** 4-5 open, heavy pages under 1 GB — that is why the "one live view" rule exists.
- **Not achievable:** idle memory under 100 MB with a *live* home page (because of the Chromium base and the renderer);
  only with a raster home page, without an engine.
- **What this means:** instead of the current 3-4 browser windows / 4-5 GB, **one browser, around 0.5 GB** —
  this is the real, measurable gain that the project promises.
