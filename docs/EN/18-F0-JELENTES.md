# 18 — F0 report: the first real memory measurement

> This document is the material for the F0 decision gate (`04-UTEMTERV.md`): **for the first time we have a number, not an estimate.**
> Measurement: 2026-11-16, VECTOR, CEF 158 (Chromium 158), Qt 6.8.3, MSVC 19.44.

## 1. What we measured

The **private memory of the engine's process tree** (browser + renderer + GPU/network/utility processes),
15 seconds after a page load, the median of 5 one-second samples. The process tree is all
`antikolt-engine-host.exe` instances (CEF launches its helper processes with its own executable).

| Page | Median | Peak | Processes |
|---|---|---|---|
| `about:blank` | **119,0 MB** | 119,0 MB | 7 |
| Wikipedia article (`en.wikipedia.org/wiki/Web_browser`) | **185,3 MB** (hidden) / **176,6 MB** (visible) | 185,4 MB | 7 |
| Google Maps | **142,9 MB** | 142,9 MB | 7 |
| YouTube homepage | **390,1 MB** (hidden) / **386,3 MB** (visible) | 393,4 MB | 10 |

The invisible- and visible-window measurements are practically identical (±5%), so the hidden-window measurement
**does not distort** — this is important, because it means CI does not need to flash a window.

## 2. What this means for the budget

The complete browser = engine + Qt shell (~60–100 MB) + Rust core (~20 MB):

| State | Engine (measured) | + shell/core (estimate) | Total | Target (`03-MEMORIA.md`) |
|---|---|---|---|---|
| Card grid, engine running, empty page | 119 MB | ~90–120 MB | **~210–240 MB** | ≤ 470 MB ✅ |
| An average article | 177–185 MB | ~90–120 MB | **~270–305 MB** | ≤ 620 MB ✅ |
| A map-like SPA | 143 MB | ~90–120 MB | **~235–265 MB** | ≤ 620 MB ✅ |
| YouTube (heavy media page) | 386–390 MB | ~90–120 MB | **~480–510 MB** | ≤ 1000 MB ✅ |

**Verdict: the 1 GB promise is comfortably achievable on this machine, and even the heaviest measured case is at half of it.**
This is better than the plan's estimate (which expected 450–620 MB for an average page) — the estimate was cautious,
which is the right attitude, but now we know the real number.

An important nuance: the measurement applies to **a single page**. The project's promise is that there is *never* more than one
live page — which is exactly why the numbers above give the upper bound of daily use, not the lower one.

## 3. Caveats (so no one misquotes them)

1. **The measured value is private commit** (`Process.PrivateMemorySize64`), because the `Working Set - Private`
   performance counter was not available in this environment. Commit is usually **≥** the
   working set, so the numbers skew upward rather than downward.
2. **The GPU process stops in this environment** (`exit_code=-2147483645` = `0x80000003`, CHECK error),
   and Chromium falls back to software compositing. The 7 processes are therefore a tree without a GPU process; on a
   working GPU **+50–150 MB** is expected. This must be examined separately in the next measurement.
3. **The Qt shell is not in the tree** — the "+ shell/core" column above is an estimate, not a measurement.
4. **Not included**: snapshot capture, preview processing, the vault, and the CEF sandbox.
5. A single machine, a single network; the protection (EDR) and the browser cache can increase the numbers.

## 4. What we found (and fixed) during the measurement

These are real engineering results, not incidental ones:

1. **Our own argument parser killed CEF's child processes.** The `--type=gpu-process` /
   `--type=utility` switches were treated by the parser as unknown arguments, the helper processes exited, and the
   GPU stopped with the "isn't usable" error. Fix: we catch the `--type=` prefix **before the parser**, and
   go straight to `CefExecuteProcess`.
2. **A race condition between `CefRunMessageLoop` and fast loading.** The `data:` URL load finished *before the loop
   started*, so the quit signal was lost → the process blocked forever. Fix:
   the measurement mode uses its own deterministic pump loop (`CefDoMessageLoopWork` + deadline), not a
   blocking loop. This is also the right pattern for the Qt integration.
3. **`CefShutdown` called with a live browser = an immediate access violation** (`0xC0000005`, a read of address
   `0x40` — the project owner also saw this error dialog). Fix: before shutdown, call
   `CloseBrowser(true)`, then pump until the browser disappears.
4. **A `WS_POPUP`, never-shown window** causes a GPU compositing error. Fix: a normal
   window type, we just do not call `ShowWindow`.
5. **In CEF 158 there is no `settings.user_data_path`** (`cache_path` / `root_cache_path` are used instead).
6. **We disabled the error dialog** (`SetErrorMode`), so that a faulty run does not block on a
   modal window, but instead signals with an exit code — CI-friendly.

## 5. Conclusion and the next step

- **The greatest risk ("does not fit into 1 GB") is gone.** The engine with a heavy page is ~390 MB,
  the complete browser is expected to be ~500 MB — half of the target.
- Further work will be determined **not by memory**, but by **features**: hibernation and
  restore, the start page, containers, the vault.
- Next slice:
  1. investigating and eliminating the GPU process shutdown (a measurement with a working GPU);
  2. connecting the **shell ↔ engine host** with the `crates/antikolt-ipc` protocol — **done** (chapter 5);
  3. the **freezing** prototype — **done** (chapter 5): a DOM snapshot, then measuring the memory
     drop; full MHTML + scrolling + storage are in the next slice.

## 5. Freezing measurement: how much RAM does saving a card to disk give back?

This is a measurement of the product's most important mechanism (`perf/measure-freeze.ps1`): the engine loads a page,
the `snapshot` command writes the DOM to a file, then the surface disappears — and meanwhile we continuously measure the
process tree's memory, per phase.

| Page | Loaded | After snapshot | **On disk (frozen)** | **RAM given back** | Snapshot size |
|---|---|---|---|---|---|
| `example.com` | 127,3 MB | 127,5 MB | 107,7 MB | **19,6 MB** | 2,4 KB (DOM) |
| Wikipedia article | 182,6 MB | 183,7 MB | 115,9 MB | **66,7 MB** | 345 KB (DOM) |
| Wikipedia article (MHTML) | 179,0 MB | 184,6 MB | 117,8 MB | **61,2 MB** | **1,44 MB (MHTML)** |

Alongside the snapshot, a **state file** is also created (`surface-N-M.state.json`, ~4,4 KB): scroll, anchor,
18 form fields, `sessionStorage`, media position, viewport/DPR — without password fields.

### Restore (offline, with the same engine)

The saved MHTML is loaded at a `file://` address, then the state is written back — **without a network**:

```
mentés:  scrollY=600, 18 űrlapmező
visszaállítás: navigate=file:///…/surface-1-0.mhtml → status=200
               restore applied: forms=18 scrollY=600
               verified: title="Webböngésző – Wikipédia" scrollY=600 fields=18 text="Ugrás a tartalomhoz…"
```

This closes the loop: **live page → disk → RAM given back → offline restore**.

The MHTML variant saves the whole page (DOM **and** embedded images/CSS), so the file is larger and it raises the peak by ~5,6 MB
at the moment of saving (the MHTML is assembled in memory) — in return, the card **can also be displayed
offline**.

**What this proves:**

1. The **DOM snapshot is produced** (345 KB of real HTML, `<title>Webböngésző – Wikipédia</title>` — the measurement
   checked the file's contents), and the cost of `snapshot` is negligible (+1,1 MB, the renderer is still alive).
2. The **engine gives memory back**: in the Wikipedia article's case **66,7 MB** is freed by the
   surface disappearing.
3. The remaining ~116 MB is the **engine baseline** (browser, network, and utility processes) — this is the part
   that *T2/ultra-low* mode gives back by stopping the engine host. The two levels therefore behave exactly as
   described in `02-ARCHITEKTURA.md` section 3.4: **T1 = fast, but +100–120 MB baseline; T2 = ~120 MB total**.

**Caveat (honestly):** for now the snapshot is **only the DOM** (CEF's `GetSource`). For full
disk hibernation we still need MHTML (embedded resources), the scroll position and anchor, the form
and `sessionStorage` state, and the media player position. The measurement proves that **the
mechanism and the memory give-back are real**, completeness is in the next slice.

## 6. Re-running

```powershell
# one page
& perf/measure-engine.ps1 -Url https://en.wikipedia.org/wiki/Web_browser

# multiple pages, with a visible window
& perf/measure-engine.ps1 -Visible -Url @('https://en.wikipedia.org/wiki/Web_browser','https://www.youtube.com/')

# freezing: how much RAM saving a card to disk gives back
& perf/measure-freeze.ps1 -Url https://hu.wikipedia.org/wiki/B%C3%B6ng%C3%A9sz%C5%91

# where the raw reports live (gitignored)
perf/reports/
```

The measurement assumes the state after `scripts/build.ps1` (the output contains the
`antikolt-engine-host.exe` and, next to it, the complete CEF runtime).
