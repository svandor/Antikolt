# 21 — Where the product stands (measured state)

> This page is **not a plan**: it records what works **as measured**, what is open, and what requires a human step.
> The source of the measured state is the gate: `pwsh -File scripts/verify.ps1` → **ALL SELF TESTS PASSED**, and the slices
> of the log (`16-FEJLESZTESI-NAPLO.md`), where every number is the output of an actual run.

## The gate, at a glance

```
build + selftest  ok        (CMake + MSBuild + Qt, both binaries are built)
core service      ok        (the core responds: hello, cards_list)
82 checks         ok        (79 self-tests + build + core service + check-repo; see below what each proves)
check-repo        ok        (SPDX, links, JS-parse, cargo fmt, clippy -D warnings, 93+69+12+1+15 Rust tests)
```

The portable instance: `scripts/dist.ps1 -Release` → `dist/Antikolt/` (445.6 MB), with a desktop and Start-menu
shortcut; on the installed binary `--profiletest`, `--workspacetest`, `--containertest`, `--tagtest`,
`--addcardtest` each ran separately (`exit=0`), because `dist` is the same code but not the same file.
**Exception, stated explicitly:** the CEF-GPU blocker is **resolved** — the `--disable-software-rasterizer` switch disabled
SwiftShader in the `ANTIKOLT_ENGINE_DISABLE_GPU=1` branch; fixed and measured (`--embedtest` → `exit=0` in 2.6 s). The
VECTOR hardware GPU path still dies (RTX 5080 + Intel, Chromium 158 beta), so on this machine the gate
runs with `$env:ANTIKOLT_ENGINE_DISABLE_GPU='1'`; the hardware path has been bisected (log 93): the d3d11 ANGLE backend is the faulty one on the
5080, `--use-angle=gl` works. The engine stdio bug is also resolved (`protocol.cpp`: it reads `stdin` from a private
`DuplicateHandle` copy, because Chromium closes the inherited handle), and frame writing has been made binary too
(`_setmode(_fileno(stdout), _O_BINARY)`, because text-mode `stdout` corrupted the `0x0A` byte of the frame length into a CRLF —
log 94). The `containertest` Qt6Core crash is also resolved (`main.cpp`: the `evaluate_json`/`evaluate_text` helpers put
their own `QEventLoop` into `std::make_shared` state — the root cause was given by AddressSanitizer). **The entire gate is thus
green: `ALL SELF TESTS PASSED`** (`$env:ANTIKOLT_ENGINE_DISABLE_GPU='1'`), `--containertest` → `exit=0` 12 s, all
`container_*` ok. Log slices 90–94.

## The repo is public

`https://github.com/svandor/Antikolt` — **PUBLIC**, per GitHub `licenseInfo: mpl-2.0`, the author
**Varga Sándor (`@svandor`)**, the description is bilingual. The remote tree has **149 files**, and there is no build output,
user data, or secret on it (`build/`, `dist/`, `target/`, `node_modules/`, `*.exe`, `*.dll`, `*.sqlite3` → not a single
hit). CI turned **green** after the first push (`completed success`, 2m12s) — the hosted machines run the Rust tests,
the JS-parse, the SPDX and link checks; the Qt/CEF binaries and the 79 native self-tests are **not** run (those
require Qt, CEF, and a GPU), the README states this too, and the measured result is in the log, not in a badge.

## What is measured done (and what the test proves)

| Area | Test | What it proves (measured output) |
|---|---|---|
| Grid, 500 cards | `--gridscaletest` | 503 cards, **13** rendered DOM elements, 20 ms to first render; the scroll jump is `not-measurable` (no layout height in the harness) |
| Storage (four stores + user preferences) | Rust tests, `--addcardtest` | all four stores are **SQLite rows** (`user_version=11`, WAL): cards, containers, workspaces, sessions, `user_prefs` with a single row (`CHECK (id = 1)`); the four derived `*.json` exports; `db_status=ok schema=11 … verified=yes` measures all four row by row; the pre-switch profile is migrated once per store (ADR-070, log 97) |
| User preferences | Rust tests (`sqlite_store::tests`, `service.rs`), `--prefstest` | `UserPrefs` defaults (`max_live_cards=5`, `idle_hibernate_ms=600_000`, `cold_card_ms=86_400_000`, `memory_budget_mb=512`, `resource_profile=Balanced`), validation; IPC `user_prefs`/`user_prefs_set`, `profile_export`/`profile_import`; the shell ⚙ Settings dock edits it (with a profile drop-down); the profile is ADR-020's `ProfileKind` (schema 0009–0010); the `user_prefs` reply also carries the derived `profile`, the shell caches `preview_refresh` and `engine_warm_idle_shutdown_ms` (T1/T2) |
| Search | `--searchtest`, `--tagtest` | accent-insensitive title/web address/note/tag, and the **page text** too (`search_page_text=ok`) |
| Containers | `--containertest` | colored dot, selector on the card, chip legend + filter, table in the core, `＋`/double-click/`×`, deleting a used container **by moving**, proxy/permission editor on the grid (`container_policy=ok`), partition cleanup on delete (`container_partition_cleanup=ok`) |
| Workspaces | `--workspacetest`, `--workspacedragtest` | tab row from the workspace table stored in the core, the card's `workspace` field is stored data, tab lifecycle, card selector; drag-and-drop onto the tab (`draggable` card → `drop` on the tab → `setWorkspace`, ADR-058; the `<select>` remains the keyboard path) |
| Tags | `--tagtest` | tag on the card, core normalization (trim/dedup/12×32), searchable |
| Home page | `--addcardtest` | `＋` + inline form: the card lands in the core (title/URL/workspace checked), renaming with `Enter` in the title editor (`wrote_by=gesture`), web-address rewrite (`edit_card_url=ok`), storage check (`db_status=ok schema=11 … verified=yes`), two-step delete (`delete_armed=ok delete_card=ok`, ADR-068); the test cleans up after itself |
| `<antikolt-card>` package | `--gridtest`, `--addcardtest`, `--containertest` | the component is in its own `home/antikolt-card.js` (ADR-075), the grid and a user page load the same thing; `home/user-home.example.html` is a minimal example; the container/workspace `<select>` comes from `setOptions`; every card-interaction test of the gate is green |
| Editable home page (storage+serving) | `--customhometest`, `--resethometest` | the `home.html` in the core data directory overrides the built-in grid (`ANTIKOLT_HOME` → `<coreData>/home.html` → `home/index.html`); the "Reset custom home page" button in the settings dock deletes it and reloads the grid (`reset_grid_returned=true`) |
| Profile migration | `--profiletest` | export → import into a fresh profile → read back **from file**; unknown schema rejected |
| Sessions | `--sessionpanel`, `--scrollbook`, `--wipetest`, `--cleanuptest` | shows what is alive, how much RAM/disk; `keep`/`sweep`/`wipe` execution; the "Policy" column drop-down overrides the container default **per card (at origin level)** (`session_panel=ok`) |
| Login popup, profile button, drag-and-drop | `--partitiontest`, `--profiletest`, `--workspacetest` | popup (ADR-073): gesture → a real window in the card's partition, 60 s, hibernation suspended (the gesture path is `not-measurable`); profile button: native `QFileDialog` over `profile_export`/`import` (the dialog is `not-measurable`, the path is measured with `--profiletest`); drag-and-drop: card → workspace tab next to the `<select>` (ADR-058) |
| Preview | `--preview*`, `--previewhashtest` | the background-worker rules (ADR-041) and the scheduler are measured; the preview name is the hash of the content (`previewContentName`, ADR-077 — Qt `QCryptographicHash::Sha256` in the shell), dedup follows from the naming (`preview_content_addressed=ok` / `worker_content_addressed=ok`) |
| Memory | `--metricstest`, `--memorytest`, `--prefstest`, `--memorypressuretest`, `--memorypressuree2etest`, `--memoryhibernatetest`, `--memorywarnpausetest`, `--videocaptest`, `--memoryreport` | the engine working set is measured (`engine_working_set_bytes`), the budget is the profile's `memory_budget_mb` (`budgetBytes`); the grid draws a used/budget bar and shows the pressure with `mem-over`/`mem-warn`/`mem-hard` signaling (the bridge passes `overBudget`/`pressure`, slice 126); the card's `memory_bytes` is the working set at freeze time (schema 0008); the `system.memory` `overBudget` signal is the basis of the media-quality limit, and from the profile's `warn`/`hard` bands a `pressure` (normal/warn/hard) is also computed (ADR-020, slice 124), and reaching the `hard` band hibernates the live card while the `warn` band pauses the preview worker (ADR-020, slices 127–128); the profile's `video_max_height` is applied **as a display cap** to the top-level `video` (`set_video_cap`, ADR-076, `--videocaptest`: Fast 2160px → Saver 720px → Manual removed), the decode resolution is `not-measurable`; `--memoryreport` writes the full controller state (profile + budget + working set + per-card memory + pressure) at once |
| Tray, log, theme, language | `--traytest`, `--logtest`, `--themetest`, `--languagetest`, `--gridlangtest` | the log does not write needlessly, the themes give the correct color; the language is switchable at runtime (ADR-078/079): the shell chrome (`ui/language`, `setLanguage`), the grid (`data-language`, `pushLanguageToGrid`, `home/app.js` `t()`), the card's inline labels + the static labels (`antikolt-card.js` `t()`, `applyStaticLabels`) AND the shell status messages (`setStatus` → `t()`) re-label in HU/EN — the HU/EN UI is done |
| Printing (save to PDF) | `--printtest` | the engine writes the PDF with its own compositor (`PrintToPDF`); the shell's `🖨️ PDF` button opens a native file dialog (the dialog is `not-measurable`, the path is measured by `--printtest` — F7 is the opener) |
| Find on page (Ctrl+F) | `--findtest` | the engine searches the text via `Find`/`CefFindHandler` and reports the match count with a `find_result` event; the shell shows a Ctrl+F search bar (Enter = next, Shift+Enter = previous, Esc = close) |
| Download | `--downloadtest` | the engine downloads the file via `StartDownload`/`CefDownloadHandler` and reports the finished state (path + success) with a `download_finished` event |
| Post-crash recovery | `--crashtest` | the engine reports the renderer exiting with a `renderer_terminated` event; the shell immediately reloads the crashed surface (the grid returns to the home page, the card to its own URL) |
| PDF viewing | `--pdftest` | the PDF opens in CEF's built-in PDFium viewer (loading, not downloading) |
| Accessibility | `--accessibilitytest` | every interactive control can be announced (text or accessible name), the Ctrl+F shortcut is registered |
| PiP (Document-PiP) de-risking | `--pipchecktest`, `--viewstest` | the API is **present** on the secure `antikolt://` scheme (`pip_secure=1 pip_api=present`), `requestWindow()` **opens the window** (`pip_window=ok`), but the windowed shutdown **dies** (ADR-096); the views framework **loads a page** (`views_load=ok`), but the CHROME style **rejects the custom cache path** (`views_partition_context=rejected`) — the views switch conflicts with partition isolation (ADR-097, **a decision is needed**) |

## What is open (deliberately, not forgotten)

1. **Encryption of the profile bundle** — the bundle also contains session rows; this is a real decision, not a matter of
   convenience.
2. **The login popup gesture path** — the engine is wired up (ADR-073: gesture → a real window in the card's
   partition, 60 s, hibernation suspended), but the gesture path is not measurable in the harness (the synthetic
   `window.open` is not a gesture): the `popup_requested`/`popup_closed` path is `not-measurable`.
3. **The full editable home page** — the `<antikolt-card>` package (ADR-075), the storage+serving of the user HTML,
   the reset, the **starting template + editing**, and the **sandboxing** are done (slices 106, 111,
   117, 118); the user HTML reaches the card operations through the bridge (deliberately), direct
   file reading (`fetch`/`XHR`) is proven closed.
4. **The rest of F6** — the budget, the settings UI, the per-card memory breakdown, the profile preset, the
   media-quality limit **signal**, the profile **preview refresh**, the **tuning report**, the **engine lifetime
   (T1/T2) hookup**, the **engine stop/restart cycle**, the **grid rasterization**, the **raster home-page
   mode**, its **automatic activation**, the **click mapping**, the **raster display + click hook**
   and the **aspect-ratio-preserving fit** are done (ADR-074/020, slices 102–120), and a `pressure` signal is already computed from the `warn`/`hard` bands (ADR-020, slice 124), and the `hard` band hibernates the live card while the `warn` band pauses the preview worker (ADR-020, slices 127–128); the profile's **other dimension's**
   actual controller action, the `video_max_height`, is **done as a display cap** (ADR-076, `--videocaptest`:
   the profile value reaches the top-level `video` as a `video{max-height:Npx}` rule, the `manual` profile removes it);
   the **decode** resolution reduction is `not-measurable` — CEF does not provide a per-page video decode limit;
   the **automatic profile switching** is done (ADR-080, `--autoprofiltest`: the `Auto` mode, based on the power source + the measured
   pressure, resolves through the core's `resolve_profile` to `Balanced`/`Saver`/`Ultra`, the shell re-queries after every sample).

## What needs a human step (not code)

- **Signing:** SignPath Foundation (open-source code signing) and/or Store — the key is not on our machine
  (`20-ALASAS.md`).
- **Passkey/vault:** `cargo vendor` without network does not work in this environment; the CDP passkey experiment
  result is in `12-PASSKEY.md`.
- **Release workflow:** the GitHub Actions release has never run yet (there is no signed artifact).

## How to verify all this in five minutes

```powershell
pwsh -File scripts/verify.ps1                 # 79 self-tests + check-repo
pwsh -File scripts/build.ps1                  # just the two binaries
pwsh -File scripts/dist.ps1 -Release          # portable instance + shortcut
build/apps/desktop-shell/antikolt-shell.exe --profiletest build/apps/engine-host/antikolt-engine-host.exe
```

If any step does not say what this page claims, **this page is wrong** — the log and the gate are the reality.

