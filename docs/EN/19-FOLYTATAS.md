# 19 — Continuation work schedule (after slice 144)

> This document is **not a plan**, but a work schedule: it starts from the current state, and for each item
> it states in which file what to do and what proves it. The plan is `00`–`15`, the decisions are `05`, the
> chronology is `16`; this page is the first reading of the next work session.

## HANDOVER — 2026-11-23 (after slice 144, for a new work session)

> This chapter is the **first reading** of the new work session. We are at commit `d131c4f`; everything starts from here.

### State (measured, not intent)

```
git rev-parse --short HEAD  →  d131c4f  (docs(slice-144): release files (THIRD-PARTY-NOTICES) + gate check (M8))
scripts/verify.ps1          →  ALL CHECKS PASSED — 80 checks all "ok"
cargo test                  →  190 green: 93 + 69 + 12 + 1 + 15
git status                  →  clean working tree, everything pushed (origin/main)
```

### Completed in this work session (slices 136–144, all green + pushed)

- **F6 video cap:** `video_max_height` as a display cap (`set_video_cap`, ADR-076, `--videocaptest`).
- **F2 preview:** the preview name is the sha256 of the content (`previewContentName`, ADR-077, `--previewhashtest`).
- **Complete HU/EN interface:** chrome + grid + card + static labels + status messages (ADR-078/079,
  `--languagetest` + `--gridlangtest`, slices 138–141).
- **M6 automatic profile switching:** `Auto` mode (ADR-080, `--autoprofiltest`, slice 142).
- **M7 bookmark import:** Netscape-HTML → cards (ADR-081, `--bookmarkimporttest`, slice 143).
- **M8 release files:** `THIRD-PARTY-NOTICES.txt` + the gate's "Release files" check (slice 144).

### Order and agreement with the maintainer (this blocks the code)

The well-scoped code work is **done**. The remainder needs a **decision first** for one large item:

1. **Password vault + password import (M7)** — **done** (ADR-083/093 + the key export of slice 163): AES-256-GCM +
   Argon2id + DPAPI, TOTP, CSV import, autofill, origin lookup, passkey storage + export; the full
   `navigator.credentials.create/get` on the real surface is `not-measurable` (focus gesture).
2. **PiP (M7)** — **deferred (decision 2026-11-24):** the maintainer chose option 2 — PiP stays on the
   windowed path with the documented crash (ADR-095/096/097, measured); the views switch (Chrome profiles)
   does not continue now.
3. **Version/milestone** — **done (slice 165):** `v0.8` → `v0.9` (badge) + the Cargo/CMake `0.1.0` → `0.9.0`; **M7 closed**.

The **UI HU→EN translation is done** (slice 164). After the version bump (165), in order: full HU→EN
documentation translation (M8) → Linux package (M8) → release (human: SignPath/Store, NVDA).

### What is open (concrete next step)

| Item | Where | What to do | What proves it |
|---|---|---|---|
| F6 `video_max_height` | **done** (ADR-076, slice 136) | the profile's `video_max_height` applied as a **display cap** (`set_video_cap` op, named style element); the decode-resolution reduction is `not-measurable` (CEF gives no per-page limit) | `--videocaptest`: Fast 2160px → Saver 720px (rendered height 720) → Manual removed |
| Automatic profile switching (M6) | **done** (ADR-080, slice 142) | `ProfileKind::Auto` + `resolve_profile` IPC (`{on_ac_power, pressure}` → `{kind, params}`); the shell re-asks every cycle based on `onAcPower()` + the memory pattern | `--autoprofiltest`: normal/ac=balanced · normal/bat=saver · hard=ultra |
| Bookmark import (M7) | **done** (ADR-081, slice 143) | Netscape-HTML → cards (`bookmarks.h` regex parser + `newCardId`), re-import does not duplicate; `importBookmarksFromPath` in the "💾 Profile" menu | `--bookmarkimporttest`: parse + id shape + upsert |
| Password vault (M7) | `crates/antikolt-core-service/src/credentials.rs` (ADR-083/084/085/086/087/088/089/090/091/092/093) | the **encryption + the storage + the TOTP + the CSV import + the passkey spike + the shell wiring + the isolated-world spike + the autofill spike + the origin-based lookup + the `fill_credential` command + the metadata-only vault panel + the `webauthn_setup` command + the passkey storage** are done (AES-256-GCM + Argon2id + DPAPI, `credential` table SQLite-only, `vault_*`/`vault_totp`/`vault_import_csv`/`vault_get_for_origin` IPC, the CDP virtual authenticator **works and can be commanded**, the `CoreClient` + `--vaulttest` controls the vault from the shell, `fill_credential` fills the form in the isolated world, the `VaultPanel` shows a metadata-only list, `--passkeystoretest` measures passkey storage); the full `navigator.credentials.create/get` on the real surface is **`not-measurable`** (focus gesture, the custom scheme is `invalid domain`), the key export from the virtual authenticator is **done** (slice 163, `--passkeyexporttest`), PiP is **deferred** | `cargo test` 190 green (93+69+12+1+15) + `--webauthntest`/`--vaulttest`/`--isolatedworldtest`/`--autofilltest`/`--fillcredentialtest`/`--vaultpaneltest`/`--passkeyflowtest`/`--passkeystoretest`/`--passkeyexporttest` in the gate |
| OAuth wiring | `main_window.cpp` (popup, 02-ARCH §8.5) | the mechanism is done, the login gesture path cannot be clicked in the harness | `not-measurable` gesture; the finished mechanism is already measured |
| `sha256` blob store | **done** (ADR-077, slice 137) | the preview name is the hash of the content (`previewContentName`, Qt `QCryptographicHash::Sha256` in the shell — the `sha2` crate dependency is gone); dedup from the naming | `--previewhashtest` (public vectors), `--previewgentest`/`--previewworkertest` (`preview_content_addressed=ok`) |
| PiP | engine (CEF 158) | **deferred (decision 2026-11-24):** de-risked (ADR-095/096/097) — the API is present on the secure `antikolt://` scheme (`pip_secure=1 pip_api=present`), `requestWindow()` opens the window (`pip_window=ok`), but the windowed shutdown crashes (`0xC0000005`), and the views switch loads (`views_load=ok`) but rejects the custom cache path (`views_partition_context=rejected`) — the maintainer chose **option 2**: PiP stays on the windowed path with the documented crash | `--pipchecktest`, `--viewstest` (gate); `--pipwindowtest` (diagnostic) |
| HU/EN interface | **done** (ADR-078/079, slices 138–141 + **slice 164 — full translation**) | `ui/language` + `i18n.h` `uiText` + `setLanguage` (chrome); `data-language` + `pushLanguageToGrid` + `home/app.js` `t()` (grid); `antikolt-card.js` `t()` (card); `setStatus` → `t()` (status messages); slice 164 wired every hard-coded chrome/dialog/tooltip/tray/status + grid/card text (`~60` new i18n entries, `~80` wrappings) | `--languagetest` + `--gridlangtest`: the chrome and the grid switch HU/EN |
| Full screen reader integration | external | manual with a screen reader (NVDA) | human step |
| Release | SignPath, Store | human step (Partner Center, certificate) | — |

### Mandatory work schedule (the slices depend on this)

1. **One slice = one measurable feature + self-test.** Every feature must have its own `--…test` (or clean function)
   that runs in the gate.
2. **The gate is the evidence:** `scripts/verify.ps1` → `ALL CHECKS PASSED` (now 80 checks). Never green by
   "lying": write the numbers into the docs from the gate output, not from memory.
3. **At the end of every slice:** build + gate green → entry in `16-FEJLESZTESI-NAPLO.md` with the **measured** output →
   commit (`feat/test(slice-NNN): …`) → `git push`. The doc language is **Hungarian**.
4. **Documents:** `16` (chronology), `21-ALLAPOT` (table), `04-UTEMTERV` (checkmark), `19` (this page).

### Sandbox (the established pattern)

- The `write`/`edit` file editing does **not** ask for escalation.
- `build.ps1`, `verify.ps1` and `git push` **ask** for `danger-full-access` (because of the pipe-capture `EPERM` and the
  ssh `CreateFileMapping`) + a one-sentence justification. After a denial, retry **once** the same command with the
  narrower permission — that is the path to approval.

### CEF 158 traps (learned in this session, do not run into them again)

- `CefBrowserHost::Find` expects **4 parameters** (no `identifier`); the first `OnFindResult` gives the match count
  with `active=0` (the selection comes in the next notification).
- `CefSettings` does **not** know `download_default_directory`; the download location is learned from `GetFullPath()`.
- `CefPdfPrintSettings`'s field is `display_header_footer` (not `header_footer_enabled`), off by default.
- `chrome://crash` is the deterministic renderer crash (`STATUS_ACCESS_VIOLATION`) — `--crashtest` uses it.

### Delegation

The work that writes files and runs the gate should stay on the **main thread** (paid/pro route) for quality; the
`subagent` (free chain) is for independent research/review.

## 0. Starting state (measured)

```
pwsh -File scripts/verify.ps1 -SkipBuild
gridtest ok · lifecycletest ok · opentest ok · embedtest ok · capturetest ok · coretest ok · check-repo ok
ALL SELF TESTS PASSED
```

The product loop that works today: start → core card list → grid with the real, rendered cards →
click (pinned or grid card) → bridge call → snapshot + surface swap → live card →
**after 10 minutes of inactivity it goes to disk by itself, and the grid comes back**.

What is **mandatory** to respect (the slices depend on this):
- I1: one live surface at a time; the grid may only be requested again after the `destroy_surface` ack
  (otherwise `engine_busy`).
- I2: snapshot first, surface destruction after.
- The engine's `stderr` is visible through the shell (`[shell] info: [engine] …`) — this is the first diagnostic.
- At the end of every slice: `pwsh -File scripts/verify.ps1` green, and `16-FEJLESZTESI-NAPLO.md` gets an
  entry with the actual output (not intent, measurement).

## 1. Excluding a pinned card from hibernation

> **Status: done and measured** (`50ded02` + slice 21) — the `pinned` flag travels across the bridge
> (`app.js` → `open_card` args → `pending_pinned_` → `live_card_pinned_` when the surface attaches),
> `onIdleTimeout` skips hibernation, `surfaceDetached` clears it. The `--lifecycletest` measures **both
> branches**: `hibernated=ok` (not pinned) and `pinned_honoured=ok` (pinned, alive even after 4 clock ticks).
> The bug found along the way (the grid's `detach` erased the flag) and the pinned status of cards seeded by
> the core are in the slice 21 entry.

**Why:** the documented opt-out (`02-ARCHITEKTURA.md` §4.2) — the user may ask that a card stay alive.
Hibernation did not ask this before this slice.

**How (the suggested, smallest path):** the `pinned` flag should travel **in the open request**, because the page
already knows its card — so no new API is needed in the core:
1. `home/app.js`, in `withNativeOpen` → `native.openCard({...})`: `pinned: card.pinned,` next to
   `title`.
2. `apps/desktop-shell/src/main_window.h`: `bool live_card_pinned_ = false;`.
3. `main_window.cpp` `handleBridgeCall` (`open_card` branch): `live_card_pinned_ = args.value("pinned").toBool();`.
4. `main_window.cpp` `onIdleTimeout`, **before** the media request:
   `if (live_card_pinned_) { setStatus("kitűzött kártya — a hibernálás kimarad"); armIdleTimer(); return; }`.
5. `main_window.cpp` `surfaceDetached`: `live_card_pinned_ = false;`.

**Evidence (still missing):** the second phase of `--lifecycletest`: `eval` on the grid
`window.antikoltNative.openCard({id:'probe', url:'data:text/html,<title>p</title>p', title:'p',`
`pinned:true})` → the card opens, but after the shortened clock expires it does **not** close; the test
writes `pinned_honoured=ok`, and the two phases together exit 0. This must be written in the next step:
`runLifecycleCheck` in `apps/desktop-shell/src/main.cpp`, after the existing first phase.

## 2. 24-hour session policy + ledger

> **Status: done and measured** (slices 22–25, ADR-033) — the decision in the core (`plan_cleanup`, `session_due`),
> the ledger in `sessions.json`, the panel shows it, the **"Clean up now" button actually performs it** (immediately on
> the open origin, otherwise on the card's next open), the engine deletes with `Storage.clearDataForOrigin`.
> Self-tests: `--coretest` `session_ledger=ok`, `--sessionpanel` `session_panel=ok`, `--wipetest`
> `wipe=ok`, `--cleanuptest` `cleanup=ok` (`page_wrote=ok`, `storage_after=|`, `origin_rows=0`).
> The **quantity** side is done: `SessionSize` + `loses_login()` during the ledger (slice 41, 78 core tests).\n> The panel **shows** it too: `7 cookies · 5.0 kB` (+ `· login` on a logged-in row), slice 42.\n> **What remains:** the **media exemption** of the cleanup (a playing card
> should delay the deletion), and per-card (origin-level) override — the rule editing is **done** (2b).

**Why:** M3 ("own daily browser") main item: `keep`/`sweep`/`wipe`, with a visible session panel.

**How (what remains):** to execute `wipe`, the engine's `clear_origin_data` op is needed (today
`not_supported`); the panel is built on the `session_list`/`session_due` response, together with the `LossEstimate`
(`10-MUNKAMENET.md` §5).

**Evidence (done):** `--coretest` `session_ledger=ok`, `cargo test -p antikolt-core-service` 9 green.

## 3. Request direction from the page (protocol v2)

> **Status: the channel is done and measured** (slice 26, ADR-034) — the page's `window.antikoltNative.request(...)`
> call is a persistent `cefQuery`; the engine holds it in `pending_bridge`, forwards it with `bridge_call` + a request
> number, the shell answers with `bridge_reply`. `PROTOCOL_VERSION` = 2, both sides check it.
> `--bridgetest`: `asked=ok`, `search_cards=1`, `search_first_title=Wikipedia — kezdőlap`,
> `unknown_method=rejected`, `bridge_request=ok`.
> **The focus return is measured and works (slice 62):** after saving, the card regains focus, and the
> `--notetest` already *asserts* it: `note_focus=01SEED…001:card`, and the chain is
> `{"hasFocus":true,"activeTag":"ANTIKOLT-CARD","activeIsHost":true,"shadowActive":"card"}`. There were two bugs in
> the earlier measurement: the probe asked the **wrong element** (the first card, not the focusing one), and the focus
> return relied on a 2-second timer that could expire before the core's re-render —
> in that case the focus fell on `body` (`note_focus=not-host:BODY`). The fix: the identifier stays until the next
> user event (not until a timer), and the grid returns the focus **synchronously after** the render, because a
> background window does not necessarily get an animation frame.
> **The rendered note measurement is also done:** the probe reads the
> `.card-notes` text from the card's shadow DOM by polling, after the state response (`note_rendered=probe note`).
> **What remains in this chapter:** the mention of the `test.hang` hook in the user docs (it already appears in
> `07-FOGALMAK.md`), and that the grid's own search intentionally stays local (the page receives the full
> list).

## 2b. Setting session rules (done, ADR-039)

> `session_rules` op + panel editor (hour, delete at app close, Save). `--rulestest`: `rules_ttl_ms=7200000`,
> `rules_lock_wipe=true`, `rules_refused=refused`, `rules_reread_ttl_ms=7200000`, `session_rules=ok`.
> The editor fields show the core's state (`panel_ttl_hours=2`, `panel_lock_wipe=true`), the container default goes back unchanged.
> **Per-card (origin-level) override: done and measured** (slice 121) — the panel's dropdown, with `session_upsert`,
> writes the row's `policy`, the core `SessionRules::resolve`-s it; the `a_rows_own_policy_overrides_the_container_default`
> test proves both directions (wipe overrides sweep, keep overrides wipe).

## 4. Previews

> **Status: both serving and generation are done, measured** (slices 30–31, ADR-035). The engine registers the `antikolt`
> scheme and answers the `blob/<name>` request from the `<user data>/previews/<name>` file (the name
> resolved within the directory); the shell takes a picture with `capture` when the card opens, moves it into the profile,
> and writes its name into the `preview_ref` field. `--previewtest`: `preview_load=loaded:4x3`,
> `preview_traversal=refused`. `--previewgentest`: `preview_ref=preview-probe.png`,
> `preview_page=loaded:1560x857`, `preview_generation=ok`.
> The daily ≤100 limit is **persistent** (`previews/budget.json`, slice 38, `--previewbudget`), the 7-day refresh
> is done (`--previewrefresh`), the orphan-file cleanup is done (`--previewclean`), and the **background**-made
> preview worker is also done (ADR-041/ADR-042, `--previewworkertest`, see §9). The grid **also
> displays** the image: `home/app.js` makes an `<img src="antikolt://blob/…">` from the record's `preview_ref` (the
> `--gridtest` measures it with `naturalWidth > 0`), so the loop is closed: worker → `preview_ref` → tile.
> **What remains:** the `sha256`-based blob store is **done** (ADR-077, slice 137) — the name is the hash of the content,
> with Qt `QCryptographicHash::Sha256` in the shell (the `sha2` crate dependency is gone); what is still open is
> that the queue should also replace the *old* images (`nextPreviewCard` already lists the stale one, this
> must be measured in real use).

**Why:** F2 (grid preview) — the `capture` primitive is done, the processor is not.

**How:** `antikolt://blob/<name>` scheme handler in the engine
(`CefRegisterSchemeHandlerFactory`, from the profile's `previews/` folder, with name sanitization), then a
scheduler: one card at a time, 8 s limit, 100 refreshes per day (`02-ARCHITEKTURA.md` §5). The
content-addressed blob store (`sha256`) is **done** (ADR-077): `previewContentName` in the shell, with Qt
`QCryptographicHash::Sha256` — the `sha2` crate dependency is gone, the filename is now the content hash.

**Status:** the scheduler is **done** (slices 52–53): `nextPreviewCard` decides, the worker surface photographs
(ADR-041, ADR-042), the cap and the frame are in the `previews.h` guards and the engine house's two-surface limit.

**Evidence:** the second phase of `--gridtest`: the card's `previewRef` loads, the image is not empty
(`naturalWidth > 0`), and the `capture` file is in the profile.

## 4b. Memory measurement (done, ADR-036)

> The engine's `metrics` op measures the page's DOM nodes, its JS heap and its own working set; the shell answers
> the page's `system.memory` request with it. `--metricstest`: `dom_nodes=57`, `engine_working_set_bytes=…`,
> `page_used_bytes=…`, `metrics=ok`.
> **What remains:** the memory **budget** (F6) and the per-card breakdown; the JS heap is quantized because of Chromium,
> so it is an indicator.

## 4c. Page theme (done, ADR-037)

> The engine's `set_media` op sets `prefers-color-scheme` (`auto`/`dark`/`light`), the shell's toolbar button
> cycles it and remembers it (re-applies it on every load). `--mediatest`: `media_dark=dark`,
> `media_light=light`, `site_theme=ok`.
> **What remains:** per-page exception; forced darkening is intentionally absent (`scroll`/`set_zoom` done, 4d).

## 4d. Scrolling and zooming (done, ADR-038)

> The engine's `scroll` (`window.scrollBy`) and `set_zoom` (`Emulation.setDeviceMetricsOverride`, multiplied by
> the DPR) ops are done. `--scrollzoomtest`: `scroll_y=600`, `dpr_at_150=1.875`, `dpr_at_100=1.25`,
> `scroll_zoom=ok`.
> The shell side is also done: toolbar `−` / `🔍 100%` / `+`, remembered and re-applied zoom on every load
> (`dpr_at_125=1.5625`).
> **The scroll position per card is done** (slice 39, `scroll_book.h` + `scroll.json`): `--scrollbook`
> proves the book (`scroll_a=640`, `scroll_b_after_top=120`, `scroll_reloaded=120`), and the wiring is covered by
> `--opentest`/`--scrollzoomtest`.
> **The end-to-end check is done** (slice 40, `--scrollrestore`): `first_scroll_y=700`, `restored_scroll_y=700`. Three real bugs were fixed along the way: the returning grid's bridge, the save/navigation race, and the grid's zero overwrite; the restore is absolute and delayed.`r`n> (b) gesture-based scrolling (inner container); (c) the 7-day threshold and the daily 100 frame cannot be tuned from the UI.

## 5. Vault and passkey
**Why:** F4/F4b; the seam is done (`crates/antikolt-core/src/vault.rs`, 10 tests, ADR-031).

**How:** `cargo vendor` on a **networked** machine (`aes-gcm`, `argon2`, `zeroize`, + the
`rand_core`/`getrandom` chain), then `AesGcmSecrets: SecretBox` with the record AAD (origin + id, ADR-010)
and the DEK wrapping; then the `credential` table (the schema in `0001_init.sql`), then the passkey
(`WebAuthn.enable` / `addVirtualAuthenticator` / `getCredentials` / `setCredential`) on the finished
`DevToolsCall` (ADR-029).

**Evidence:** the seam's tests green + a `create`/`get` round on a test RP (F0 S-D spike).

## 5b. Fixed: the `--capturetest` crash on exit

One out of four runs returned with `0xC0000005`, **after it had printed every result** — so the shutdown
was at fault. Cause: the `destroy_surface` request was sent in a **fire-and-forget** manner, then the channel
was closed immediately (`client.shutdown()`), so the reply arrived at an already-dying client.

Fix: the test waits for the `destroy_surface` reply (max. 2 s), and only then closes the channel.
**Measurement: 8/8 runs green** (previously 1 crash out of 4).


## 6. Known traps (learned the expensive way)

| Trap | What it causes | Where it is recorded |
|---|---|---|
| `file://` + `type="module"` | the script does not run, the grid is empty | `16`, slice 16 |
| `shadowRoot.dataset` | every card render throws, empty elements | `16`, slice 16 |
| start-time bridge decision | the sample `open` runs instead of the real one | `16`, slice 17 |
| `create_surface` before destroy | `engine_busy`, the grid does not come back | `16`, slice 19 |
| page without `console` | the error is invisible | `16`, slice 18 |
| `DevToolsReply` document lifetime | empty dict, empty `state.json` | ADR-029 |

## 7. Tools

- `scripts/verify.ps1` — one command, every self-test + `check-repo` (the session's measuring tool).
- `scripts/build.ps1` — toolchain discovery + build + two `--selftest`.
- `scripts/check-repo.ps1` — the CI gate (SPDX, links, hygiene, `node --check`, `cargo fmt/clippy/test`).
- The engine's log on the shell's `stderr`: `[shell] info: [engine] [engine-host] …` — every
  protocol op, DevTools call and page `console` message shows here.

## 8. Release and signing (ADR-040)

| Item | Status |
|---|---|
| Portable build + desktop/Start menu shortcut | done (`scripts/dist.ps1`, `-Release`) |
| Own icon | done (`assets/antikolt.icon`, `--selftest` → `icon=ok`) |
| **Tray icon + single-instance launch** | done (ADR-043, `--traytest`; the `dist\Antikolt` updated) |
| Signing tool | done (`scripts/sign.ps1`: PFX / thumbprint / self-signed / `-VerifyOnly`) |
| Release workflow | **scaffold** (`.github/workflows/release.yml`, never ran) |
| **MSIX package** | done (`scripts/msix.ps1`, `payload=ok`, 201.5 MB — reloaded from the fresh payload on 2026-11-18) |
| Store Identity + local install | remaining (Partner Center; certificate obligation) |
| SignPath application | human step (public repo + review) |

## 9. Done: the background preview worker (`--previewworkertest`)

ADR-041 is done, the worker lives in `MainWindow`, and the engine house learned what the second surface
requires (ADR-042). What was **extra work** beyond the recipe, and what the next slice will not have to re-derive:

1. **The engine house used to hold a single surface.** The `surface` field was only checked by the wire: every
   command ran against the global `HostState`, and a command addressed to a non-existent surface silently served
   the grid. Now there is per-surface state (the `create_surface` id is the key), `bridge_reply` looks for
   the request's owner, an unknown surface gets `surface_not_found`, and the engine rejects **more than
   two** surfaces with `engine_busy`. The `hidden` flag can also be given per surface.
2. **The shell side** was built according to the recipe: `PreviewWorker` state machine with the `previews.h` guards, its own
   partition (`persist:preview`), hidden surface, 8 s cap (`ANTIKOLT_PREVIEW_DEADLINE_MS` for the test,
   like `ANTIKOLT_HIBERNATE_MS`), abort on card open and at the cap, moving the image under
   `previews/`, `previewConsumeBudget`, `preview_ref` in the core's record, and the grid queue starting the next
   card 1.2 s after the worker.
3. **The stricter engine exposed the bugs in our tests.** Nine self-tests failed *immediately*: `eval` addressed to a stale
   surface id (after the hibernated grid), and `id` parameters copied into deferred lambdas that pointed to
   garbage after the creating closure died. Each was a real bug that the "serves-everything" engine had hidden;
   after the fix the whole gate is green.
4. **The measurement to know instead of the garbage:** the hidden surface's first `Page.captureScreenshot` on this
   machine (no GPU, software compositing) sometimes waits **more than 20 s** for a frame. This is not a bug in
   the product: the 8 s cap closes the worker, the card is skipped, and the queue retries in the next round.
   The test therefore does the same (at most three rounds), and prints how many were needed
   (`worker_attempts`); the contract is "eventually loads", not "succeeds the first time".

**Evidence:** `previewworkertest ok`, with `deadline_fired=ok`, `worker_attempts=1`, `worker_pixels=1579x942`
`colours=13`, `worker_ref=preview-worker-probe.png`, `worker_budget=1`, `abort_state=idle`,
`abort_surface_gone=no live surface`, `user_card_loaded=ok` — the full gate (`scripts/verify.ps1`) green.

**Next slice candidates** (from the open items of `04-UTEMTERV`): the memory budget (F6) and the
per-card breakdown; the `vault` (F4) with `cargo vendor` on a networked machine (ADR-031's seam is done); the
containers and the per-card session override; the media exemption from the cleanup; the virtualized,
500-card grid. On the release side there is a single **human step**: the SignPath application.

## 10. Done: the tray icon that doubles as the run indicator (`--traytest`, ADR-043)

The user request: "the launcher icon to the tray, which is also the running icon". Two pieces were needed, and
both are measured:

1. **Single-instance guard** (`instance_guard.h/.cpp`): before starting, the shell acquires a named local
   socket (`antikolt-shell-<user>`). The second launch **connects first**, and if the running instance answers,
   it asks it for the window, then exits with 0, without window or engine. The order matters: on Windows two
   processes can both create a pipe instance for the same name, so "listen succeeded" alone is not
   proof (we measured: this way the second also became primary). Without the guard (no local socket) the program
   starts the old way, only noting it in the log.
2. **Tray icon** in `MainWindow`: the program icon (the same as the shortcut's), "Antikolt — running" tip,
   click toggles show/hide, menu: "Open" / "Quit". The **close button sends it to the tray**, does not quit,
   and explains this once; quitting is the menu's "Quit" item. Without a tray (or with `ANTIKOLT_NO_TRAY`)
   everything is as before — the other self-tests run in this mode.

**What to know:** a window sent to the tray does not give memory back (the engine and the grid live, ~150 MB), but the open
card hibernates according to the 10-minute rule, so the browser waiting in the tray releases the card's memory.

**Evidence:** `--traytest` → `instance_primary=ok`, `instance_second=instance=secondary exit=0` (a real
second process!), `instance_raised=ok`, `tray_icon=ok`, `tray_hides=ok`, `tray_restores=ok`, `tray_quit=ok`,
`fallback_tray=absent close=closed`, `tray_and_instance=ok`.
## 11. Done: one window, and the dark pages (`--themetest`, ADR-044)

Three complaints, three answers, all measured:

1. **An extra console window.** The shell is a console-subsystem exe, and Windows gives it its own console
   window for shortcut launches (measured: `ConsoleWindowClass` among the process's windows). Now `DropOwnConsoleWindow`
   drops it if we are alone on it; if a shell is also attached to it (every self-test), it stays its own. The
   GUI-subsystem path was tried and rejected: PowerShell does not wait for and does not return the output of a GUI program,
   which would silently disable the gate.
2. **The grid did not follow the control bar** — it had its own `data-theme` switch, and the shell push lagged
   behind by one click. Fixed: the control bar's decision drives it, both on theme change and on grid load.
3. **The web page stayed white** (the Wikipedia article), while the window was amoled — this was the behavior
   according to ADR-037, not a bug. New switch: **"🌙 Dark pages"** (off by default, remembered), which inverts
   the page with the engine's `set_forced_dark` op and a named style element.

**Evidence** (`--themetest`): `theme_chrome` 244 / 58 / 40 / 13 (light/grey/dark/amoled), `grid_capture=light`
`luma=240` → `grid_capture=amoled luma=50`, `grid_theme_attr=amoled`, `dark_capture=mode_off luma=251` →
`mode_invert 4` / `mode_dim 39` / `mode_warm 33`, `dark_page_modes=ok`, `theme_wcag_aa=ok`, `theme_icons=ok`,
`theme_check=ok`.

**What the user needs to know:** the **degree** of darkening is chosen by *them* (Off / Dark / Dark
grey / Dark warm), the setting stays in the profile (`QSettings`, `ui/page_dark_mode`), and the inversion is
approximate (image and video colors, even re-inverted, may differ from the original).

**Related rule:** `08-DIZAJN.md` §8–9 and `15-HOZZAJARULAS.md` §4 — one visible window, and dropping the
own console window.

## 12. Pending developments (state of 2026-11-19)

This list was made from the code and the log, not from the `04-UTEMTERV` markers: the roadmap's checkmarks are outdated
(several done things stand without a checkmark, see below), so the real state is this chapter.

**Recently done (the slices of 2026-11-18/19):** tray icon + single-instance launch (ADR-043), one visible window
(dropping the own console window), the UI themes **measured** per pixel + WCAG AA, the grid follows the control bar,
the four dark degrees of pages (ADR-044/045), the **shell log file** (`logs/shell.log`, rotated), the **search
diacritic-independent** (ADR-047: `kave` finds the `Kávézó` card, an empty result names the term), the
**cleanup media exemption** (ADR-048), the **500-card grid measured** (ADR: `--gridscaletest`, 13 rendered
DOM elements out of 503), the **page-text search** (ADR-049: the page text captured at freeze is also findable),
the full lifecycle of the **containers** (ADR-050…054), the **labels** (ADR-055: on the card, in the core
normalized, the search sees it without diacritics too), and the **workspaces** (ADR-056: the tab bar from the
core's `workspaces.json`, the card's `workspace` field is stored data — instead of the old `index > 12` guess).

**Product features (large items):**

- **Virtualized grid with 500 cards** — **measured** (`--gridscaletest`): 503 cards, 13 rendered DOM elements, 20 ms
  to the first render; the scroll jump cannot be measured in this harness (no layout height), which the test
  states explicitly. Open: per-container permission/proxy (the editable home page and the per-card session
  override are done: slices 106/117/118 and 121).
- **Workspaces** — **done** (ADR-056/057/058, `--workspacetest`): the tab bar comes from the core's `workspaces.json`
  (`workspaces_list`/`workspace_upsert`/`workspace_delete`), the card's `workspace` field is **stored data** (the old
  `index > 12` guess fixed), the tab bar has a lifecycle: `＋`, double click = rename, `×` = delete (the
  core's `in_use` response shows), and the card's workspace can be switched with the card's own selector (ADR-058).
  The drag-and-drop is also done (ADR-058 confirmed, `--workspacedragtest`, slice 123).
- **Profile relocation** — **done in the core and on the bridge** (ADR-059/060, `--profiletest`):
  `profile_export`/`profile_import` in a JSON bundle (`schema`, `cards`, `containers`, `workspaces`,
  `sessions`, `rules`); unknown schema is rejected, the import validates first, the bridge expects the `profile.export/import`
  route (on the page `antikoltProfileWrite/Read`). Open: the **button** and the native file picker (not clickable
  in the harness, therefore `not-measurable`), and the encryption of the bundle (a real decision because of the session rows).
- **Containers** — **done** (ADR-050/051/052/053, `--containertest`): the card's colored dot and container
  selector, the chip legend + filter, the table in the core (SQLite rows, ADR-070), and the UI lifecycle: `＋`
  container, double click = rename, `×` = delete — for a used container **with relocation** (ADR-054:
  the core first relocates with `move_to`, then deletes; the legend offers it). **Per-container permission/proxy: done
  and measured** (ADR-071, slice 122) — the core stores it, the shell builds it with `surfacePolicyFromEntry`, the engine applies it
  (`--containerpolicytest`, `--partitiontest`, `--containertest`).
- **Storage** — **done** (ADR-069/070, log 96–97): all four stores are **SQLite rows** (`antikolt.sqlite3`,
  `user_version=4`, WAL) — cards, containers, workspaces, sessions; the four `*.json` **exports** (on takeover,
  on shutdown, on `db_status(sync)`); the pre-switch profile is taken over once per store (`meta` markers),
  an unreadable old document stops the core instead of overwriting it, and the empty profile is not re-seeded;
  `db_status` checks all four exports row by row. Open: the hardware GPU path (VECTOR), F3 and OAuth.
- **OAuth/SSO popup** in the correct partition (max. 60 s, documented I1 exception — the mechanism is done, the
  login flow is not wired up).
- **Download manager, Ctrl+F, PDF, PiP, printing, post-crash recovery, HU/EN interface,
  password and bookmark import, Linux (Nobara/KDE/Wayland) + RPM.**
- **Media exemption from the cleanup** (§2) **done** (ADR-048, `--mediacleantest`): the playing card stays. The
  `test.hang` page-side hook from §3 and the rendered note measurement are still open, however.
- **Page-text search** **done** (ADR-049, `--searchtest` `search_page_text=ok`): the page text of frozen cards
  is also searchable. What remained: there is no capture for a live card (it is made at the moment of freezing); the `page_text`
  field is already known to the schema (`0002`), and the card's row lives in the database (ADR-069).
**Vault and passkey (F4/F4b):** DEK/KEK, optional master password, auto-lock, KDBX export, fill in isolated
world with origin binding, TOTP + clipboard cleanup, software authenticator (`create`/`get` with a test RP),
honest notice that there is no hardware protection. **Blocks:** it needs `cargo vendor`, and for that
network (the dependencies are not on disk).

**Memory (F6):** budget controller with bands and a media-quality limit, per-card breakdown, technique ladder
with luminance check, five resource profiles, raster home page (engine stopped), tuning report, as well as
the S3/S6 assertions of the test bench. A **`pressure` signal is already computed** from the `warn`/`hard` bands (ADR-020, `--memorypressuretest`,
slice 124); the actual **action** (the video-resolution limit and the action bound to pressure) is still to come.

**Release:** SignPath application (**human step**: public repo + review) → signed exe; Store identity +
local MSIX install (Partner Center + certificate); the `release.yml` scaffold never ran; updater
(Ed25519 signature) and installer are still to come.

**Smaller, known gaps:**

- The "stale preview" branch of `--previewqueue` is still unmeasured in real use; the `sha256` blob store is **done**
  (ADR-077 — `previewContentName` in the shell, with Qt `QCryptographicHash::Sha256`).
- Per-page theme exception (§4c) is intentionally absent; the `04-UTEMTERV` checkmarks must be aligned to the code.
- The GUI-subsystem switch (instead of the console window) would require rewriting the harness's `Start-Process -Wait` — now
  it is not worth it, it is documented.
