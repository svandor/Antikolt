# 16 — Development log

> A chronological log of what was completed, **how it was verified**, and what is blocked. The plan is in the
> `00`–`15` documents; this log records the reality.

## 2026-11-24 (one hundred sixty-sixth slice) — the full HU→EN documentation translation (M8)

All 22 documents in `docs/HU/00`–`21` are now translated into English under `docs/EN/` — after the UI
(slice 164), the **documentation** is bilingual too. Hungarian remains authoritative; the English files are
translations.

### What changed

- **`docs/EN/00`–`21`:** 22 new English documents (a 1:1 mirror of `docs/HU/`, ~11,400 lines).
- **`docs/EN/README.md`:** the "Documents" table now points at the English documents (`00`–`21`), "18 ADRs"
  fixed to "97 ADRs"; the intro states Hungarian is authoritative.

### Evidence (measured)

```
check-repo → ALL CHECKS PASSED; "Markdown links: every relative link resolves" (including the 22 new EN
             documents), cargo test 190 green (93+69+12+1+15); all 22 files' line counts match their HU
             source (05: 1978, 16: 5780 lines — the chunks reassembled with exact parity)
```

### What came up

- The translation ran in a free-chain workflow: 31 units (20 whole documents + 11 line-range chunks for the
  two large files), **31/31 OK** in one pass. `05-DONTESEK` (1978 lines) and `16-FEJLESZTESI-NAPLO`
  (5780 lines) were chunked by line ranges and reassembled on the main thread — parity is the truncation check.
- Code tokens (`--selftest`, `db_status=ok schema=11`, `user_version=11`), relative link targets and numbers
  carried over untouched (the link check is green).

### What remains open

- The Linux package (M8), the release (human: SignPath/Store, NVDA) and the updater/installer (Ed25519) code.

## 2026-11-24 (one hundred sixty-fifth slice) — version bump v0.8 → v0.9, closing out M7

The maintainer chose the **full bump**: the `v0.8` badge → `v0.9`, and the package version `0.1.0` →
`0.9.0` — with this, M7 (vault + passkey done, PiP deferred, ADR-095/096/097) is formally closed.

### What changed

- **`Cargo.toml`:** `workspace.package.version` `0.1.0` → `0.9.0` (this provides the `VERSION` constant and the `welcome`
  "core.version" field).
- **`CMakeLists.txt`:** `project(... VERSION 0.1.0)` → `0.9.0` (the output of `--version` / `--selftest`).
- **`Cargo.lock`:** the four workspace crates' version is `0.9.0`.
- **`README.md`:** badge + status line `v0.8` → `v0.9`; the "68/77 self tests" → "79 self tests / 82 checks";
  the "vault/passkey not yet present" → done, PiP deferred.
- **`docs/EN/README.md`:** `v0.8` → `v0.9`, ADR range `…097`, "77 self tests" → "79".
- **`apps/desktop-shell/README.md`:** the `--selftest` sample output `0.1.0` → `0.9.0`.

### Evidence (measured)

```
Gate (v0.9.0, GPU disabled) → ALL CHECKS PASSED — 82 checks
  (79 self tests + build + core service + check-repo), cargo test 190 green
  (93+69+12+1+15), db_status=ok schema=11 (user_version=11)
```

### What surfaced

- The IPC `PROTOCOL_VERSION` (u16 = 2) is independent of the crate version: the `0.1.0` → `0.9.0` only changes the
  displayed version text (About, `--version`, the `welcome` "core" object), not the wire
  protocol — there is no self test that expects the old version text.
- The two outdated numbers in `21-ALLAPOT.md` ("user_version=7", "38 self tests") were corrected from the gate's log
  ("user_version=11", "79 self tests").

### What remains open

- The full HU→EN **documentation** translation (M8, next slice), the Linux package (M8) and the release
  (human: SignPath/Store, NVDA).

## 2026-11-24 (one hundred sixty-fourth slice) — the full HU→EN translation (first half of M8)

The bilingualism of the interface **became complete**: the chrome (Qt widgets, menus, dialogs, tooltips, the
session log, the PDF/profile/bookmark dialogs, the tray), **every** message of the status bar and
every text of the grid/card JS appears in English in English mode — there is no more "stuck" Hungarian left.

### What changed

- **`apps/desktop-shell/src/i18n.h`:** ~60 new HU→EN entries (session log + rule editor,
  dialog/button/tooltip labels, theme and page-darkening labels, the zoom/page-theme/dark-pages
  live+error variants, tray message).
- **`apps/desktop-shell/src/main_window.cpp`:** ~50 strings wrapped in `t(...)` — the previously
  "hard-coded" `setStatus(...)` calls, widget labels, tooltips and the `themeLabel()`/`pageDarkModeLabel()`
  call sites.
- **`home/app.js` + `home/antikolt-card.js`:** ~30 strings via `t()` (workspace/container editor,
  proxy and permission editor, the card pin/note/delete/label tooltips), the template strings
  (`… törlése`, `… szűrése`) with a `pageLanguage()` ternary; the EN tables extended (~40 entries).

### Evidence (measured)

```
Gate → ALL CHECKS PASSED (82 checks, exit 0); languagetest=ok · gridlangtest=ok · check-repo=ok
       (home/app.js parses), Rust 190 green (93+69+12+1+15)
```

### What surfaced

- The survey was done by 3 parallel free-chain subagents; one of them (the "shell-chrome" surveyor), despite the "read
  only" instruction, **partially edited** (i18n.h + main_window.cpp), moreover incompletely
  (wrapped strings without the map entry) — I reverted its work and wrote the complete, consistent
  fix on the main thread. The free chain is good for surveying, but the load-bearing editing is done by
  Pro.
- The template labels (`„${name} törlése"` → `„Delete ${name}"`) switch word order, so they cannot go into the
  exact mapping of `t()` — the `pageLanguage()` ternary is the solution (like the existing `cardCount`).

### What remains open

- The internal `tr()` error strings of `core_client`/`engine_client` and the ok-strings of `replyToPage`/`abortPreviewWorker`
  remain **intentionally** (dev/JS-facing, not user-facing); the Linux package (second half of M8)
  and the release (human steps) remain.

## 2026-11-24 (one hundred sixty-third slice) — passkey key export from the virtual authenticator (M7)

The vault's passkey loop is closed: the CDP virtual authenticator's full credential (credentialId + privateKey +
rpId + userHandle + signCount) can be exported, not just its count.

### What changed

- **`apps/engine-host/src/host.cpp`:** `webauthn_credentials` now also returns the full credential list besides
  `count` (JSON `credentials` field); new `webauthn_add_credential` command (`WebAuthn.addCredential`)
  injects a fixed resident credential (the page-side `create` is focus-gated, not available in the harness).
- **`apps/desktop-shell/src/`:** `webauthnAddCredential` + `--passkeyexporttest` measures the credentialId and the
  privateKey over the injection → export round.

### Evidence (measured)

```
--passkeyexporttest → passkey_export=ok (exit 0):
  passkey_export_setup=ok · passkey_export_inject=ok ·
  passkey_export_credentials=ok count=1 has_id=yes has_key=yes
```

### What surfaced

- The `WebAuthn.addCredential` + `getCredentials` round works: the fixed EC P-256 PKCS#8 credential can be injected,
  and the full credential (including the privateKey) can be read back — this is the basis of key export (the vault's
  passkey storage is already measured in slice 158, so the chain authenticator → vault is closed).
- The test first died with `0xC0000409`, then failed in the gate with `count=0` — because of two use-after-
  free errors: the embedded async lambdas captured the `authenticator` `QString` **and** the `id` interface
  identifier by reference (`[&]`), and referenced them after the setup handler returned; the
  by-value capture (`[&, authenticator, id]`) fixed both.

### What remains open

- The full `navigator.credentials.create/get` on the real interface (`not-measurable`, focus gesture); PiP
  **deferred** (2026-11-24 decision).



The views framework (`CefBrowserView`/`CefWindow`) **loads a page**, but CHROME-style views
**rejects the custom cache path** — the basis of partition isolation.

### What changed

- **`apps/engine-host/src/`:** `--viewstest` (`RunViewsTest`) + `ViewsTestDelegate` (CHROME style):
  `CefBrowserView::CreateBrowserView` + `CefWindow::CreateTopLevelWindow` instead of windowed `SetAsChild`.

### Evidence (measured)

```
--viewstest → views_load=ok, views_partition_context=rejected, exit 0
The rejector is isolated to cache_path: empty-settings context → accepted, cache-only context → rejected.
```

### What surfaced

- The views framework **works** (loads a page, in CHROME style) — the basis for hosting the PiP
  window is ready.
- But CHROME-style views **rejects the custom cache path** (the basis of partition isolation,
  ADR-009/071): `CreateBrowserView` returns null for a secondary-profile context (`VerifyBrowserContext()`
  is false).
- Therefore the views switch is **not drop-in**: rethinking partitions (Chrome profiles) or staying on the windowed
  PiP path (with the crash) requires a decision (ADR-097).

### What remains open

- **A decision is needed:** partitions as Chrome profiles, or deferring PiP with the windowed crash.



The Document-PiP **window** measured: `requestWindow()` opens it, but the windowed mode's shutdown crashes.

### What changed

- **`apps/engine-host/src/`:** `--pipwindowtest` (`StartPipWindowProbe`): calling `requestWindow()` with a CDP
  `userGesture` (synthetic gesture), `pip_window=ok|rejected:<error>`. Diagnostic, not a gate step.

### Evidence (measured)

```
--pipwindowtest → pip_window=ok, then EXIT=0xC0000005 (access violation in shutdown)
pointer identifiers: OnBeforePopup(url=about:blank) → 2nd browser (OnAfterCreated) → 3 different closing
browsers in OnBeforeClose (including ones that never came into OnAfterCreated)
scripts/verify.ps1 → ALL SELF TESTS PASSED (81 checks, --pipwindowtest is NOT in the gate because of the crash)
```

### What surfaced

- `documentPictureInPicture.requestWindow()` **opens the PiP window** (the Promise resolves, the window
  has a document) — without a views switch, on the current windowed engine.
- In windowed mode Document-PiP creates an **internal window cascade** (beyond the `OnBeforePopup` `about:blank`
  popup, further windows that close in `OnBeforeClose` but never appear in `OnAfterCreated`)
  → the host's one-popup model cannot track this, `CefShutdown` faces a live internal
  window and dies with `0xC0000005`.

### What remains open

- **Hosting** the PiP window (the views framework `CefBrowserView`/`CefWindow`, or deeper CEF work) —
  a **Pro model is needed** for the remaining crash; the key export/import into the vault.



The ADR-094 PiP conclusion ("a views switch is needed") was based on a false premise, and was refuted by measurement.

### What changed

- **`apps/engine-host/src/main.cpp`:** `--pipchecktest` now loads the **secure** `antikolt://blob/pip-test.html`
  (with `WritePipTestPage`, following the pattern of `WritePasskeyTestPage`).
- **`apps/engine-host/src/host.cpp`:** `StartPipProbe` runs in `OnLoadEnd` (not in `OnAfterCreated`, so it
  measures the actually loaded page), and reports both `window.isSecureContext` and
  `'documentPictureInPicture' in window` (`pip_secure=0|1 pip_api=present|absent`).
- **`scripts/verify.ps1`:** `pipchecktest` is a gate step (Direct).

### Evidence (measured)

```
--pipchecktest (without flag)                          → pip_secure=1 pip_api=present (exit 0)
--pipchecktest (with DocumentPictureInPictureAPI flag) → pip_secure=1 pip_api=present (exit 0)
scripts/verify.ps1                                     → ALL SELF TESTS PASSED (81 checks, pipchecktest ok)
```

### What surfaced

- The earlier `pip_api=absent` was a **confound** of the `data:` URL measurement: `data:` is an opaque (not
  secure) origin, and `documentPictureInPicture` is `[SecureContext]`-gated.
- In CEF 158 there is **no** `chrome_runtime` setting (Alloy ceased to exist in CEF 128; CEF 158 is
  Chrome-runtime-only) — the engine already runs on the Chrome runtime, **there is nothing to switch over**.
- The `DocumentPictureInPictureAPI` flag is on by default since M116, so the flag is a no-op.

### What remains open

- The actual PiP: wiring up `documentPictureInPicture.requestWindow()` and measuring the behavior of the popping-up
  PiP window — the CEF Chrome runtime renders the PiP window as a fully styled popup (CEF #3448).



The passkey-export side (reading the virtual authenticator) is measured.

### What changed

- **`apps/engine-host/src/`:** `webauthn_credentials` command (`WebAuthn.getCredentials`, `{count}`).
- **`apps/desktop-shell/src/`:** `EngineClient::webauthnCredentials` + `--passkeyexporttest`.
- **`scripts/verify.ps1`:** `passkeyexporttest` gate step.

### Evidence (measured)

```
--passkeyexporttest → passkey_export=ok (exit 0): setup ok, credentials count=0 (empty authenticator)
scripts/verify.ps1 → ALL CHECKS PASSED (80 checks)
```

### What remains open

- The full `navigator.credentials.create/get` on the real interface (`not-measurable`, focus gesture) —
  reading the authenticator (`getCredentials`) is already commandable; the PiP views architecture switch.

## 2026-11-23 (one hundred fifty-eighth slice) — passkey storage measured (M7)

The vault's passkey storage is measured end-to-end: the `SecretKind::Passkey` path writes/reads the `passkey`
field.

### What changed

- **`apps/desktop-shell/src/main.cpp`:** `runPasskeyStoreCheck` + `--passkeystoretest`.
- **`scripts/verify.ps1`:** `passkeystoretest` gate step.

### Evidence (measured)

```
--passkeystoretest → passkey_store=ok (exit 0): unlock/create/get all ok
scripts/verify.ps1 → ALL CHECKS PASSED (79 checks)
```

### What remains open

- The full `navigator.credentials.create/get` on the real interface (`not-measurable`, focus gesture) +
  the key export from the virtual authenticator into the vault; the PiP views architecture switch.

## 2026-11-23 (one hundred fifty-seventh slice) — de-risking PiP: the flag is not enough (M7)

The necessity of the PiP views switch is empirically confirmed.

### What changed

- **`apps/engine-host/src/`:** `--pipchecktest` diagnostic probe (`'documentPictureInPicture' in window`).

### Evidence (measured)

```
--pipchecktest (without flag)                              → pip_api=absent
--pipchecktest (with DocumentPictureInPictureAPI flag)     → pip_api=absent
```

### What surfaced

- `--enable-features=DocumentPictureInPictureAPI` **does not by itself make** the
  Document-PiP API available in the Alloy/windowed runtime — the **views architecture switch is really necessary**
  (CefBrowserView/CefWindow + `chrome_runtime`), a separate large slice.

### What remains open

- The views architecture switch (Document-PiP lives only in the Chrome/views runtime); the passkey
  key export/import into the vault.

## 2026-11-23 (one hundred fifty-sixth slice) — de-risking the passkey flow (M7)

Passkey is now commandable, and the limits of the whole flow are measured.

### What changed

- **`apps/engine-host/src/`:** `webauthn_setup` command (focus emulation + `WebAuthn.enable` +
  `addVirtualAuthenticator`, `{authenticator_id}`).
- **`apps/desktop-shell/src/`:** `EngineClient::webauthnSetup` + `--passkeyflowtest` (localhost
  `ProbeOrigin` + `navigator.credentials.create`).
- **`scripts/verify.ps1`:** `passkeyflowtest` gate step.

### Evidence (measured)

```
--passkeyflowtest → passkey_flow=de-risked (exit 0):
  passkey_setup=ok · passkey_create=focus-blocked (NotAllowedError: does not have focus)
```

### What surfaced (the essence of the de-risking)

- The custom `antikolt://` scheme is not enough for WebAuthn (`invalid domain`) — **localhost is needed**.
- The full `create` **requires a focus gesture**, which the offscreen harness cannot provide → the full
  `create`/`get` works on the real interface, `not-measurable` in the harness.

### What remains open

- The full `navigator.credentials.create/get` on the real (focused) interface + the key export/import into
  the vault; PiP (views architecture switch).

## 2026-11-23 (one hundred fifty-fifth slice) — the vault panel: metadata-only list (M7)

The vault is now visible in the shell too: a panel unlocks the vault and shows a metadata-only list —
without secrets.

### What changed

- **`apps/desktop-shell/src/vault_panel.{h,cpp}` (new):** `VaultPanel` (master password field + Unlock +
  table), with `vault_list` (metadata: site/type/label), never with `vault_get`.
- **`apps/desktop-shell/src/main.cpp`:** `runVaultPanelCheck` + `--vaultpaneltest`.
- **`scripts/verify.ps1`:** `vaultpaneltest` gate step.

### Evidence (measured)

```
--vaultpaneltest → vault_panel=ok (exit 0): locked_empty/rows=1/shows_origin/no_secret all ok
scripts/verify.ps1 → ALL CHECKS PASSED (77 checks)
```

### What remains open

- The add/edit/TOTP display in the panel; the full `navigator.credentials`
  passkey flow; PiP (views architecture switch).

## 2026-11-23 (one hundred fifty-fourth slice) — autofill as a command: `fill_credential` (M7)

Autofill now also works as a command: the shell sends `fill_credential`, the engine fills the form in the
isolated world, and the secret does not leak.

### What changed

- **`apps/engine-host/src/`:** `fill_credential` command (protocol) + `FillCredential` class (the
  credential JSON-quoted into the script, a 4-step CDP sequence, `{filled: true}` response).
- **`apps/desktop-shell/src/`:** `EngineClient::fillCredential` + `--fillcredentialtest` self test.
- **`scripts/verify.ps1`:** `fillcredentialtest` gate step.

### Evidence (measured)

```
--fillcredentialtest → fill_credential=ok (exit 0): fill_reply/fill_form/fill_no_leak all ok
scripts/verify.ps1 → ALL CHECKS PASSED (76 checks)
```

### What remains open

- The graphical vault panel; the full `navigator.credentials` passkey flow; PiP
  (views architecture switch).

## 2026-11-23 (one hundred fifty-third slice) — autofill lookup by origin (M7)

The first half of password fill: the core finds the credential matching the page's origin.

### What changed

- **`crates/antikolt-core-service/src/credentials.rs`:** `get_for_origin` (exact → subdomain match,
  against suffix attacks) + `hostname`/`origin_matches` helper functions.
- **`crates/antikolt-core-service/src/main.rs`:** `vault_get_for_origin` IPC (`origin` → `credential`
  or `credential: null`).
- **`tests/service.rs`:** `the_vault_resolves_a_credential_by_origin_for_autofill`.

### Evidence (measured)

```
cargo test → 190 green: 93 + 69 + 12 + 1 + 15   (the +3 lookup tests)
scripts/verify.ps1 → ALL CHECKS PASSED (75 checks)
```

### What remains open

- The `fill_credential` command in the engine host + the shell orchestration; the graphical vault panel; the full
  `navigator.credentials` passkey flow; PiP (views architecture switch).

## 2026-11-23 (one hundred fifty-second slice) — autofill in an isolated world: spike (M7)

A de-risking spike of the actual password-fill mechanism: the fill script running in the isolated world
fills the login form, the page's main world sees the filled value, but never the secret.

### What changed

- **`apps/engine-host/src/`:** `--autofilltest` flag + `EngineClient::StartAutofillProbe` (getFrameTree →
  createIsolatedWorld → fill in the world with the native value setter → verification in the main world),
  `HostState.autofill_done/ok` and the exit code.
- **`scripts/verify.ps1`:** `autofilltest` gate step.

### Evidence (measured)

```
--autofilltest → autofill=ok verdict=ok (exit 0)
scripts/verify.ps1 → ALL CHECKS PASSED (75 checks)
```

### What remains open

- The shell↔core orchestration (fetching the credential by origin + the `fill_credential` command); the
  graphical vault panel; the full `navigator.credentials` passkey flow; PiP
  (views architecture switch).

## 2026-11-23 (one hundred fifty-first slice) — fill in an isolated world: spike (M7)

A de-risking spike of the security basis of password fill: does the isolated world work in CEF 158?
**Yes** — CDP `Page.createIsolatedWorld` creates it, and a probe set inside it is invisible to the main
world.

### What changed

- **`apps/engine-host/src/`:** `--isolatedworldtest` flag + `EngineClient::StartIsolatedWorldProbe`
  (4 steps: getFrameTree → createIsolatedWorld → Runtime.evaluate in the world → Runtime.evaluate in the main
  world), `HostState.isolated_world_done/ok` and the exit code.
- **`scripts/verify.ps1`:** `isolatedworldtest` gate step.

### Evidence (measured)

```
--isolatedworldtest → isolated_world=ok main_sees=undefined (exit 0)
scripts/verify.ps1 → ALL CHECKS PASSED (74 checks)
```

### What remains open

- The actual fill (fetching the credential from the core by origin + the form-fill script);
  the graphical vault panel; the full `navigator.credentials` passkey flow; PiP
  (views architecture switch).

## 2026-11-23 (one hundred fiftieth slice) — the vault from the shell: CoreClient vault methods + `--vaulttest` (M7)

The vault can now also be controlled from the shell — not only from the core's Rust tests.

### What changed

- **`apps/desktop-shell/src/core_client.{h,cpp}`:** `vault_status`/`vault_unlock`/`vault_lock`/
  `vault_list`/`vault_create`/`vault_get`/`vault_delete` over the core protocol.
- **`apps/desktop-shell/src/main.cpp`:** `runVaultCheck` + `--vaulttest` flag (unlock → create →
  list without secrets → get → lock → the locked vault refuses).
- **`scripts/verify.ps1`:** `vaulttest` gate step.

### Evidence (measured)

```
--vaulttest → vault_check=ok (exit 0): locked_before/unlock/create/list/get/locked_after/refuses all ok
scripts/verify.ps1 → ALL CHECKS PASSED (73 checks)
```

### What remains open

- The graphical vault panel + the isolated-world autofill; the full `navigator.credentials` passkey flow;
  PiP (views architecture switch).

## 2026-11-23 (one hundred forty-ninth slice) — passkey F0 spike: the CDP virtual authenticator works (M7)

A de-risking spike of the passkey's "B" path (12-PASSKEY.md): does
`WebAuthn.addVirtualAuthenticator` work in CEF 158 over the DevTools transport? **Yes** — but only after
`WebAuthn.enable`.

### What changed

- **`apps/engine-host/src/`:** `--webauthntest` flag + `EngineClient::StartWebAuthnProbe` (a two-step
  CDP sequence: `WebAuthn.enable` → `WebAuthn.addVirtualAuthenticator`), `HostState.webauthn_done/ok`
  and the exit code (against the "lying green": non-zero if the authenticator is not created).
- **`scripts/verify.ps1`:** `webauthntest` gate step (Direct, on the engine binary).

### Evidence (measured)

```
--webauthntest → webauthn_authenticator=ok id=913755a5-3355-4540-9386-b8aa868dbdc7 (exit 0)
scripts/verify.ps1 → ALL CHECKS PASSED (72 checks)
```

### What remains open

- The full `navigator.credentials.create/get` flow (secure-context test page, key export/import into the
  vault); the shell vault interface + isolated-world autofill; PiP (views architecture switch).

## 2026-11-23 (one hundred forty-eighth slice) — Chrome/Firefox password CSV import (M7)

The second half of the "vault + CSV import" decision: the Chrome and Firefox password exports can be written into the vault.

### What changed

- **`crates/antikolt-core-service/src/import.rs` (new):** `parse_credential_csv` (by-name
  column mapping, UTF-8 BOM, quoted commas, skipping incomplete rows) + `to_new_credentials`.
- **`crates/antikolt-core-service/src/main.rs`:** `vault_import_csv` IPC — `csv` text + optional
  `container_id` → `{imported, skipped}`.
- **`tests/service.rs`:** end-to-end `the_vault_imports_a_chrome_password_csv` (import → list without
  secrets → get).

### Evidence (measured)

```
scripts/verify.ps1 → ALL CHECKS PASSED (71 checks)
cargo test         → 187 green: 93 + 67 + 11 + 1 + 15   (the +7 import tests: 6 unit + 1 end-to-end)
```

### What remains open

- Passkey (WebAuthn virtual authenticator, F0 spike); the shell vault interface + isolated-world
  autofill; PiP (views architecture switch).

## 2026-11-23 (one hundred forty-seventh slice) — TOTP code generation (RFC 6238, M7)

The middle leg of the "passwords + TOTP + passkey" decision: the vault already stores the TOTP secret (slice 146),
now it **also computes a code** from it — in the core, so the secret never leaves the core.

### What changed

- **`crates/antikolt-core/src/totp.rs` (new):** `totp_code` — RFC 6238 (HMAC-SHA-1/256/512 + dynamic
  truncation, RFC 4226) + base32 secret; the `now_ms` parameter, so the RFC vectors run without a clock.
- **`crates/antikolt-core-service/src/credentials.rs`:** `totp_code_for` + `totp_params` (the RFC default:
  6 digits, 30 s, SHA-1).
- **`crates/antikolt-core-service/src/main.rs`:** `vault_totp` IPC — `id_of_credential` → `{code,
  seconds_remaining}`; the opening of the secret happens in the core.
- **Dependencies:** `hmac`, `sha1`, `sha2`, `data-encoding`.

### Evidence (measured)

```
scripts/verify.ps1 → ALL CHECKS PASSED (71 checks)
cargo test         → 180 green: 93 + 61 + 10 + 1 + 15   (the +6 TOTP tests: 5 RFC vectors + 1 store endpoint)
```

### What remains open

- Passkey (WebAuthn virtual authenticator); the Chrome/Firefox CSV import; the shell-vault interface +
  isolated-world fill; PiP (views architecture switch).

## 2026-11-23 (one hundred forty-sixth slice) — the vault's store: credential table + vault IPC (M7)

The second half of the password vault: after the encryption (slice 145) it now also **stores**. The `credential`
table is SQLite-only (schema 0011), the secret fields are sealed with AES-256-GCM, the DEK/KEK is in `vault_meta`,
and the `vault_*` IPC operations serve the shell over the wire.

### What changed

- **`crates/antikolt-core/src/secrets.rs`:** `random_dek` (the DEK from the CSPRNG, not from the id generator).
- **`crates/antikolt-core-service/src/sqlite_store.rs`:** schema 0011 (`credential` + `vault_meta` table) and
  the `Db` methods (`all_credentials`, `credential`, `upsert_credential`, `touch_credential`,
  `delete_credential`, `count_credentials`, `vault_meta`, `set_vault_meta`).
- **`crates/antikolt-core-service/src/credentials.rs` (new):** `CredentialStore` — the sealing/opening,
  the DEK/KEK unlock (master password + DPAPI), the lock/unlock and the 5-minute idle auto-lock; the list is meta, the get is secret.
- **`crates/antikolt-core-service/src/main.rs`:** `vault_status`/`vault_unlock`/`vault_lock`/`vault_list`/
  `vault_create`/`vault_get`/`vault_delete` IPC operations + ticking the vault on every request.
- **`tests/service.rs`:** end-to-end `the_vault_seals_serves_and_persists_credentials` (sealing,
  meta-list without secrets, get, lock, unlock after restart).

### Evidence (measured)

```
scripts/verify.ps1 → ALL CHECKS PASSED (71 checks)
cargo test         → 174 green: 88 + 60 + 10 + 1 + 15   (the +8 vault tests: 7 unit + 1 end-to-end)
```

### What remains open

- TOTP code generation; passkey (WebAuthn virtual authenticator); the Chrome/Firefox CSV import; the
  shell-vault interface + isolated-world fill; PiP (views architecture switch).

## 2026-11-23 (one hundred forty-fifth slice) — the vault's encryption: AES-256-GCM + Argon2id + DPAPI (M7)

The basis of the password vault (M7). Since ADR-031 the vault's **shape** was ready, but the encryption stood
behind `UnavailableSecrets` (the build environment had no crates.io access). The network is now available, so the
encryption per the maintainer's decision — **DPAPI + optional master password**, for passwords + TOTP + passkey —
has arrived: the vault can now seal and open.

### What changed

- **`Cargo.toml` + `crates/antikolt-core/Cargo.toml`:** the `aes-gcm` 0.10, `argon2` 0.5, `zeroize` 1
  workspace dependencies wired up (the earlier "no crate on disk" blocker is gone).
- **`crates/antikolt-core/src/secrets.rs` (new):** `AesGcmSecrets` — AES-256-GCM with a fresh 96-bit nonce per
  record and the record's AAD (origin + id, ADR-010); `from_master_password` + `generate_salt`
  (Argon2id); `secrets::dpapi` — Windows DPAPI (`CryptProtectData`/`CryptUnprotectData`,
  `CRYPTPROTECT_UI_FORBIDDEN`) in the crate's only `unsafe` module.
- **`crates/antikolt-core/src/lib.rs`:** `pub mod secrets;`.

### Evidence (measured)

```
scripts/verify.ps1 → ALL CHECKS PASSED (71 checks)
cargo test         → 166 green: 88 + 53 + 9 + 1 + 15   (the 10 new secrets tests: round-trip, AAD binding,
                      tampering, wrong password, salt uniqueness, DPAPI round-trip under Windows)
```

### What remains open

- The DEK/KEK storage and the writing/reading of the `credential` table (the vault's single writer is SQLite, ADR-069/070);
  TOTP; passkey (WebAuthn); the Chrome/Firefox CSV import; the shell-vault interface; PiP
  (views architecture switch — the maintainer chose this, a separate slice).

## 2026-11-23 (one hundred forty-fourth slice) — release files (THIRD-PARTY-NOTICES + verification, M8)

The M8 "Licence/NOTICE/AUTHORS/THIRD-PARTY/CONTRIBUTING" item: the missing third-party notice was completed,
the existing files were updated, and the gate now guards them.

### What changed

- **`THIRD-PARTY-NOTICES.txt` (new):** the detailed notice of third parties — CEF (BSD-3-Clause, with full
  text), Chromium, Qt 6 (LGPL-3.0, dynamically linked), SQLite (public domain), Rust crates
  (MIT/Apache-2.0, in `Cargo.lock`).
- **`NOTICE`:** the third-party line now points to `THIRD-PARTY-NOTICES.txt` (not "whenever the shell is ready").
- **`CONTRIBUTING.md`:** the fresh numbers (156 tests) and the outdated "the shell hasn't arrived yet" note
  removed.
- **`README.md` + `docs/EN/README.md`:** the fresh metrics (68 self tests, 156 Rust tests, ADR-081, schema 10).
- **`scripts/check-repo.ps1`:** new "Release files" check — the LICENSE/NOTICE/AUTHORS/CONTRIBUTING/
  THIRD-PARTY-NOTICES files exist and carry their essential content.

### Evidence (measured)

```
scripts/verify.ps1 → ALL CHECKS PASSED (71 checks; inside check-repo the new "release files" is also green)
cargo test         → 156 green: 78 + 53 + 9 + 1 + 15
```

### What remains open

- The **full HU→EN documentation translation** (the English `docs/EN/README.md` is the condensed summary, the detailed
  plan is Hungarian); password import; PiP (decision); the screen reader (NVDA); signing/Store and the M8 Linux package.

## 2026-11-23 (one hundred forty-third slice) — bookmark import from Netscape HTML (M7)

The first half of M7's "password and bookmark import": the **bookmark import** is done. Netscape HTML is the only
common Chrome/Firefox/Edge export, so one parser covers all three; every bookmark becomes a card.

### What changed

- **`apps/desktop-shell/src/bookmarks.h` (new):** `parseBookmarksHtml` (a tolerant regex parser: title + URL from the
  `<A HREF>` anchors, skipping `javascript:` and folders) + `newCardId` (the C++ counterpart of the grid's ULID).
- **`apps/desktop-shell/src/main_window.{h,cpp}`:** `importBookmarksFromPath` (file → parse → skipping already
  existing URLs → card upsert into the core, then refreshing the grid); in the "💾 Profile" menu a new
  "Import bookmarks…" item (with a file picker).
- **`--bookmarkimporttest` (new):** measuring the parser (3 cases + skipping `javascript:`/folder), the id shape and the
  card upsert.

### Evidence (measured)

```
scripts/verify.ps1 → ALL CHECKS PASSED (71 checks all "ok", including bookmarkimporttest and check-repo)
--bookmarkimporttest → bookmark_parse=ok · bookmark_id=ok · bookmark_upsert=ok · bookmark_cards=ok
cargo test         → 156 green: 78 + 53 + 9 + 1 + 15
```

### What remains open

- Password import (there is no target vault in the product); PiP (architecture decision); the full screen-reader
  integration (NVDA); the OAuth hookup; the human release items (SignPath, Store); the M8 Linux package.

## 2026-11-23 (one hundred forty-second slice) — automatic resource profile (power + pressure, M6)

The open half of M6's "Five resource profiles, with automatic switching": the profile can now **follow the machine**.
`Auto` mode resolves in the core to a built-in profile based on the power source and the measured memory pressure.

### What changed

- **`crates/antikolt-core/src/profile.rs`:** `ProfileKind::Auto` (serde `auto`), `params()` = `None`.
- **`crates/antikolt-core-service/src/sqlite_store.rs`:** the round-trip mapping of the `auto` string.
- **`crates/antikolt-core-service/src/main.rs`:** `resolve_profile` IPC (`{on_ac_power, pressure}` →
  `{kind, params}`), based on `ProfileKind::auto` + `params()`.
- **`apps/desktop-shell/src/core_client.{h,cpp}`:** `resolveProfile`.
- **`apps/desktop-shell/src/main_window.{h,cpp}`:** `onAcPower()` (`GetSystemPowerStatus` + the `ANTIKOLT_AC_POWER`
  override); `applyProfileParams` extracted; `refreshUserPrefs` resolves `auto` with normal pressure,
  `checkMemoryPressure` re-queries after every sample; a new "Automatic" item in the profile selector.
- **`--autoprofiltest` (new):** measures the power-source override and the core `resolve_profile` mapping (6 cases).

### Evidence (measured)

```
scripts/verify.ps1 → ALL CHECKS PASSED (70 checks all "ok", including autoprofiltest and check-repo)
--autoprofiltest   → resolve_normal/ac=balanced · resolve_normal/bat=saver · resolve_warn/ac=balanced ·
                     resolve_warn/bat=saver · resolve_hard/ac=ultra · resolve_hard/bat=ultra · auto_profile=ok
cargo test         → 156 green: 78 + 53 + 9 + 1 + 15
```

### What remains open

- The password and bookmark import (M7); PiP (architecture decision); the full screen-reader integration (NVDA);
  the OAuth hookup; the human release items (SignPath, Store); the M8 Linux package.

## 2026-11-23 (one hundred forty-first slice) — the shell's status messages are also switchable (M7, i18n)

The last item of the HU/EN interface: the shell's status-bar messages (the `setStatus` calls) were Hungarian so far. This
slice makes `setStatus` itself language-aware, so all ~80 messages switch, without a restart.

### What changed

- **`apps/desktop-shell/src/main_window.cpp`:** `setStatus` translates the Hungarian source with `t()`; for
  messages carrying format placeholders (`%1`/`%2`/`%3`), `t()` is applied to the **template**, **before** `.arg()`
  (translation must happen before substitution, otherwise the filled-in numbers won't be found in the map).
- **`apps/desktop-shell/src/i18n.h`:** the complete status map — engine/core lifecycle, card freezing,
  preview, session cleanup, profile, PDF, settings, raster — HU→EN.

### Evidence (measured)

```
scripts/verify.ps1 → ALL CHECKS PASSED (69 checks all "ok", check-repo still green)
cargo test         → 156 green: 78 + 53 + 9 + 1 + 15
```

### What remains open

- PiP (architecture decision); the full screen-reader integration (NVDA); the OAuth hookup; the password and
  bookmark import; the human release items (SignPath, Store).

## 2026-11-23 (one hundred fortieth slice) — the card's inline labels and the grid's static labels are also switchable (M7, i18n)

ADR-079 made the grid's main strings switchable; this slice closes the remaining grid side: the card's
inline editor labels and the static (HTML) labels.

### What changed

- **`home/antikolt-card.js`:** its own `t()` + a HU→EN `EN` map (the card is a separate IIFE, so its own table
   carries): the status badges (signed in / expiring soon / signed out / unknown state), the
   English of „Save", „Cancel", „Sure?", „＋tag" and the tag/editor aria-labels.
- **`home/app.js`:** `applyStaticLabels` extended with the static chrome — the „Jump to cards"
   skip link, the „Pinned" / „Cards" section titles, the theme buttons, the memory label, the container legend and the
   footer key hints (`data-i18n`); the `refreshSystem` memory-pressure tooltips and the „engine:" prefix too.
- **`home/index.html`:** the footer hints split into `data-i18n` spans so that `applyStaticLabels` labels them.

### Evidence (measured)

```
scripts/verify.ps1 → ALL CHECKS PASSED (69 checks all „ok", gridlangtest and check-repo still green)
--gridlangtest     → grid_lang_before=hu · grid_lang_after=en · grid_language_witness=en
                     (the switch mechanism used by the card and static labels too)
cargo test         → 156 green: 78 + 53 + 9 + 1 + 15
```

### Still open

- The shell **status messages** (the `setStatus` calls) are still Hungarian; the PiP (architecture decision); the full
   screen-reader integration (NVDA); the OAuth hookup; the human release items (SignPath, Store).

## 2026-11-23 (129th slice) — the grid's language now follows the shell (HU/EN, M7)

ADR-078 made the shell chrome switchable; this slice binds the **grid** (`home/app.js`) to the same
language — so the product's main surface also speaks English when the user chooses it.

### What changed

- **`apps/desktop-shell/src/main_window.{h,cpp}`:** `pushLanguageToGrid` + the `system.language` bridge; the language
  goes to the grid via the `data-language` attribute (mirroring the theme's `data-theme`), on load and on
  `setLanguage`, re-rendering through the `window.antikoltSetLanguage` hook.
- **`home/app.js`:** `t()` + HU→EN `EN` map; `setLanguage`/`restoreLanguage` (from `data-language` and
  localStorage); the counter, the empty state, the container legend, the workspace/container buttons and the
  search placeholder's English.
- **`home/index.html`:** `data-language="hu"` initial value.
- **`--gridlangtest` (new):** measures the search placeholder's „Keresés" → „Search" switch and the `window.antikoltLanguage`
  witness — via the shell's own `setLanguage` path.

### Evidence (measured)

```
scripts/verify.ps1 → ALL CHECKS PASSED (69 checks all „ok", including gridlangtest and check-repo)
--gridlangtest     → grid_lang_before=hu · grid_lang_after=en · grid_language_witness=en ·
                     grid_placeholder_after=Search — title, URL, note, tag, page text (diacritics optional)
cargo test         → 156 green: 78 + 53 + 9 + 1 + 15
```

### Still open

- The card's **inline** editor labels („Save", „＋tag" in `antikolt-card.js`) and the grid's static
  aria-labels; the shell **status messages**; the PiP (architecture decision); the full screen-reader
  integration (NVDA); the OAuth hookup; the human release items (SignPath, Store).

## 2026-11-23 (128th slice) — the shell language is switchable (HU/EN), the chrome first (M7, i18n)

The first slice of the HU/EN interface: the language-switch setting, the translation mechanism and the shell **chrome**'s
(toolbar, tray, settings dock, search bar) English. Hungarian remains the authoritative language; the status messages and the
grid are the next slices.

### What changed

- **`apps/desktop-shell/src/i18n.h` (new):** `uiText(hu, lang)` — the source is Hungarian, `en` supplies the
  English from a `QHash`; an unknown key / non-`en` language returns the source (a missing translation is never an empty label).
- **`apps/desktop-shell/src/main_window.{h,cpp}`:** `ui/language` QSettings (default `hu`); `MainWindow::t()`
  and `setLanguage()` — the switch **without restart**, re-labelling in place; the settings dock's `Language:` dropdown.
  The chrome (toolbar, tray, settings-dock labels, search bar) is wrapped in `t()`.
- **`--languagetest` (new):** measures the pure `uiText` table, the QSettings persistence and the in-place
  re-labelling — without the engine and the core.

### Evidence (measured)

```
scripts/verify.ps1 → ALL CHECKS PASSED (68 checks all „ok", including languagetest and check-repo)
--languagetest     → language_map=ok · language_default=ok · language_switch=ok ·
                     language_persist=ok · language_restore=ok · language=ok
cargo test         → 156 green: 78 + 53 + 9 + 1 + 15
```

### Still open

- The English of the **status messages and the grid** (`home/app.js` + `antikolt-card.js`) — the chrome is done, the rest is for the
  next slices; the PiP (architecture decision); the full screen-reader integration (NVDA); the OAuth
  hookup; the human release items (SignPath, Store).

## 2026-11-23 (137th slice) — the preview name is the content hash (F2, `sha256` blob store)

The open `sha256` blob-store item, which until now stood as „blocked" citing the absence of the Rust `sha2` crate. The
research concluded: the name is produced in the **shell** (the capture happens there, the bytes are there), so the `sha2`
crate is not needed at all — Qt's `QCryptographicHash::Sha256` yields the same thing, offline.

### What changed

- **`apps/desktop-shell/src/previews.h`:** `previewContentName(bytes, ext)` — the name is the hex form of the bytes' sha256
  + extension (ADR-077).
- **`apps/desktop-shell/src/main_window.cpp`:** `producePreview` and the background worker **hash the finished
  image** after the capture, and save it into the profile under the content name; identical bytes are no longer copied to a new file (dedup), only the
  record learns the same name. The freshness check looks at the stored `preview_ref`.
- **`--previewhashtest` (new):** a pure-function test, pinned to public sha256 test vectors (the known hashes of
  the empty and the „hello" inputs).
- **`--previewgentest` / `--previewworkertest`:** instead of the expected `preview_ref`, they now measure that the stored name
  is the hash of the **file's content** (`preview_content_addressed=ok` / `worker_content_addressed=ok`), and the interrupted
  worker's „no image" claim is proven by the record's empty `preview_ref`.

### Evidence (measured)

```
scripts/verify.ps1 → ALL CHECKS PASSED (67 checks all „ok", including previewhashtest and check-repo)
--previewhashtest  → hash_empty_pinned=ok · hash_hello_pinned=ok · hash_deterministic=ok ·
                     hash_distinct=ok · preview_hash=ok
--previewgentest   → preview_ref=<sha256>.png · preview_content_addressed=true · preview_generation=ok
--previewworkertest→ worker_ref=<sha256>.png · worker_content_addressed=true · preview_worker=ok
cargo test         → 156 green: 78 + 53 + 9 + 1 + 15
```

### Still open

- The rest of F7 (PiP, the full screen-reader integration, HU/EN); the OAuth hookup; the human release items
  (SignPath, Store); a live measurement of the `--previewqueue` „stale preview" branch.

## 2026-11-23 (136th slice) — the video-height cap as a display cap (F6)

The last open control action of F6: the profile's `video_max_height` was until now only **reported**
(`--memoryreport`), not applied. This slice applies it — as a **display cap**, because the research
concluded that CEF/Chromium offers no per-page video decode resolution limit (ADR-076).

### What changed

- **`apps/engine-host` (`protocol.h`/`.cpp`, `host.cpp`):** new `set_video_cap` op + `VideoCap` class — a
  named style element (`antikolt-video-cap`) injects the `video{max-height:Npx}` rule (the dark-pages
  switch trick, ADR-044/045); `0` removes it, a profile switch replaces it.
- **`apps/desktop-shell` (`engine_client`, `main_window`):** `EngineClient::setVideoCap`; the shell caches the
  derived profile's `video_max_height` (`video_max_height_`), and applies it in `loadFinished` and on
  profile switch (like theme/zoom/dark pages).
- **`--videocaptest` (new):** injects a top-level `<video>` into the grid, then walks three profiles — Fast
  (2160), Saver (720), Manual (no parameter) — and measures that the computed `max-height` and the rendered height
  follow the profile.
- **`crates/antikolt-core/src/profile.rs`:** the `video_max_height` documentation honestly says that this is
  a **display** cap, not a decode cap.

### Evidence (measured)

```
scripts/verify.ps1 → ALL CHECKS PASSED (66 checks all „ok", including videocaptest and check-repo)
--videocaptest     → video_fast_max=2160px · video_fast_present=true · video_cap_max=720px ·
                     video_cap_height=720 · video_manual_max=none · video_cap=ok
cargo test         → 156 green: 78 + 53 + 9 + 1 + 15
```

### Honest limitation (stated, not concealed)

The cap limits the **rendered box**, not the **decoded stream**: CEF has no per-page
video decode limit, so the memory saving is indirect (a smaller composited box), and the scope is the
top-level `video` (the cross-origin iframe's own document). The test reports this as `video_cap_resolution=not-measurable`, and
writes it out. ADR-076.

### Still open

- The rest of F7 (PiP, the full screen-reader integration, HU/EN); the OAuth hookup; the human release items
  (SignPath, Store).

## 2026-11-23 (135th slice) — accessibility: named controls + Ctrl+F (F7)

The sixth piece of F7: the first, measurable part of **keyboard navigation and screen-reader support**. The search bar's
controls are now **named** (the screen reader can read them out), and a self test verifies that every
interactive element of the interface can be announced, and the Ctrl+F shortcut is registered.

### What changed

- **`apps/desktop-shell` (main_window):** the search-bar field, the result counter and the close button got an explicit
  `accessibleName` (the placeholder alone is not a label, and „✕" cannot be read out).
- **`--accessibilitytest` (new):** walks the interface's buttons, text fields and dropdowns, and verifies that
  each can be announced (text or accessible name), and that the Ctrl+F shortcut is registered. Qt's
  own „decorations" (the toolbar overflow button and the text-field clear button) are excluded from the check.

### Evidence (measured)

```
scripts/verify.ps1 → ALL CHECKS PASSED (65 checks all „ok", including accessibilitytest and check-repo)
--accessibilitytest → a11y_controls=18 · a11y_missing=0 · a11y_ctrl_f=true · accessibility=ok
cargo test         → 156 green: 78 + 53 + 9 + 1 + 15
```

### Still open

- The rest of F7 (PiP, the full screen-reader integration, HU/EN); the F6 `video_max_height` research item; the OAuth
  hookup; the human release items (SignPath, Store).

## 2026-11-23 (134th slice) — viewing PDFs (F7)

The fifth piece of F7: **displaying PDFs**. CEF's built-in PDFium viewer was already in place; now a
self test records that a PDF opens as a **viewer** (not as a download), and this behaviour is also checked at the gate.

### What changed

- **`--pdftest` (new):** writes a minimal, one-page PDF to a temporary file, navigates the browser to it, and
  verifies that the load happens (`loadFinished`) **and** no download starts (`download_finished` is absent) —
  i.e. the PDF appears in the viewer.

### Evidence (measured)

```
scripts/verify.ps1 → ALL CHECKS PASSED (64 checks all „ok", including pdftest and check-repo)
--pdftest         → pdf_loaded=true · pdf_downloaded=false · pdf=ok
cargo test        → 156 green: 78 + 53 + 9 + 1 + 15
```

### Still open

- The rest of F7 (PiP, accessibility, HU/EN); the F6 `video_max_height` research item; the OAuth hookup;
  the human release items (SignPath, Store).

## 2026-11-23 (133rd slice) — automatic reload after a crash (F7)

Slice 132 added the detection; now the **second half of the recovery**: the crashed page's automatic
reload. The `CefBrowser` survives the renderer's exit, so a fresh navigation starts a new renderer — the grid
returns to the home page, the card returns to its own URL.

### What changed

- **`apps/desktop-shell` (main_window):** `live_card_url_` tracks the live card's URL; the
  `rendererTerminated` handler now immediately reloads the crashed surface (`homePageUrl()` for the grid, the
  stored URL for the card), and writes this to the status too.
- **`--crashtest` (extended):** after the `chrome://crash` crash it now also verifies that the grid
  **comes back on its own** (the home page reloads).

### Evidence (measured)

```
scripts/verify.ps1 → ALL CHECKS PASSED (63 checks all „ok", including crashtest and check-repo)
--crashtest       → crash_terminated=true · crash_status=2 · crash_recovered=true · crash=ok
cargo test        → 156 green: 78 + 53 + 9 + 1 + 15
```

The engine's log shows the full cycle: `navigate` → renderer exits (`STATUS_ACCESS_VIOLATION`) → the shell's
`navigate` (the automatic reload) → the grid loads again.

### Still open

- The rest of F7 (PDF, PiP, accessibility, HU/EN); the F6 `video_max_height` research item; the OAuth
  hookup; the human release items (SignPath, Store).

## 2026-11-23 (132nd slice) — detecting renderer crashes (F7)

The fourth piece of F7: the **first half of crash recovery** — detecting the renderer crash. The engine
already logged the event via `CefRenderProcessHandler`; from now on, with a `renderer_terminated` event, it
**also notifies the shell**, which shows an understandable status instead of the dead surface.

### What changed

- **`apps/engine-host` (host.cpp):** `OnRenderProcessTerminated` now also sends a `renderer_terminated` event
  (`surface` + `status` + `code`).
- **`apps/desktop-shell` (engine_client, main_window):** `rendererTerminated` signal; the shell writes the crash to the status,
  so the dead card does not stay silently empty.
- **`--crashtest` (new):** navigates the grid to `chrome://crash`, and verifies that the `renderer_terminated`
  event arrives.

### Evidence (measured)

```
scripts/verify.ps1 → ALL SELF TESTS PASSED (63 checks all „ok", including crashtest and check-repo)
--crashtest       → crash_terminated=true · crash_status=2 (STATUS_ACCESS_VIOLATION) · crash=ok
cargo test        → 156 green: 78 + 53 + 9 + 1 + 15
```

`chrome://crash` is the deterministic crash trigger in CEF 158 too (the renderer exits with a null-pointer
dereference) — this makes the detection measurable. The automatic reload is the next step.

### Still open

- The rest of F7 (the crash's **automatic** recovery, PDF, PiP, accessibility, HU/EN); the F6
  `video_max_height` research item; the OAuth hookup; the human release items (SignPath, Store).

## 2026-11-23 (131st slice) — downloading files (F7)

The third piece of F7: **download**. The engine downloads the file via CEF's `StartDownload`/`CefDownloadHandler`,
and reports the finished state (full path + success) with a `download_finished` event; the shell learns from the
response where the file ended up.

### What changed

- **`apps/engine-host` (host.h/.cpp):** `EngineClient` is now a `CefDownloadHandler`; the `download` op
  (`StartDownload`); `OnDownloadUpdated` sends a `download_finished` event at the finished/stopped state.
- **`apps/desktop-shell` (engine_client):** `EngineClient::download(surface, url)` + `downloadFinished` signal.
- **`--downloadtest` (new):** downloads a self-contained (offline) `data:` content, and verifies that the file
  arrived and its content matches.

### Evidence (measured)

```
scripts/verify.ps1 → ALL SELF TESTS PASSED (62 checks all „ok", including downloadtest and check-repo)
--downloadtest    → download_success=true · download_path=…\download.txt · download_bytes=23 · download=ok
cargo test        → 156 green: 78 + 53 + 9 + 1 + 15
```

Along the way I also fixed a CEF-158 API divergence: in this version `CefSettings` does **not** know
`download_default_directory`, so the test uses the finished event's `GetFullPath()` to locate the file
(not prior knowledge of the directory).

### Still open

- The rest of F7 (PiP, full print dialog, crash recovery, accessibility, HU/EN); the F6
  `video_max_height` research item; the OAuth hookup; the human release items (SignPath, Store).

## 2026-11-23 (130th slice) — find on page (F7 Ctrl+F)

The second piece of F7: **find on page**. The engine searches the text via CEF's `Find`/`CefFindHandler`, and
sends the number of matches back with a `find_result` event; the shell shows a Ctrl+F search bar (Enter = next,
Shift+Enter = previous, Esc = close).

### What changed

- **`apps/engine-host` (protocol.h/.cpp, host.h/.cpp):** `find`/`stop_find` ops; the `Command` `text`/`find_next`/
  `forward` fields; `EngineClient` is now a `CefFindHandler` (the `find_result` event: `surface` + `count` + `active`).
- **`apps/desktop-shell` (engine_client, main_window):** `EngineClient::find`/`stopFind` + `findResult` signal (the
  `wireEngine` wires it to every engine); the Ctrl+F search bar (`antikoltFindBar`) with Enter/Shift+Enter/Esc keys.
- **`--findtest` (new):** inserts a `findprobe` probe repeated 3× into the grid, and verifies that the match count is 3.

### Evidence (measured)

```
scripts/verify.ps1 → ALL SELF TESTS PASSED (61 checks all „ok", including findtest and check-repo)
--findtest        → find_probe_added=true · find_count=3 · find_active=0 · find=ok
cargo test        → 156 green: 78 + 53 + 9 + 1 + 15
```

Along the way I also fixed two CEF-158 API divergences (the `Find` expects 4, not 5 parameters; the first `OnFindResult` reports the match count
with `active=0`, the selection comes only in the next notification), and I also ruled out a full-gate flake (before the search,
a short pause + retry, because the renderer sometimes sees the DOM change late).

### Still open

- The rest of F7 (download, PiP, full print dialog, crash recovery, accessibility, HU/EN); the F6
  `video_max_height` research item; the OAuth hookup; the human release items (SignPath, Store).

## 2026-11-23 (129th slice) — saving a page to PDF (the F7 opening)

The F6 memory governor is done (the `warn`/`hard` band actions are slices 127–128); the remaining F6 is the `video_max_height`
research item. With that, the first piece of **F7 (desktop browser basics)** opened: **printing** in „save to PDF"
form. The engine writes the file with its own compositor (`CefBrowserHost::PrintToPDF`), the shell names it.

### What changed

- **`apps/engine-host` (protocol.h/.cpp, host.cpp):** new `print_pdf` op — `path` field in `Command`, `PdfPrint`
  (`CefPdfPrintCallback`) class, dispatch, the capabilities list and the „known ops" error message extended.
- **`apps/desktop-shell` (engine_client, main_window):** `EngineClient::printPdf(surface, path, …)`; `MainWindow`
  `printToPdf()` with a native file dialog; a `🖨️ PDF` toolbar button (the dialog is `not-measurable`, the path is
  measured by `--printtest` — the profile-button pattern).
- **`--printtest` (new):** saves the grid to PDF, and verifies the file's existence, size and the `%PDF-` magic.

### Evidence (measured)

```
scripts/verify.ps1 → ALL SELF TESTS PASSED (60 checks all „ok", including printtest and check-repo)
--printtest        → print_ok=true · print_bytes=210429 · print_magic=%PDF- · print=ok
cargo test         → 156 green: 78 + 53 + 9 + 1 + 15
```

### Still open

- The rest of F7 (Ctrl+F, download, PiP, full print dialog, crash recovery, accessibility, HU/EN); the
  F6 `video_max_height` research item; the OAuth hookup; the human release items (SignPath, Store).

## 2026-11-23 (128th slice) — the warning band pauses the preview (F6)

Slice 127 added the „hard = hibernate" action; now the „warn = release background resources" came. The
governor now samples **the grid too** (not only the live card), and on reaching the `warn` band it pauses the
background preview worker for 5 minutes — `hard` still hibernates the live card.

### What changed

- **`main_window.{h,cpp}`:** `memory_check_` now runs from the constructor, and samples whichever surface is
  live (grid or card). `checkMemoryPressure` sets `preview_paused_until_ms_` (5 minutes) at the `warn` band, and
  at the `hard` band the same **and** hibernates (if a card is live). `startPreviewWorker` rejects during the pause; the
  `system.memory` response gains a new `previewPaused` field.
- **`--memorywarnpausetest` (new):** a 0.5 MiB `warn` + 1 GiB `hard` (never fires) → the ~110 MiB grid falls into the `warn`
  band, and the response is `pressure=warn` + `previewPaused=true`.

### Evidence (measured)

```
scripts/verify.ps1     → ALL SELF TESTS PASSED (59 checks all „ok", including memorywarnpausetest and check-repo)
--memorywarnpausetest  → warn_used=113532928 · warn_pressure=warn · warn_preview_paused=true · memory_warn_pause=ok
cargo test              → 156 green: 78 + 53 + 9 + 1 + 15
```

### Still open

- The `video_max_height` media-quality limit (F6's last, „3–4 week" item); the OAuth hookup; the human
  release items (SignPath, Store).

## 2026-11-23 (127th slice) — the hard band hibernates the live card (F6)

Slices 124–126 built the `pressure` signal and made it visible on the grid; now the **action** came: the
„hard = aggressive action" (ADR-020). If the live card's measured working set reaches the hard band, the shell hibernates it via the same
path as the idle timer — the same guards (pinned, popup, media), only the trigger differs.

### What changed

- **`main_window.{h,cpp}` — `checkMemoryPressure()` + `memory_check_`:** a repeating sample timer that, on a live card, measures the
  working set with `metrics`, and on reaching `memory_hard_bytes_` calls `onIdleTimeout` (the
  `freezeCard` path is shared). The factory 15 s is negligible; `ANTIKOLT_MEMORY_CHECK_MS` shortens it for the test. The timer stops
  when the card detaches.
- **`--memoryhibernatetest` (new):** a 1 MiB hard band + 1 s sample timer + long idle window → the freshly opened
  card (~100 MiB) goes to disk from the governor, not from idleness.

### Evidence (measured)

```
scripts/verify.ps1     → ALL SELF TESTS PASSED (58 checks all „ok", including memoryhibernatetest and check-repo)
--memoryhibernatetest  → home_loaded=file:///…/home/index.html · card_click=ok · card_surface=2 ·
                         card_closed=2 · memory_hibernate=ok
cargo test              → 156 green: 78 + 53 + 9 + 1 + 15
```

### Still open

- The `warn` level's effect (pausing the background preview) and the `video_max_height` media-quality limit; the OAuth
  hookup; the human release items (SignPath, Store).

## 2026-11-23 (126th slice) — the grid shows the memory pressure, and the bridge is found (F6)

Slice 125's `pressure` signal has now reached **the page** — and along the way it turned out that until now it had **not reached it**: the bridge's
`system.memory` mapping only forwarded `{usedBytes, budgetBytes}`, and dropped the `overBudget` and the `pressure`.
The grid's `refreshSystem` read them, but always got `undefined`, so the „memory-saving"
indicator (`mem-over`) in fact never fired. The bridge fix closed this, and the grid now shows the `warn`/`hard` class
too.

### What changed

- **`home/app.js` — the bridge:** `system.memory` now forwards the `overBudget` and the `pressure` (the bug that
  silently cut the indicators).
- **`home/app.js` — the grid:** `refreshSystem` puts a `mem-warn`/`mem-hard` class on the
  memory number based on the `pressure` (`normal` clears both; these persist, the click only hides the `mem-over` tip), and the
  tooltip states the pressure. New witness: `window.antikoltMemoryState()`.
- **`home/app.css`:** `mem-warn` (accent) and `mem-hard` (`--danger`) styles.
- **`--memorypressuree2etest`** (from slice 125) extended: besides the response's `pressure=hard`, it also proves the grid's
  `mem-hard` class — the full chain: working set → band → `system.memory` → bridge → grid.

### Evidence (measured)

```
scripts/verify.ps1      → ALL SELF TESTS PASSED (57 checks all „ok", including memorypressuree2etest and check-repo)
--memorypressuree2etest → pressure_e2e_used=106950656 · pressure_e2e=hard · pressure_e2e_page_hard=true ·
                          memory_pressure_e2e=ok
node --check home/app.js → ok (part of check-repo)
cargo test               → 156 green: 78 + 53 + 9 + 1 + 15
```

### Still open

- Binding the pressure **to action** (the `warn`/`hard` level's effect: preview-worker pause, card hibernation, the
  `video_max_height` media-quality limit); the OAuth hookup; the human release items (SignPath, Store).

## 2026-11-23 (125th slice) — the memory-pressure signal proven end to end (F6)

Slice 124's `pressure` signal was only proven at the **formula** level (`--memorypressuretest`, pure function).
The end-to-end proof was missing: that the **measured** working set, measured against the **real** bands, in the
`system.memory` response, yields the correct class. This now exists — via test override of the bands, with the same pattern
as `ANTIKOLT_HIBERNATE_MS`.

### What changed

- **Test override:** `ANTIKOLT_MEMORY_WARN_BYTES`/`ANTIKOLT_MEMORY_HARD_BYTES` (a positive value overrides the profile's
  bands in `refreshUserPrefs`), so a self test can push the measured working set across the boundary.
- **`--memorypressuree2etest` (new):** with 0.5 MiB / 1 MiB bands the ~110 MiB live working set crosses both bands,
  so the response must be `pressure=hard` — the measurement feeds the signal, not the formula.

### Evidence (measured)

```
scripts/verify.ps1     → ALL SELF TESTS PASSED (57 checks all „ok", including memorypressuree2etest and check-repo)
--memorypressuree2etest → home_surface=1 · pressure_e2e_used=109428736 · pressure_e2e=hard · memory_pressure_e2e=ok
--memorypressuretest    → pressure_normal=normal · pressure_warn_at=warn · … · memory_pressure_check=ok
--memoryreport          → memory_pressure=normal · memory_report=ok
cargo test              → 156 green: 78 + 53 + 9 + 1 + 15
```

### Still open

- Binding the pressure **to action** (the `warn`/`hard` level's effect: preview-worker pause, card hibernation, the
  `video_max_height` media-quality limit) and the page-side indicator; the OAuth hookup; the human release items.

## 2026-11-23 (124th slice) — the memory warn/hard bands as a live signal (F6)

The first piece of F6's „real controller for the warn/hard bands": the profile's `memory_warn_bytes`/`memory_hard_bytes` bands
until now only **appeared** (`--memoryreport`), but drove nothing. Now the measured working set is compared to the bands,
and the result — the `normal`/`warn`/`hard` **pressure class** — travels in the page's `system.memory` response, exactly as
the core's `ProfileParams::pressure` defines it (boundary-inclusive: `>= warn` → `warn`, `>= hard` → `hard`).

### What changed

- **`memoryPressureName(used, warn, hard)`** (free function in `main_window.{h,cpp}`): the shell counterpart of the core's `pressure`;
  an unfilled (0) band never fires, so the two bands are independently optional.
- **The shell caches the bands** (`memory_warn_bytes_`/`memory_hard_bytes_`, `refreshUserPrefs`), and the `system.memory`
  response gains a new `pressure` field — so the page sees the system pressure alongside `overBudget` (the basis for future
  governor actions).
- **`--memorypressuretest` (new, pure function test):** all three classes at and above the boundary, plus the
  „no band" and „only one band" cases.
- **`--memoryreport`** also writes `memory_pressure=`, and the report is green only if the field arrived (proof of the wiring,
  not just the formula).

### Evidence (measured)

```
scripts/verify.ps1 → ALL SELF TESTS PASSED (56 checks all „ok", including memorypressuretest and check-repo)
--memorypressuretest → pressure_normal=normal · pressure_warn_at=warn · pressure_warn_above=warn ·
                       pressure_hard_at=hard · pressure_hard_above=hard · pressure_no_bands=normal ·
                       pressure_warn_only=warn · pressure_hard_only=hard · memory_pressure_check=ok
--memoryreport      → memory_pressure=normal · memory_report=ok
cargo test          → 156 green: 78 + 53 + 9 + 1 + 15
```

### Still open

- Binding the pressure **to action** (the `warn`/`hard` level's effect — preview-worker pause, card hibernation, and the
  `video_max_height` actual media-quality limit) and the page-side indicator; the OAuth hookup; the human release
  items (SignPath, Store).

## 2026-11-23 (123rd slice) — proving workspace drag-and-drop (ADR-058)

The workspace drag-and-drop open item is closed: the wiring was already in the code (the card is `draggable`, `dragstart`
arms the `application/x-antikolt-card` MIME type, the tab's `dragover` accepts **only** this type, and `drop`
calls `setWorkspace` — the `<select>` remains the keyboard/accessible path, drag is the fast path, exactly as the
ADR-058 „can be added later" branch said), but **there was no test proving it**. `--workspacetest` only measures the
`<select>` path, so the `21-ALLAPOT` „drag-and-drop" row stood without evidence. This now exists.

### What changed

- **`--workspacedragtest` (new, `runWorkspaceDragCheck` in the shell):** synthetic drag/drop (there is no reliable
  user gesture in the harness), on the **real** elements: the card is `draggable`, `dragstart` arms and returns the
  MIME type, the tab's `dragover` accepts the card type, rejects the foreign text-drag, and `drop` moves the
  card to the target tab (the core agrees too), and dropping back restores it — the profile stays as we found it.

### Evidence (measured)

```
scripts/verify.ps1 → ALL SELF TESTS PASSED (55 checks all „ok", including workspacedragtest and check-repo)
--workspacedragtest → workspace_drag_wired=ok draggable=yes mime=01SEED000000000000000003 ·
                      workspace_drag_gate=ok accepted=yes ignored=yes ·
                      workspace_drag_move=ok card=01SEED000000000000000003 target=w-work now=w-work answer=dropped ·
                      workspace_drag_restored=ok answer=dropped · workspace_drag_check=ok
cargo test            → 156 green: 78 + 53 + 9 + 1 + 15
```

### Still open

- The profile-move button, the OAuth hookup; the human release items (SignPath, Store); the remaining F6
  controller (video resolution, `warn`/`hard` bands).

## 2026-11-22 (122nd slice) — proving per-container proxy/permission (F3)

The F3 „per-container permission/proxy" open item is closed: the code was already fully wired (the core stores the
container's proxy and permissions, the shell builds a `SurfacePolicy` from it, the engine applies it — ADR-071), but the
shell's proxy-building logic was not separately tested. I extracted it into a cleanly testable function.

### What changed

- **`surfacePolicyFromEntry(entry)`** (free function in `engine_client.{h,cpp}`): builds a proxy and a
  permission map from the container row — `direct` → `direct`, `fixed` + bare host → `http://host:port`, schemed host → unchanged,
  the `proxy_bypass` and the `permissions` pass through.
- **`--containerpolicytest` (new, pure function test):** all three proxy branches + the bypass + the permission + the empty row.

### Evidence (measured)

```
scripts/verify.ps1 -SkipBuild → ALL SELF TESTS PASSED (55 self tests + check-repo)
--containerpolicytest       → policy_direct=direct · policy_fixed=http://127.0.0.1:1080 ·
                              policy_scheme=socks5://127.0.0.1 · container_policy_check=ok
cargo test                  → 156 green: 78 + 53 + 9 + 1 + 15
```

### Still open

- The workspace drag-and-drop, the profile-move button, the OAuth hookup; the human release items.

## 2026-11-22 (121st slice) — proving per-card session override

The ADR-039 open item is closed: the per-card (origin-level) session override was already in the code
(the panel's dropdown → `session_upsert`, the core resolves via `SessionRules::resolve`), but **there was no test proving**,
that an origin's own `wipe` overrides the container's `sweep` default. This now exists.

### What changed

- **Rust test `a_rows_own_policy_overrides_the_container_default`:** a `wipe` row signs out even under the `sweep` default
  (deletes the cookies), and a `keep` row deletes nothing even under the `wipe` default (no plan).

### Evidence (measured)

```
scripts/verify.ps1 -SkipBuild → ALL SELF TESTS PASSED (54 self tests + check-repo)
cargo test                  → 156 green: 78 + 53 + 9 + 1 + 15
```

### Still open

- The per-container permission/proxy (F3), the workspace drag-and-drop, the profile-move button, the OAuth
  hookup; the human release items.

## 2026-11-22 (120th slice) — the raster's aspect-ratio-preserving fit (F6)

The raster no longer stretches: the shell sizes `raster_full_` **proportionally** relative to the window
(`Qt::KeepAspectRatio`) and centres it; the click mapping accounts for the actual drawn rectangle (letterbox)
offset, so the card under the cursor can be hit precisely even on the undistorted image.

### What changed

- **`updateRasterPixmap()`:** sizes the full-resolution raster to the label proportionally, and saves the drawn size
  into `raster_drawn_size_`; the `eventFilter` refits on the `Resize` event.
- **The click hook:** maps the label click first into the drawn image with the letterbox offset, then into CSS space;
  a click falling into the letterbox does not open a card, only wakes it.

### Evidence (measured)

```
scripts/verify.ps1 -SkipBuild → ALL SELF TESTS PASSED (54 self tests + check-repo)
--rasterhometest / --rasterclicktest → green
cargo test                  → 155 green: 78 + 52 + 9 + 1 + 15
```

### Still open

- A sharp, in-window visual check of the raster; the actual controller for the profile's other dimensions (video resolution,
  `warn`/`hard`); the human release items.

## 2026-11-22 (119th slice) — showing the raster + the click hook (F6)

The last, visual piece of the raster home page: when the engine stops, the grid's raster image is **visible** in the card area,
and clicking the raster wakes the engine, and even opens the card under the cursor directly.

### What changed

- **The card area became a stack:** `QStackedLayout` page 0 is the live card, page 1 is `raster_label_` (the raster);
  `enterRasterMode` loads `home-raster.png` and switches, `wakeFromRaster` switches back.
- **The click hook:** the `eventFilter` maps the `raster_label_` click to the grid's CSS coordinate, measures it
  against the `raster_cards_` rectangles, and puts the found card (url + title + id) into the `pending_*` fields before the
  engine restarts — so the `engineReady` greeting opens the card right away, not the grid.
- **`captureCardPositions`:** now also carries the viewport's CSS size (`vw`/`vh`) and the cards' url + title.

### Evidence (measured)

```
scripts/verify.ps1 -SkipBuild → ALL SELF TESTS PASSED (54 self tests + check-repo)
--rasterhometest / --rasterautotest / --rasterclicktest → all green
cargo test                  → 155 green: 78 + 52 + 9 + 1 + 15
```

### Still open

- The raster's **visual check** in a live window (the QLabel fits stretched, the click mapping is linear);
  the actual controller for the profile's other dimensions (video resolution, `warn`/`hard`); the human release items.

## 2026-11-22 (118th slice) — sandboxing the user home page (item 3)

The security lock on the editable home page: the user's `home.html` loads from the profile directory, so it
**cannot reach** the profile's data files (`antikolt.sqlite3`, card data) directly. A test now records this:
Chromium's `file://` origin is opaque, so both `fetch` and `XHR` are rejected with CORS.

### What changed

- **`--sandboxtest` (new):** a `secret.txt` and a malicious `home.html` go into the profile, the latter trying to read with `fetch` and
  `XHR`; the test proves both are blocked (`blocked by CORS policy`), so the
  sibling file does not leak.

### Evidence (measured)

```
scripts/verify.ps1 -SkipBuild → ALL SELF TESTS PASSED (54 self tests + check-repo)
--sandboxtest               → sandbox_result={"fetchBlocked":true,"xhrBlocked":true} · sandbox=ok
cargo test                  → 155 green: 78 + 52 + 9 + 1 + 15
```

### Still open

- The user's HTML reaches the card operations (intentionally) via the **bridge**; the direct file read
  is closed. The F6 mapping widget hookup (raster click → card open) remains the product's UI.

## 2026-11-22 (117th slice) — the editable home page's starting template (item 3)

The editor's first, measurable piece: if the user has no `home.html` yet, the shell writes a **working starting
page** (and copies `antikolt-card.js` next to it), so the user does not start from an empty file, but from a
page that already renders the cards.

### What changed

- **`createCustomHomeTemplate()`:** creates `<coreData>/home.html` (the template defines `window.antikoltAcceptCards`,
  which renders the shell's cards), and copies the built-in `antikolt-card.js` next to it — the earlier
  gap: the page in the profile would not have reached the package.
- **The settings dock's „Edit" button:** creates the template (if absent), then opens it in the editor
  (`QDesktopServices::openUrl`).
- **`--homeedittest` (new):** the template is created, the package is next to it, the page loads `antikolt-card.js` and
  is served.

### Evidence (measured)

```
scripts/verify.ps1 -SkipBuild → ALL SELF TESTS PASSED (53 self tests + check-repo)
--homeedittest              → homeedit_home_exists=true · homeedit_card_copied=true · homeedit_has_script=true ·
                              homeedit_served=true · home_edit=ok
cargo test                  → 155 green: 78 + 52 + 9 + 1 + 15
```

### Still open (item 3)

- **Sandboxing** the user HTML (the safe load) and the editor's further convenience (the in-product
  HTML editor); the F6 mapping widget hookup.

## 2026-11-22 (116th slice) — the raster's click mapping (F6)

The last piece of the raster home page: before rasterizing, the grid emits the visible cards' rectangles, the shell
saves them, and provides a point→card mapping — so clicking the raster can open the matching card.

### What changed

- **`captureCardPositions()`:** the grid queries the id and `getBoundingClientRect` of every visible `<antikolt-card>` element,
  and stores them (`raster_cards_`); `enterRasterMode` calls it before the raster.
- **`cardAtRasterPoint(x, y)`:** measures the point against the rectangles (point in rectangle), empty if nowhere.
- **`--rasterclicktest` (new):** the grid loads → the positions are mapped → the number of visible cards > 0, the distant point
  maps to empty.

### Evidence (measured)

```
scripts/verify.ps1 -SkipBuild → ALL SELF TESTS PASSED (52 self tests + check-repo)
--rasterclicktest           → rasterclick_cards=9 · rasterclick_far=empty · raster_click=ok
cargo test                  → 155 green: 78 + 52 + 9 + 1 + 15
```

### What remains open (F6)

- Wiring the mapping **to the raster widget's click** (the QLabel click → `cardAtRasterPoint` → wake +
  card opening) — the logic is ready, the widget hook is the product's UI; item 3's editor and sandboxing.

## 2026-11-22 (one hundred fifteenth slice) — automatic activation of the raster home page (F6)

The raster home page now switches on **by itself**: when a card is written to disk and, per the profile's T2, the
engine is idle, an idle timer calls `enterRasterMode` after the threshold (raster + shutdown). This is the "idle 120–180 MB"
promise's last mechanical piece.

### What changed

- **`armEngineIdleTimer()`:** a one-shot QTimer using the profile's `engine_warm_idle_shutdown_ms`; `-1` (warm,
  T1) does not start a timer.
- **Starts when the card is written to disk:** `surfaceDetached` calls it (not under `raster_mode`, so that the
  shutdown does not re-arm itself).
- **`--rasterautotest` (new):** "ultra" (0 ms) profile + winding up the timer → the engine stops, raster mode switches on;
  at the end of the test the factory "balanced" is restored (otherwise the "ultra" would stop the next
  run's preview worker — the `previewworkertest` failure signalled this, fixed).

### Evidence (measured)

```
scripts/verify.ps1 -SkipBuild → ALL SELF TESTS PASSED (51 self tests + check-repo)
--rasterautotest            → rasterauto_stopped=true · raster_auto=ok
--previewworkertest         → preview_worker=ok (after the profile restore)
cargo test                  → 155 green: 78 + 52 + 9 + 1 + 15
```

### What remains open (F6)

- The **click mapping** (raster click → opening the card) — the last UX piece; item 3's
  editor and sandboxing.

## 2026-11-22 (one hundred fourteenth slice) — raster home page mode (F6)

The raster home page now works as a single mode switch: the shell rasterizes the grid, **stops the engine**, and
remembers the mode; the wake restarts the engine, and the grid reloads. Stopping the engine is the big
memory promise (toward idle 120–180 MB), and the raster is what the user sees until then.

### What changed

- **`enterRasterMode()`:** puts the grid into the profile's `home-raster.png` via `capture`, then stops the engine; it
  also clears the old surface id (`card_->detach()`), otherwise the restart would navigate the dead surface.
- **`wakeFromRaster()`:** restarts the engine; the `engineReady` greeting reloads the grid.
- **`--rasterhometest` (new):** the full cycle — grid load → mode on (shutdown + raster) → wake
  (restart + grid returns).

### Evidence (measured)

```
scripts/verify.ps1 -SkipBuild → ALL SELF TESTS PASSED (50 self tests + check-repo)
--rasterhometest            → rasterhome_stopped=true · rasterhome_mode=true · rasterhome_restarted=true ·
                              rasterhome_grid_returned=true · raster_home=ok
cargo test                  → 155 green: 78 + 52 + 9 + 1 + 15
```

### What remains open (F6)

- The raster home page's **automatic** activation per the profile's T2 (stopping the idle engine) and the
  **click mapping** (raster click → opening the card); item 3's editor and sandboxing.

## 2026-11-22 (one hundred thirteenth slice) — rasterizing the grid (F6)

The raster home page's next piece: the shell can render the home page surface into a single image file
(`<profile>/home-raster.png`). This is the raster that the shell will show when the engine has stopped — the
shutdown/restart (112th) already works, the raster is now ready.

### What changed

- **`captureGrid()`:** puts the home page surface into a PNG via `capture`, and copies it from `screenshots/` into the profile's
  `home-raster.png` (a stable name that the raster mode reads).
- **`--rastertest` (new):** grid load → `captureGrid()` → the `home-raster.png` exists, a valid PNG.

### Evidence (measured)

```
scripts/verify.ps1 -SkipBuild → ALL SELF TESTS PASSED (49 self tests + check-repo)
--rastertest                → raster_grid_loaded=true · raster_bytes=242038 · raster_png=true · raster=ok
cargo test                  → 155 green: 78 + 52 + 9 + 1 + 15
```

### What remains open (F6)

- The **raster home page**'s last piece: the raster's **display** with the engine stopped, stopping the idle engine per the
  profile's T2, and mapping the click to the card (cold restart); item 3's editor and
  sandboxing.

## 2026-11-22 (one hundred twelfth slice) — making the engine restartable (F6, raster prerequisite)

The raster home page (engine stopped) requires that the engine **can shut down and start back up**.
It turned out that `EngineClient` did not support this until now: `start()` returned early, because after `shutdown()`/exit
the `process_` pointer was not nulled. I fixed this, and verified the full cycle with a self test.

### What changed

- **`EngineClient::onProcessFinished`:** nulls (and deletes) the exited engine's `process_`, clears the protocol
  buffer and the engine info — so `start()` launches a fresh process.
- **`--enginerestarttest` (new):** start → greeting → shutdown → restart → greeting, measuring every state.

### Evidence (measured)

```
scripts/verify.ps1 -SkipBuild → ALL SELF TESTS PASSED (48 self tests + check-repo)
--enginerestarttest          → restart_first_start=true · restart_running_after_shutdown=false ·
                               restart_running_after_restart=true · restart_welcome_second=true · engine_restart=ok
cargo test                   → 155 green: 78 + 52 + 9 + 1 + 15
```

### What remains open (F6)

- The **raster home page** itself: rasterizing the grid, stopping the engine when idle, displaying the raster
  and cold restart on click — shutdown/restart now works, raster display is the next
  step; item 3's editor and sandboxing.

## 2026-11-22 (one hundred eleventh slice) — resetting the custom home page (item 3)

The editable home page now closes into a usable loop: the user's configured `home.html` can be reset
to the built-in grid with a single button. A broken custom page can never be a dead end.

### What changed

- **`resetCustomHome()`:** deletes the core data directory's `home.html`, and reloads the grid — `homePageUrl()`
  now returns the built-in `home/index.html`.
- **The settings dock button:** "Reset custom home page" (`resetCustomHome` objectName).
- **`--resethometest` (new self test):** loads a scratched `home.html` → `resetCustomHome()` → the file disappears, the
  built-in grid returns (`reset_grid_returned=true`).

### Evidence (measured)

```
scripts/verify.ps1 -SkipBuild → ALL SELF TESTS PASSED (48 self tests + check-repo)
--resethometest             → reset_custom_served=true · reset_grid_returned=true · reset_file_gone=true ·
                              reset_home=ok
cargo test                  → 155 green: 78 + 52 + 9 + 1 + 15
```

### What remains open (item 3)

- The **editor UI** (writing the HTML from the product) and the **sandboxing** of the user HTML; the F6 raster home page.

## 2026-11-22 (one hundred tenth slice) — wiring in the engine-lifetime dimension (F6)

The raster home page (engine stopped) requires the engine-lifetime (T1/T2) dimension: the shell now caches
the profile's `engine_warm_idle_shutdown_ms`, and the tuning report plus `--prefstest` verify that the "ultra" profile
requests a 0 ms (immediate) shutdown. The actual shutdown/restart is the next big step.

### What changed

- **`engine_shutdown_ms_` in the shell:** `refreshUserPrefs()` caches the
  `engine_warm_idle_shutdown_ms` from the derived profile; `None` (warm, T1) → `-1`, `Some(0)` (immediate, T2) → `0`.
- **The report shows it:** `--memoryreport` → `memory_engine_shutdown_ms`.
- **`--prefstest` verifies:** the "ultra" profile's `pref_derived_engine_shutdown=0` (regardless of the sent `manual`/`balanced`
  value, from the derivation).

### Evidence (measured)

```
scripts/verify.ps1 -SkipBuild → ALL SELF TESTS PASSED (46 self tests + check-repo)
--prefstest                 → pref_derived_engine_shutdown=0 · pref_derived_idle=120000 · prefs=ok
--memoryreport              → memory_engine_shutdown_ms=0 · memory_report=ok
cargo test                  → 155 green: 78 + 52 + 9 + 1 + 15
```

### What remains open (F6)

- The **raster home page** itself (actually stopping the engine when idle + raster display + cold
  restart) — the big, 3–4 week item; the actual controller action of the `warn`/`hard` bands and the video resolution;
  item 3's editor.

## 2026-11-22 (one hundred ninth slice) — the tuning report (F6)

F6's "tuning report" item comes together as a single diagnostic: `--memoryreport` reads the whole memory controller's
state at once — the profile, the budget, the live surface's working set, and the per-card memory —, and
writes it out in a single table. This is what you need to see for tuning.

### What changed

- **`--memoryreport` (new self test/diagnostic):** the shell reads, one after another, the core's `user_prefs` (profile + the
  derived parameters + budget), the `cards_list` (the per-card `memory_bytes` sum) and the page's
  `system.memory` (working set + `overBudget`), then writes the `memory_profile`, `memory_idle_ms`,
  `memory_preview_refresh`, `memory_video_max_height`, `memory_warn/hard_bytes`, `memory_budget_bytes`,
  `memory_working_set_bytes`, `memory_over_budget`, `memory_card_sum_bytes` and `memory_cards_measured` fields.
- **Honest about the `manual` profile:** `manual` has no derived parameters, so the parameters are 0 — the report shows
  the profile from `prefs.resource_profile`, and the 0 parameter is not an error.

### Evidence (measured)

```
scripts/verify.ps1 -SkipBuild → ALL SELF TESTS PASSED (46 self tests + check-repo)
--memoryreport              → memory_profile=manual · memory_budget_bytes=67108864 ·
                              memory_working_set_bytes=106741760 · memory_over_budget=true ·
                              memory_card_sum_bytes=117477376 · memory_report=ok
cargo test                  → 155 green: 78 + 52 + 9 + 1 + 15
```

### What remains open

- The **raster home page** (engine stopped) and wiring in the T1/T2 engine lifetime — the big, 3–4 week F6 remainder; the
  actual controller action of the profile's `warn`/`hard` bands and the video resolution (the signals are already out);
  item 3's editor.

## 2026-11-22 (one hundred eighth slice) — wiring in the profile's `preview_refresh` (F6)

The profile now derives not only hibernation but also the **preview refresh**: the core exposes the derived
profile parameters, and the shell aligns the background preview worker to them. This is the key to the profile's other
dimensions too (engine lifetime, video resolution, `warn`/`hard` bands) — everything travels in a single reply.

### What changed

- **`user_prefs` → `profile`:** alongside `prefs`, the core's reply also sends the derived `ProfileParams`
  (`idle_hibernate_ms`, `preview_refresh`, `video_max_height`, `memory_warn/hard_bytes`, `engine_warm_idle_shutdown_ms`);
  `null` for the `Manual` profile (no derived parameters).
- **The shell caches `preview_refresh`:** `refreshUserPrefs()` reads it from the reply's `profile`; the factory `Balanced`
  is true, `Saver`/`Ultra` are false, `Manual` (null) leaves the factory value.
- **The background preview worker respects it:** with `preview_refresh=false` the worker does not start (as a fifth,
  alongside ADR-041's four conditions).
- **New Rust test:** `user_prefs_carry_the_derived_profile_params` — the `saver` profile carries `preview_refresh=false`,
  `idle=300000`, `video_max_height=720`, and `manual` a `null` profile.

### Evidence (measured)

```
scripts/verify.ps1 -SkipBuild → 44 self tests green (the only error was cargo fmt, fixed)
scripts/check-repo.ps1       → ALL CHECKS PASSED
cargo test                   → 155 green: 78 + 52 + 9 + 1 + 15
--prefstest                  → pref_derived_idle=120000 · prefs=ok
```

### What remains open (F6)

- **Wiring in the profile's other dimensions** (engine lifetime T1/T2, video resolution, the `warn`/`hard` bands to
  the controller — the parameters are already at the shell), the **raster home page** and the **tuning report**; item 3's
  editor.

## 2026-11-22 (one hundred seventh slice) — reconciling the profile with the ADR-020 model

In the 104th slice `resource_profile` became a standalone string field with its own `profile_preset` — but the core already
contained ADR-020's **full** profile model (`antikolt-core::profile::ProfileKind`/`ProfileParams`), which I had not noticed
back then. This slice eliminates the parallel model: the profile is now the ADR-020 enum, with one source.

### What changed

- **`resource_profile` → `ProfileKind` (ADR-020):** instead of a `String`, the core's `ProfileKind` enum (`Fast`/`Balanced`/
  `Saver`/`Ultra`/`Manual`, snake_case on the wire and in SQL); `serde(default)` gives the factory `Balanced`.
- **`profile_preset` removed:** the derivation comes from `ProfileKind::params()` (15/10/5/2 minutes of hibernation, plus
  engine lifetime, preview refresh, video resolution and bands). I **dropped the budget derivation** — ADR-020's
  profile does not set `memory_budget_mb` (that is a separate user setting), but the controller's `warn`/`hard`
  bands.
- **Schema `0010`:** rewrites the Hungarian values that entered in 0009 (`gyors`/`kiegyensulyozott`/`takarekos`/`kezi`) to the
  ADR-020 snake_case names; an unknown value in `profile_kind_from_str` falls back to `Balanced`.
- **`--prefstest` with the right assertion:** the "ultra" profile derives hibernation (`derived_idle=120000`), the budget
  stays separate (`pref_budget_bytes=536870912` from the sent 512).

### Evidence (measured)

```
scripts/verify.ps1 -SkipBuild → ALL SELF TESTS PASSED (44 self tests + check-repo)
--prefstest                 → pref_derived_idle=120000 · pref_budget_bytes=536870912 · prefs=ok
--addcardtest               → db_status=ok schema=10 … verified=yes
cargo test                  → 154 green: 78 + 52 + 8 + 1 + 15
```

### What remains open

- **Wiring in the profile's other dimensions** (engine lifetime T1/T2, preview refresh, video resolution, the
  `warn`/`hard` bands to the controller), the **raster home page** and the **tuning report**; item 3's editor.

## 2026-11-22 (one hundred sixth slice) — serving the user-authored home page (the core of item 3)

Of the "full editable home page" item, the **storage + serving** is done: the shell now serves a
`home.html` in a profile instead of the built-in grid. The editor UI (writing the HTML from the product) is a separate, bigger item —
for now the user puts the file into the profile from outside, and the shell loads it.

### What changed

- **`home.html` in the core data directory overrides the built-in grid:** `MainWindow::homePageUrl()` order:
  `ANTIKOLT_HOME` → `<coreData>/home.html` → the built-in `home/index.html`. The `coreDataDir()` static helper
  (`ANTIKOLT_CORE_DATA` or the platform app-data + `/core`) is the single place where the core data directory is computed —
  `startEngine` also uses it, so the two paths cannot diverge.
- **The profile carries it:** the `home.html` is in the core data directory, so the profile migration (ADR-059) carries it
  just like the cards; the user page can also load `antikolt-card.js` from the same directory.
- **`--customhometest` (new self test):** the shell loads a `home.html` written into a scratched data directory, and the page title
  ("sajat kezdolap") proves that the user's file, not the grid, landed on the surface.

### Evidence (measured)

```
scripts/verify.ps1 -SkipBuild → ALL SELF TESTS PASSED (44 self tests + check-repo)
--customhometest            → custom_home_title=sajat kezdolap · custom_home=ok
cargo test                  → 154 green: 78 + 52 + 8 + 1 + 15
```

### What remains open (item 3)

- The **editor UI** (writing the HTML from the product) and refining the **secure loading** (the user HTML
  is a fully privileged `file://` page, not sandboxed) — as well as the F6 remainder (engine lifetime T1/T2,
  preview refresh, GPU, raster home page, tuning report).

## 2026-11-22 (one hundred fifth slice) — the media-quality limit signal (F6)

The media-quality limit is the part that can honestly be done from the browser: the **signal**, not actually clamping
the video to 720p (the resolution cannot be forced from outside). Exceeding the budget now reaches the page, the
surface shows a "memory-saving" indicator, and the click overrides it.

### What changed

- **`system.memory` → `overBudget`:** the shell adds to the reply whether the live surface's working set exceeds the
  profile's budget (`used > memory_budget_bytes_`); this is the band signal on which the media-quality limit builds (03-MEMORIA.md
  §4).
- **The grid signals and overrides it:** the `#mem` field gets the `mem-over` class when the page goes over the budget (text color +
  accent border); one click hides it until the next page load (`memoryHintDismissed`).
- **`--prefstest` in two phases:** (1) under the "ultra" profile's 256 MiB, `overBudget=false`; (2) over a manual 64 MiB
  budget, `overBudget=true` — the signal reaches the page in both directions.

### Evidence (measured)

```
scripts/verify.ps1 -SkipBuild → ALL SELF TESTS PASSED (42 self tests + check-repo)
--prefstest                 → pref_over_budget_under=false · pref_over_budget_manual=true · prefs=ok
cargo test                  → 154 green: 78 + 52 + 8 + 1 + 15
```

### What remains open (F6)

- The actual **video-resolution limit** (720p) cannot be forced from outside — the signal is what was implemented; the
  profile's **other dimensions** (engine lifetime T1/T2, preview refresh, GPU), the **raster home page** and the
  **tuning report** remain.

## 2026-11-22 (one hundred fourth slice) — the resource profile as a preset (F6)

Of F6's "resource profile" item, the part that builds on the already wired-in settings is done: the profile is a
**preset** that derives the hibernation time and the memory budget. The profile's further dimensions (engine lifetime,
preview refresh, media quality, GPU) are separate, bigger slices.

### What changed

- **`resource_profile` in `user_prefs` (schema `0009`):** `gyors` | `kiegyensulyozott` *(default)* | `takarekos` |
  `ultra` | `kezi`, validated; `serde(default)` for importing pre-0009 bundles; the `ALTER … ADD COLUMN` follows the 0007
  idempotent pattern.
- **`profile_preset(profile) → (idle, budget)` in the core** (one source): fast `(15 min, 1024 MiB)`,
  balanced `(10 min, 512 MiB)`, saver `(5 min, 384 MiB)`, ultra `(2 min, 256 MiB)`; `kezi` and unknown
  fall back to the factory.
- **The preset only overrides when a profile is explicitly sent** (`user_prefs_set`): the field *not* sent by the client
  (the serde default) does not secretly switch presets — the custom values remain valid even if the profile is not given.
- **The settings dock's profile drop-down:** the four profiles + "Manual" in a `QComboBox`; with a named profile, the core
  overrides the hibernation and the budget, "Manual" leaves the ones below.
- **`--prefstest` extended:** the client sends `resource_profile="ultra"` with a `512` MiB budget — the preset
  derives it to `256`, and the page's `budgetBytes` carries this.

### Evidence (measured)

```
scripts/verify.ps1 -SkipBuild → ALL SELF TESTS PASSED (42 self tests + check-repo)
--prefstest                 → pref_budget_bytes=268435456 (the "ultra"'s 256 MiB) · prefs=ok
cargo test                  → 154 green: 78 + 52 + 8 + 1 + 15
--addcardtest               → db_status=ok schema=9 … verified=yes
```

### What remains open (F6)

- The profile's **other dimensions**: engine lifetime (T1 warm / T2 cold), preview refresh on/off, **media-quality
  limit** (720p), GPU selection — and the **raster home page** (engine stopped) plus the **tuning report**.

## 2026-11-22 (one hundred third slice) — the per-card memory breakdown (F6)

F6's "per-card breakdown" item was missing until now: `system.memory` measured the live surface, and the card did not
know how much it cost while it was still alive. This slice measures the working set at the freeze boundary and writes it onto the card.

### What changed

- **`memory_bytes` on the card (schema `0008`):** `Card` gained an `i64` field (`serde(default)`, `0` = "not yet
  measured"), the `ALTER … ADD COLUMN` follows the 0007 idempotent pattern; `upsert()` preserves the stored value if the page
  sends the record with 0 ("I didn't measure", not "delete it"), and on a URL change drops it together with the text — both
  belong to the old page's working set.
- **`card_memory` IPC op + `CoreClient::cardMemory`:** a one-column write, so the measurement does not race an
  in-progress typed note (following the `card_page_text` pattern, ADR-049).
- **The shell measures at freeze time:** `freezeCard()` fetches the surface's `engine_working_set_bytes` before the snapshot
  and writes it into the core — freezing is the natural boundary (a live card changes even while being measured), and continuous measurement
  would itself eat memory.
- **The grid shows it:** the card's `meta` row gets an `≈N MB` indicator (`card-memory`), and `pageCardFromCore` carries
  the `memoryBytes` field over.
- **`--memorytest` (new self test):** open card → freeze → in the core, the card's `memory_bytes > 0`.

### Evidence (measured)

```
scripts/verify.ps1 -SkipBuild → ALL SELF TESTS PASSED (42 self tests + check-repo)
--memorytest                 → memory_bytes=117633024 (~112 MiB) · memory=ok
cargo test                   → 153 green: 78 + 51 + 8 + 1 + 15
--addcardtest                → db_status=ok schema=8 … verified=yes
```

### What remains open (F6)

- **Resource profiles** (Fast/Balanced/Saver/Ultra), **media-quality limit** (720p in the orange band),
  **raster home page** (engine stopped) and the **tuning report** — the roadmap's 3–4 week items.

## 2026-11-22 (one hundred second slice) — the settings UI controller + the idle timer from the profile

The story of ADR-072 (UserPrefs) and ADR-074 (memory budget) ended at the IPC until now: the four settings
(`max_live_cards`, `idle_hibernate_ms`, `cold_card_ms`, `memory_budget_mb`) lived in the core, but there was no native
control in the UI, and the shell's hibernation timer used the hard 10 minutes. This slice eliminates both.

### What changed

- **Settings dock (`main_window.{h,cpp}`):** `showSettings()` builds a
  `QDockWidget` with four `QSpinBox`es following the Sessions panel's pattern (live cards 1..12, hibernation 1..60 minutes, cold card 1..720 hours,
  budget 64..4096 MiB) and a Save button; `saveUserPrefs()` writes with the core's `user_prefs_set` (the core validates),
  then `refreshUserPrefs()` re-reads what the core accepted. It opens via the toolbar "⚙ Settings" button.
- **The idle timer from the profile:** `refreshUserPrefs()` now also caches `idle_hibernate_ms`
  (`idle_hibernate_ms_`) and sets the `idle_` interval; the `ANTIKOLT_HIBERNATE_MS` override (for speeding up the self test)
  still takes precedence, the hard 10 minutes is only the factory value before the core's reply.
- **`--prefstest` (new self test):** the shell asks the core for a 256 MiB budget, re-reads it, then the page's
  `system.memory` reply carries exactly this (`budgetBytes = 268435456`) — the budget write reaches the page, not just the
  core.

### Evidence (measured)

```
scripts/verify.ps1 -SkipBuild → ALL SELF TESTS PASSED (41 self tests + check-repo)
--prefstest                 → pref_set_error= · pref_budget_bytes=268435456 · prefs=ok
cargo test                  → 152 green: 78 + 50 + 8 + 1 + 15
```

### What remains open

- The settings dock is **not clickable** in the harness (`not-measurable`), like the file dialog — the path underneath is
  measured by `--prefstest` and the Rust tests.
- The F6 remainder (per-card memory breakdown, resource profiles, raster home page) and the full editable
  home page (storing/serving the user HTML) remain the roadmap's 3–4 week items.

## 2026-11-22 (one hundred first slice) — the `<antikolt-card>` standalone package + the memory budget (F6)

Of the two remaining big roadmap items, the part that is measurable in one slice is done: the card component
became a standalone package (ADR-075), and the memory budget became a real profile setting, with a band and a self test
(ADR-074). The full editable home page (storing and serving the user HTML) and the rest of F6
(per-card breakdown, resource profiles, raster home page) remain open.

### What changed

- **`<antikolt-card>` as a separate package (ADR-075):** the component + its helpers (`hashHue`, `STATUS`, `cardStyles`,
  `AntikoltCard`, the define guard) moved from `app.js` into the `home/antikolt-card.js` IIFE; the grid and a
  user page load **the same thing** (`index.html` in the `defer` order, before `app.js`). The container/workspace
  `<select>` reads from `setOptions({containers, workspaces})`, the focus state with `getPendingFocusId()`;
  `home/user-home.example.html` is a minimal own-home-page example.
- **Memory budget (ADR-074):** `memory_budget_mb` in `UserPrefs` (schema **`0007`**, default `512`, validation
  `64..=4096`, `serde(default)` for importing pre-F6 bundles; the `ALTER … ADD COLUMN` only when the column is missing).
  The shell caches it with `CoreClient::userPrefs` + `refreshUserPrefs()`, `system.memory` gives it as `budgetBytes`; the
  grid's `#mem` field draws a used/budget bar (`--mem-fill`). `--metricstest` now also asserts
  `page_budget_bytes > 0`.

### Evidence (measured)

```
cargo test                   → 152 tests green: 78 + 50 + 8 + 1 + 15 (the schema was numbered to 0007)
cargo clippy -D warnings     → 0
cargo fmt --all -- --check   → clean
scripts/verify.ps1 -SkipBuild → ALL SELF TESTS PASSED (all self tests + check-repo)
--metricstest                → page_budget_bytes=536870912 (512 MiB), metrics=ok
--addcardtest                → db_status=ok schema=7 cards=… verified=yes
```

### What remains open

- The **full** editable home page (storing the user HTML, serving it, and an editor UI) and
  the rest of F6: **per-card memory breakdown**, **resource profiles**, **raster home page** (engine stopped)
  — the roadmap's 3–4 week items.
- The budget's **settings UI** (the `user_prefs_set` native control) — for now the value can be set via the IPC and the
  profile migration.

## 2026-11-22 (one hundredth slice) — the pre-release remainder: popup, profile button, card policy, drag-and-drop

The items that remained after the handover and are solvable from code, in one round: wiring up the login popup (ADR-073), the
profile bundle's UI button, the per-card session override, drag-and-drop to workspaces, and the
fix of the `page_text` staleness after a web-address change.

### What changed

- **OAuth/SSO popup (ADR-073):** the engine gained `OnBeforePopup` (`apps/engine-host/src/host.{h,cpp}`):
  on a user gesture the popup opens as a real window in the opener's partition (shared cookies), without a gesture
  it is blocked; the `popup_requested`/`popup_closed` events carry the decision. The engine tracks the popup's browser separately
  (`HostState::popup_browser`), so closing it does not take the card down; the 60 s lifetime lives in the engine
  (`kPopupTimeoutMs`, steady-clock). The shell (`engine_client.*`, `main_window.*`) suspends
  hibernation with the `popup_open` flag for the popup's duration.
- **Profile bundle button:** the toolbar's "💾 Profile" button (`main_window.cpp` `buildToolBar`,
  `exportProfile`/`importProfile`) calls the already-ready `profile_export`/`profile_import`
  path through a native `QFileDialog`; after loading, the grid re-renders. The dialog is not clickable in the harness, so the button is
  `not-measurable` — the path underneath is measured by `--profiletest`.
- **Per-card session override:** the Sessions panel's "Policy" column became a drop-down (keep/sweep/wipe),
  which writes the row's `policy` with the core's `session_upsert` (`SessionRules::resolve` already used this); the
  `sessionPanelSummary` also reads the drop-down.
- **Drag-and-drop:** the card is draggable (`<antikolt-card>` `draggable` + `dragstart`, skipping the card's own controls),
  the workspace tab accepts it (`dragover`/`drop` → `setWorkspace`), marking with an accent border. The `<select>`
  remains the keyboard-accessible path (ADR-058).
- **`page_text` on web-address change:** `CardStore::upsert` (`store.rs`) deletes the captured page text if the
  `url` changed (the old capture belonged to the old page). The delete confirmation (ADR-068) was already done earlier — the
  `21-ALLAPOT` open list listed this wrongly, now fixed.

### Evidence (measured)

```
cargo test                   → 152 tests green: 78 + 50 + 8 + 1 + 15 (over the 99th slice's 151; the new store.rs
                               test measures the web-address-change page_text deletion)
cargo clippy -D warnings     → 0
cargo fmt --all -- --check   → clean
scripts/verify.ps1 -SkipBuild → ALL SELF TESTS PASSED (all self tests + check-repo)
--sessionpanel               → session_panel=ok (the "Policy" column with the drop-down too)
--addcardtest                → edit_card_url=ok, db_status=ok schema=6 cards=11 verified=yes
```

### What remains open

- The popup's **gesture path is not measured in the harness** (`not-measurable`): the synthetic `window.open` is not a user
  gesture, so there is no self test on the `popup_requested`/`popup_closed` path — the decision is recorded in the code and in
  `02` §8.5.
- **Editable home page + `<antikolt-card>`** (the component exists, the user-authored home page is a bigger item)
  and the **memory budget (F6)** — the roadmap's 3–4 week items, not this slice's work.
- The **vault/passkey** (F4) is `cargo vendor` without network; the **hardware GPU path** (VECTOR) is driver-level; the
  **signing** (SignPath) is a human step.

## 2026-11-22 (ninety-ninth slice) — user preferences (`UserPrefs`, ADR-072)

The 97th slice made all four stores SQLite-first (schema 0004). This slice adds the user-level settings: `max_live_cards`, `idle_hibernate_ms`, `cold_card_ms` — a single row in the `user_prefs` table, schema **0006** (`PRAGMA user_version = 6`), with a default and validation.

### What changed

- `sqlite_store.rs`: schema **`0006`**. New `user_prefs` table with `CHECK (id = 1)` (a single row). `UserPrefs` struct (`max_live_cards: u32 = 5`, `idle_hibernate_ms: u64 = 600_000` — ten minutes, `cold_card_ms: u64 = 86_400_000` — one day), `Default`, `defaults()`, `validate()` (range + `cold_card_ms <= idle_hibernate_ms` rejected: cold is the later state, the cold window must be longer than the idle). `user_prefs()`/`set_user_prefs()` methods; the `0006` migration writes the seed row (5 / 600000 / 86400000) with `ON CONFLICT (id) DO NOTHING`. Unit tests added.
- `sessions.rs`: `user_prefs()` delegates to the DB; `set_user_prefs()` validates (`prefs.validate().map_err(SessionError::Rules)?`), then writes.
- `bundle.rs`: `pub prefs: Option<UserPrefs>` (`#[serde(default)]`); `Bundle::new(...)` takes over the `prefs: UserPrefs` parameter.
- `main.rs`: `user_prefs` (read) and `user_prefs_set` (write) IPC ops; `profile_export` reads `sessions.user_prefs()` → `Bundle::new`; `profile_import` calls `sessions.set_user_prefs(prefs)` if `read.prefs` is `Some`.
- Test updates: `service.rs` `db_status` schema 5 → 6; two new `user_prefs` tests in `service.rs` (default + round-trip, and it survives a restart); `store.rs` schema digest 5 → 6.

### Evidence (measured)

```
cargo test                  → 151 tests green: 78 + 49 + 8 + 1 + 15 (over the 98th slice's container migration; the schema was numbered to 0006, because the 98th slice took 0005)
cargo clippy -D warnings    → 0 warnings
cargo fmt --all -- --check  → clean
```

### What remains open

There is still no native control on the settings page in the UI: for now the values are only carried by the IPC layer and the profile migration, and the shell's timer does not yet use the `user_prefs` values.

## 2026-11-21 (ninety-eighth slice) — the container isolation boundary: profile directory, proxy, permissions (ADR-071)

The biggest silently remaining gap so far: the engine **never fulfilled** ADR-009's "one partition per card" promise —
every surface was created with a `nullptr` `CefRequestContext`, so every card used a single cookie jar,
and the `partition` key was decoration. This slice eliminates this and makes the container a real boundary:
its own profile, its own proxy, its own permission policy.

### What changed

- `apps/engine-host/src/host.cpp`: `RequestContextForPartition` — one `CefRequestContext` per partition, the CEF profile
  in a **single** directory under the root (`<user data>\persist_card_<hash>`); the `partitions\` subdirectory
  gave a "Cannot create profile at path" error and the partition silently fell into memory (measured), so the flat layout
  remained. `CreateSurface` now gives this context to `CreateBrowser`, not `nullptr`.
- `PartitionContextHandler` + `ApplyProxyPreference`: the container's proxy (`system`/`direct`/`fixed`) translates into the CEF `proxy`
  preference, in the `OnRequestContextInitialized` hook (when the first surface is created). The feedback goes
  to the log (`applied=1`, `error=…` on error), it does not disappear.
- `PermissionPolicy` (`CefPermissionHandler`): `media` on the `CefMediaAccessCallback`, everything else on the
  `CefPermissionPromptCallback`; **what the container does not permit, it denies**, because there is no prompt UI.
  The bitmask → name mapping (geolocation, notifications, camera, microphone, clipboard, midi, …) is in one place, and the
  `Permits` is the only "yes".
- `protocol.h/.cpp`: the `create_surface` config's new, additive fields `container`, `proxy`, `proxy_bypass`, `permissions`;
  the old shell also works without these (the proto/README.md rule).
- `home/app.js`: `openCard` carries the card's `container` field; renaming the container sends the **whole record** back
  (the policy must not be lost to a rename).
- `main_window.cpp` / `live_card.*` / `engine_client.*`: before creating the surface, the shell asks the core for the container
  table, and passes the `SurfacePolicy` (container, proxy, bypass, permissions) with `create_surface`.
- `crates/antikolt-core-service`: schema **`0005`** — the `containers` table's new columns (`proxy_mode`, `proxy_host`,
  `proxy_port`, `proxy_bypass`, `permissions`), `ALTER TABLE` only when the column is missing (an abandoned
  migration must not fail with "duplicate column"); validation: unknown mode, half-finished fixed proxy and an undecided
  permission value rejected; the `system`/`direct` mode does not carry a host/port.
- `apps/engine-host/src/main.cpp`: new `--partitiontest`, and `scripts/verify.ps1` runs it directly with the engine
  (the `Direct` marking), because the claim is about the CEF request-context behavior, not the shell.

### Evidence (measured)

```
cargo test (offline)     → 147 tests green: 78 + 47 + 6 + 1 + 15
check-repo               → ALL CHECKS PASSED
--partitiontest (with the shell's real profile root, forward slash) → EXIT=0:
  user_data=C:\Users\info\AppData\Roaming\Antikolt Project\Antikolt\engine
  partition_cache=…\engine\persist_probe-one|…\engine\persist_probe-two
  partition_contexts=ok partition_created=ok partition_distinct=ok partition_cache_paths=ok
  partition_reused=ok partition_surfaces=ok partition_pages_loaded=ok
  proxy_one=fixed_servers|socks5://127.0.0.1:1080  proxy_two=direct|
  proxy_fixed_applied=ok proxy_direct_applied=ok partition_profiles_on_disk=ok
  permission_policy=ok
  (the two profile directories are not empty: 54–54 entries)
```

**The real cause of the blocker (measured, not the HANDOFF-F3 "empty root" theory):** CEF's `ChromeBrowserContext::InitializeAsync`
compares the partition cache path's parent with the profile manager's
`user_data_dir` with an exact string match (`cache_path_.DirName() == user_data_dir`). The shell, because of Qt's `QStandardPaths`, passes the profile
with a **forward slash** (`…/engine`), but Chromium normalizes the root to **backslashes** — the forward-slash child
thus fails the match, and CEF prints the `chrome_browser_context.cc:116` `Cannot create profile at
path` error, then **silently falls into memory** (the `--partitiontest` `partition_profiles_on_disk=FAILED`). The fix:
`main.cpp` normalizes the `user_data_path` to backslashes once after reading — from then on the partition profiles really
land on disk, and `previews/`/`snapshots/`/`screenshots/` also stay in the parent (the `cef-root` subdirectory theory is
**disproven** and retracted).

**The gate is green at the end of the slice:** `scripts/verify.ps1` (software rendering, `ANTIKOLT_ENGINE_DISABLE_GPU=1`) →
**ALL SELF TESTS PASSED** (38/38, exit 0). During the slice 13 checks failed, and the first diagnosis — "a profile that writes to disk
jams the `file://`/`http://127.0.0.1` loading" — **was wrong** (the `wipetest` measurement misled: the
profile-path difference only accompanied the real cause, it was not it). The **real cause is the surface-command race**: the shell sends
the `navigate` and the `install_bridge` **immediately** after the `create_surface` reply, but CEF creates the browser
**asynchronously** (`OnAfterCreated`); the commands fell into the same pump batch, when `state.browser` was still
null, so the engine **rejected** them with `surface_not_found`, the shell ignored the error reply, and the
surface silently parked on `about:blank` — this was the "loading jam". The fix: the `navigate` is queued
(`HostState::pending_navigation`, applied by `OnAfterCreated`), and the `install_bridge` is deferred to setting the flag
alone (the `OnLoadEnd` injects the bridge). The `data:` card and the `http://127.0.0.1` probe page both
load immediately (`load_status=200`, `wipe=ok`). The per-container engine process (HANDOFF-F3 option 2) was done in a separate commit,
and next to the gate's green it is the carrier of the card containers' isolation.

### What else was completed in this slice (the above "open" points closed)

- **The surface editor is done**: the container editor (`home/app.js` legend) carries the proxy mode
  (`system`/`direct`/`fixed` + host/port/bypass) and the permission switches too, next to the name; `containers.upsert` sends the whole
  record, so the policy is not lost. Measured: `container_policy=ok` (`fixed` + `127.0.0.1:1080` +
  `geolocation=allow` in the core).
- **The partition cleanup is done**: deleting the card removes the `persist_card_<hash>` directory, deleting the container
  removes the `<profile>/containers/<id>` profile (with the engine stopped first). Measured: `container_partition_cleanup=ok`.

### What remained open (stated)

- The OAuth/SSO popup remains the next big slice; the hardware GPU path is open on VECTOR.

## 2026-11-21 (ninety-seventh slice) — all four stores SQLite-first (ADR-070)

The 96th slice made the cards a database; the containers, workspaces and sessions remained JSON documents, their
tables were only updated by `db_status(sync)`, and nothing read them. This was the last "second write": the
`sync_reference_tables` was the last remnant of the mirror. This slice makes the four stores uniformly SQLite-first.

### What changed

- `sqlite_store.rs`: schema **`0004`**. The `sessions` table now carries the ledger's full row (state, policy,
  cookie/storage sizes, service workers, `last_seen`/`last_verified`, RAM, `keep_alive`), the policies in a
  one-row `session_rules` table. The new `meta` table holds per-store adoption markers
  (`adopted:cards`, `adopted:containers`, …), because the four stores open four connections to the same file, and cannot
  depend on which opened first; `user_version` thus now means only the schema. In a v3 file, `0004` records
  `adopted:cards`, otherwise `cards.json` would be written back over the newer rows. The `sessions` rebuild tries the
  `policy` column, so a second open must not discard the already-adopted rows. `sync_reference_tables`
  **removed**; instead `upsert_/delete_/replace_all_` operations for container, workspace, session.
- `containers.rs`, `workspaces.rs`, `sessions.rs`: the store reads and writes on the `Db`; `list()` is now `Result` (an error must not
  look like an empty list), `export_json()` writes from the rows, `verify_export()` compares row by row (`-1` = not measurable).
  The pre-switch document's adoption happens per store, with the `meta` marker; unreadable document → `*Error::Legacy`
  (the core stands down, does not seed), empty document → stays empty, missing → seed (4 containers / 3 workspaces / empty ledger).
- `antikolt-core`: `LoginState::as_db`/`from_db` following the `SessionPolicy` pattern, so the JSON ledger's values are read back
  exactly; an unknown value is a loud error in the core.
- `persist.rs` (new): the atomic JSON export in one place (temp file → `fsync` → rename), all four stores use it.
- `main.rs`: `db_status` checks the **four exports** (`exports`, the `checked` sum, `mismatches`, `verified`; if
  any export is unreadable, `mismatches = -1` = `not measured`); `export_all` writes on shutdown, after import and on
  `db_status(sync)`; `session_list`/`session_due`/`profile_export` pass the `Result`s on as errors.
- `apps/desktop-shell/src/main.cpp`: the `--addcardtest` storage check now also expects `schema ≥ 4`, `containers ≥ 4`,
  `workspaces ≥ 3`, and prints the `sessions=` / `checked=` number — the gate measures on the four stores, not just the
  cards.

### Evidence (measured)

```
cargo test (offline)     → 144 tests green: 78 + 45 + 5 + 1 + 15
check-repo               → ALL CHECKS PASSED
--- full gate (software rendering) ---
1st run (started at 6:41): 1 CHECK(S) FAILED — only the `traytest` single-instance branches: meanwhile a portable
   Antikolt was running on the machine (`instance_primary=FAILED`, `instance_raised=FAILED`, because the request went to the running foreign
   instance; the `instance_second=instance=secondary exit=0` and the `tray_*` items are green) — it did not come from the code,
   the single-instance guard spoke correctly.
2nd run (6:58:29–7:02:49, after closing the running instance): ALL SELF TESTS PASSED, 4m20s, 37 self tests + check-repo

addcardtest:
  add_card=ok typed=submitted before=10 after=11 page=11 error=""
  db_status=ok schema=4 cards=11 verified=yes containers=4 workspaces=3 sessions=16 checked=35
  add_card_check=ok
containertest:
  container_used_delete=ok shown="1 card is still in this container" move=offered
  container_move_delete=ok container_gone=yes card_in=c-private
rulestest: session_rules=ok
traytest:  tray_and_instance=ok
```

The `checked=35` is the row-by-row sum of the gate profile's four exports (11 cards + 4 containers + 3 workspaces + 16
session rows from earlier runs + 1 policy item), and all 35 match. **The adoption measured on the real profile too:**
the `%APPDATA%\Antikolt Project\Antikolt\core` file is now `user_version=4` (the header `00 00 00 04`), the four `*.json`
were written out as exports on shutdown (7:02:46), and the 10 existing cards survived (`before=10`).

### New tests

`containers_round_trip_in_legend_order_and_a_replace_drops_rows`, `workspaces_round_trip_in_tab_order_and_a_replace_drops_rows`,
`a_session_row_round_trips_with_every_ledger_field`, `the_session_rules_are_one_row_that_starts_absent`,
`replacing_the_ledger_takes_rows_and_rules_together`, `a_fresh_database_migrates_to_the_current_schema_and_remembers_its_adoptions`,
`a_v2_file_has_no_adoption_markers_and_a_v3_file_already_adopted_the_cards` (on the `Db`), per store
`a_fresh_store_is_seeded_in_the_database_and_exported`, `a_pre_switch_document_is_adopted_once_and_emptying_it_stays_empty`,
`an_unreadable_legacy_document_stops_the_core`, `the_export_is_written_from_the_rows_and_can_be_checked`,
`a_pre_switch_ledger_is_adopted_with_its_rules_once`, `a_fresh_ledger_is_empty_and_survives_a_reopen`, e2e:
`the_container_workspace_and_session_documents_are_adopted_once_and_then_rows`, and the `an_export_replaces_the_file_in_one_step_and_leaves_no_temporary`.

### What remained open (stated)

- The hardware GPU path remains open on VECTOR; on this machine the gate runs with software rendering
  (`ANTIKOLT_ENGINE_DISABLE_GPU=1`).
- The per-container permission/proxy (F3) and the OAuth popup are the next big slice.
- The four `*.json` exports can become stale without `sync` — `db_status` also states this (`verified: no`), it does not lie about
  a match; after shutdown and import, however, they are always fresh.
- The `checked` count counts the session policies as one item (the document describes them as one object),
  the rows one by one.

## 2026-11-21 (ninety-sixth slice) — the card store SQLite-first: the JSON becomes an export (ADR-069)

The 64th–66th slices built a **mirror**: the JSON file wrote, the database followed, and the two had to be kept in sync
by hand. This proved the migration on real data (`db_status` re-checked it row by row), but the 81st slice also
showed its cost: the `container_delete` `move_to` branch left silent staleness in the mirror, because the count matched. This
slice **eliminates the second write**: the cards have a single writer, `antikolt.sqlite3`, and `cards.json`
becomes a derived **export**.

### What changed

- `store.rs`: the `CardStore` no longer holds a file but the `Db`. The `open` **adopts** the `cards.json` once from the **pre-switch** file
  (`user_version` 0–2) (with store normalization: trim/dedup/tag ceiling,
  page-text ceiling), then writes the `3` (`Db::mark_switched`). If the old JSON is unreadable, the core **refuses**
  (`StoreError::Legacy`, stating the filename) — it does not seed over the user's cards.
- **An empty profile is not reseeded:** the sample cards are created only if the database file is new **and** there is no
  JSON to adopt. Whoever deleted all their cards gets an empty grid on the next launch too — this is the naive "if empty, seed"
  bug that the `a_profile_the_user_emptied_stays_empty` test rules out.
- `sqlite_store.rs`: schema **`0003`** (no new table: the number itself is the marker that the file is no longer a mirror), `move_container`
  is a single SQL `UPDATE`, `replace_all_cards` in one transaction, `sync_reference_tables` for the container/workspace
  reference tables. The `mirror`, `upsert_container`, `upsert_workspace` **deleted**.
- `main.rs`: the `mirror_card`/`mirror_delete` calls and the list's self-healing branch are gone; the `cards_list` is a plain read;
  the `db_status` measures the **DB ↔ export** match row by row (`checked`, `mismatches`, `verified`), the `sync` refreshes the derived
  copies (export from cards, reference tables from the JSON stores); export on shutdown and after import.
- **The export is not written on every launch:** on adoption, on shutdown, and on `db_status(sync)`. Serializing the whole list
  on every launch would bring back the old O(n) write to the startup path — exactly what the swap eliminated.

### Evidence (measured)

```
cargo test (offline)     → 129 teszt zöld: 78 + 31 + 4 + 1 + 15
check-repo               → ALL CHECKS PASSED
--- teljes kapu (szoftveres renderelés) ---
1. futás, teljes build:  pwsh -File scripts/verify.ps1 → ALL SELF TESTS PASSED, 4m41s, 37 önteszt + check-repo
2. futás, a végleges fán: ALL SELF TESTS PASSED (ugyanaz a 40 sor, a végleges kóddal)

addcardtest:
  add_card=ok typed=submitted before=10 after=11 page=11 error=""
  db_status=ok schema=3 cards=11 verified=yes containers=4 workspaces=3
  add_card_check=ok
containertest:
  container_used_delete=ok shown="1 kártya még ebben a konténerben van" move=offered
  container_move_delete=ok container_gone=yes card_in=c-private
```

**Adoption also measured on the real profile:** the gate ran on this machine's pre-switch profile left over from an earlier build
(`%APPDATA%\Antikolt Project\Antikolt\core`: `user_version=2` mirror + `cards.json`). The first launch adopted it, the **10
existing cards survived** (`before=10`), and the file is now `user_version=3` (`SQLite format 3`, the header's 4 bytes
`00 00 00 03`) — no seeding and no row was lost.

The `verified=yes` now measures the **reverse direction**: not that the mirror rewritten from JSON matches, but that the
export **written out from the database** returns the same rows the core reads — every row, not the first one.

### New tests

`a_pre_switch_json_profile_is_adopted_once`, `a_profile_the_user_emptied_stays_empty_instead_of_reseeding`,
`an_unreadable_legacy_profile_stops_the_core_instead_of_being_replaced`, `the_json_document_wins_over_a_stale_pre_switch_mirror`,
`an_adoption_that_discards_rows_keeps_them_in_a_backup` (the `VACUUM INTO` backup before the adoption that discards rows),
`the_export_is_a_faithful_copy_the_report_can_verify`, `moving_a_container_is_one_statement_that_touches_only_its_cards`,
`replacing_the_whole_list_drops_the_rows_that_are_gone`, `deleting_a_card_removes_exactly_that_row`,
`the_reference_tables_are_refreshed_from_the_json_stores`, e2e: `the_database_is_the_store_and_the_report_proves_the_export`,
`a_profile_written_before_the_switch_is_adopted_not_reseeded`.

### What remained open (stated)

- **Containers, workspaces and sessions** remain JSON documents; the core's `containers`/`workspaces`/
  `sessions` tables are only refreshed by `db_status(sync)`, and nothing reads them. Their own swap is a separate slice.
- The `cards.json` **may go stale** after the last export/shutdown — `db_status` says so even without `sync`
  (`verified: no`), it does not lie about a match.
- Jumping backwards (a new file with an old build) is not supported: the old build reads `user_version=3` and would write the mirror.
- The gate on this machine still runs with software rendering (`ANTIKOLT_ENGINE_DISABLE_GPU=1`); the hardware GPU path
  is open on VECTOR (slice 93).

## 2026-11-20 (ninety-fifth slice) — the handover to the Flash model: stated, with an escalation rule

The handover of the remaining work to **DeepSeek V4.1 Flash** went into `HANDOVER.md`, unambiguously: the deep native
debugging is already closed by **DeepSeek V4 Pro** (slices 90–94), Flash carries the feature code, the SQLite-first
store swap, the container-permission/proxy + OAuth and the release. The point is a **non-negotiable escalation
rule**: if Flash cannot localize a small native bug within a few attempts, **it must stop and state that
"Pro model required"** together with the evidence — not burn futile attempts on the same dead end.
The reason is measured: V4.1 Flash is strong in coding (54.2) and reasoning (75.8), but weak in autonomous multi-file
bug hunting (agentic 39.5) — that part stays on Pro. The rule is also recorded in section 0 of `HANDOVER.md` and in the
"Handover in Hungarian" block, so a fresh session sees it immediately.

## 2026-11-20 (ninety-fourth slice) — the frame distortion: the engine's `stdout` was written in text mode (CRLF), and `cards.upsert` stalled

The `containertest` stall believed in slice 93 to be "renderer slowness" **was not a renderer problem**, but the
distortion of the protocol frame. The tracer gave the exact number: the shell read
**`frame length=1116685`** for the `cards.upsert` frame instead of the correct `4362`. `1116685 = 0x110A0D` → bytes `0D 0A 11 01`:
the `0x0A` byte became `0D 0A` (CRLF).

### The root (measured)

The engine writes the frame length in 4 bytes, in binary (`EncodeLength`), but **`stdout` was in the CRT's default
text mode** — Windows translates the `0x0A` (LF) byte inside it to `0x0D 0x0A` (CRLF). The
`cards.upsert` frame is 4362 bytes (`0x110A`), whose **low length byte is exactly `0x0A`** — so the 4-byte
header became 5 bytes, and every frame after it shifted. None of the length bytes of the smaller frames (37, 42, 122, 916 …)
is `0x0A`, so it never surfaced before; `cards.upsert` was the first frame
whose length ended in `0x0A`. This is the same class as slice 91's stdio fix — there I made the
**read** binary, not the **write**.

### The fix

`apps/engine-host/src/protocol.cpp` `StdioLink::Start()`: on Windows `_setmode(_fileno(stdout), _O_BINARY)`
before any frame is written out. The frame channel is now byte-exact.

### Evidence

```
--containertest  → exit=0 12 s   container_page=ok container_core=ok container_restored=ok
                                 container_legend=ok container_filter=ok container_table=ok
                                 container_created=ok container_reload=ok container_rename=ok
                                 container_used_delete=ok container_move_delete=ok container_switch=ok
```

The trace: `frame length=4362` (instead of the faulty `1116685`) → `card_upsert` toward the core → `bridge_reply`. The
`containertest` is therefore **green even with software rendering** — the earlier "slowness" was an illusion caused by the
frame distortion (the page's `cards.upsert` never got an answer, its JS waited, the renderer main thread blocked).

### The hardware GL gate (with slice 93's workaround)

With `ANTIKOLT_ENGINE_EXTRA_SWITCHES='--use-angle=gl'` the whole gate is **36/37**: every windowed test (including
`containertest`), plus `coretest`, `sessionpanel`, the preview row and `check-repo` are green. The only failure is
`capturetest`, which is **green in isolation** (2.3 s, `capture=ok capture_size=ok`) — so a transient
timeout under load, not a code bug. The software-rendering gate is fully green.

## 2026-11-20 (ninety-third slice) — the bisect of the hardware GPU path: the d3d11 backend is faulty on the 5080, GL works

I bisected the hardware GPU crash (RTX 5080 + Chromium 158 beta) with `ANTIKOLT_ENGINE_EXTRA_SWITCHES`,
without recompiling. The measured result:

- **default (d3d11):** the GPU process dies (`GPU process exited unexpectedly`, three times), but
  `--embedtest` is still `exit=0` (the test does not fail on a GPU death — this gap is worth closing later).
- **`--use-angle=gl`:** the GPU process **stays alive**; `--embedtest` → `embed=ok paint=ok`, the engine's
  `--selftest` → `load=ok`, without a single GPU death.
- `--use-angle=d3d9` and `--use-gl=desktop`: `Requested GL implementation … Exiting GPU process` — not good.

So the d3d11 ANGLE backend is the faulty one on the 5080, while the GL backend works. The hardware GPU path on VECTOR can thus be
run like this: `$env:ANTIKOLT_ENGINE_EXTRA_SWITCHES='--use-angle=gl'`.

### What GL does not solve either

The `containertest` is not green with GL either: the re-render after `cards.upsert` in the renderer **stays slow** (the
polling `eval`s answer, but with ~20 s delay, so the 25-attempt loop lingers). This is no longer a
GPU-backend fault, but the page/renderer behaviour after the container switch — it deserves its own slice.

## 2026-11-20 (ninety-second slice) — the source of the containertest crash found (AddressSanitizer) and fixed

I localized the `containertest` `0xC0000005` (Qt6Core) crash left open in slice 91 **with AddressSanitizer** —
not with a debugger, but with MSVC's already existing `/fsanitize=address` flag. The bug was the
"Qt-timer + lambda captured by reference" trap documented in slice 31, in the shell's test helper function.

### The root (measured)

The `evaluate_json` helper (and its pair, `evaluate_text`) captured its own `QEventLoop` and its result **by reference**
(`[&]`) in the engine-host's reply callback. If an `eval` runs out of its 20 s, the helper's own timer first
exits the loop (the `loop`/`value` on the stack is destroyed), but the callback still sits in `EngineClient::pending_`
— when the engine-host's own 20 s deadline (`EngineClient::send`) fires afterwards, the callback calls `quit()` on an **already
destroyed** `QEventLoop`. The ASan output gave the exact line:

```
#1  runContainerCheck::<lambda>::<lambda>  main.cpp:5409          ← loop.quit() a halott loopon
#5  EngineClient::send::<lambda>           engine_client.cpp:120  ← a 20 s-os határidő
```

### The fix

The helper's state (`loop` + `value`/`text`) goes onto the **heap** (`std::make_shared<State>`), and the callback captures
the `state` **by value** — so a late-arriving deadline/reply never reaches dead stack. I fixed the same
pattern in the other test helpers too (7 `evaluate_json`/`evaluate_text` + the `page_table`), because the same
latent bug was in them.

### The ASan tool (first time in the repo)

- `apps/desktop-shell/CMakeLists.txt`: `ANTIKOLT_ASAN` option (for the shell; on MSVC `/fsanitize=address`).
- `scripts/build.ps1`: `-Asan` switch → `pwsh -File scripts/build.ps1 -Asan -BuildDir build-asan`.
- The shell is ASan-instrumented, the engine-host is **not** (CEF's child processes do not mix with the ASan runtime).

### Evidence

The `containertest` under ASan **no longer crashes** (the UAF is gone), the other three windowed tests stayed green:

```
searchtest  → exit=0 7.6s    search_check=ok
notetest    → exit=0 8.7s    note_save=ok note_rendered_ok=ok
addcardtest → exit=0 12.1s   delete_armed=ok delete_card=ok add_card_check=ok
```

### What must still be seen after the fix (not the shell's fault)

After the UAF fix the `containertest` **does not crash, but stalls** at the re-render after `cards.upsert`: the trace
shows the engine **receives** the `eval id=19/20/21` requests (`op=eval … surface=1`), but **does not answer** them —
with SwiftShader (software rendering) the re-render blocks the renderer's DevTools. This is the same
software-rendering slowness as the `notetest` gate timeout: on a healthy GPU it is not expected to appear (the
measurement of the hardware GPU path on VECTOR has been open since slice 90).

## 2026-11-20 (ninety-first slice) — the stdio lifecycle bug resolved (the engine no longer dies mid-test)

The "non-GPU" blocker stated in slice 90 was in the **engine's own stdio**, not in the shell. The engine reads the
protocol from its `stdin` (`StdioLink::ReaderLoop`, `fread(stdin)`); however Chromium (CEF), when starting its
child processes (renderer/GPU), **closes the browser process's inherited stdin handle**. The engine therefore
saw an early EOF after the page's first `eval` (`[engine-host] stdin closed`), and exited (`shutting down`, code 0)
— while the shell kept writing frames. The trace (`ANTIKOLT_ENGINE_TRACE=1`) proved it: the shell's
`write`/`wrote`/`flushed` markers ran **after** the engine's `stdin closed`, so it was not the shell that closed the channel.

Fix: `StdioLink::ReaderLoop` reads from a **private duplicate** instead of `stdin` (`DuplicateHandle` →
`_open_osfhandle` → `_fdopen`, `apps/engine-host/src/protocol.cpp`). Even if Chromium closes the original
handle, the duplicate keeps the pipe's read end alive while the shell writes.

### Evidence (the actual output)

```
--notetest    → exit=0 8.4s    note_save=ok note_write=ok note_rendered_ok=ok note_focus_ok=ok
--searchtest  → exit=0 6.9s    search_accent=ok search_page_text=ok search_escape=ok search_check=ok
--addcardtest → exit=0 11.9s   delete_armed=ok delete_card=ok add_card_check=ok
```

The full gate (`verify.ps1`, `ANTIKOLT_ENGINE_DISABLE_GPU=1`) now **runs to completion** (it no longer stalls at `searchtest`),
and its result table is: **35 of the 37 windowed self-tests ok**, plus `coretest`, `sessionpanel`, `scrollbook`,
`traytest`, `logtest`, `preview*`, `rulestest` and `check-repo` all ok. The two remaining failures are **not the engine
lifecycle**:

- `notetest` in the full gate is `note_save=FAILED (timeout waiting for the engine-host's reply)` — green in isolation (8.4 s),
  so it is the slowness of software rendering + the state accumulated during the gate, not a fault of the note editor.
- `containertest` crashes with `0xC0000005` (Qt6Core) around the page's `cards.upsert` bridge call — reproducible in
  isolation too; this is a lifetime bug of the shell's bridge callback (the class of the "Qt-timer + lambda"
  trap already documented in slice 31), it deserves its own slice.

### What I do not claim

The gate is **not** "ALL SELF TESTS PASSED" on this machine: `notetest` (flaky timeout) and `containertest`
(Qt6Core access violation) still fail. Of the two, `containertest` is a real bug; the `notetest` timeout comes from the
speed of software rendering, and is not expected to appear on a healthy GPU.

## 2026-11-20 (ninetieth slice) — the CEF-GPU blocker resolved (measured), and what was behind it

I eliminated `HANDOVER.md`'s single blocker. The bug was not in the GPU driver but in our **own
code**: the `ANTIKOLT_ENGINE_DISABLE_GPU=1` branch requested `--use-angle=swiftshader`, but immediately after that
it also switched on `--disable-software-rasterizer` — the latter **turns off** SwiftShader, so the software
fallback did exactly the opposite of what its name promised. Together with `--disable-gpu --disable-gpu-compositing`,
these two contradictory switches produced the documented error: `ContextResult::kFatalFailure: Failed to
create shared context for virtualization`. Fix: `--disable-software-rasterizer` (and `--disable-dev-shm-usage`,
which does nothing on Windows) was removed from `apps/engine-host/src/host.cpp`.

On the VECTOR machine the **hardware** GPU path itself dies (RTX 5080 + Intel iGPU, Chromium 158 beta; the GPU process
exits with a `0x80000003` breakpoint, and `--use-angle=d3d11` does not help either), so on this machine the software path is needed —
that is what `ANTIKOLT_ENGINE_DISABLE_GPU=1` is for, and it works after the fix.

### Evidence (the actual output)

```
antikolt-engine-host --selftest        (ANTIKOLT_ENGINE_DISABLE_GPU=1):
    exit=0   gpu_exited=0   shared_context_fail=0   load=ok   status=200
antikolt-shell --embedtest <motor>     (ANTIKOLT_ENGINE_DISABLE_GPU=1):
    exit=0   elapsed=2.6s   embed=ok   viewport_match=ok   paint=ok   focus=ok   capture_size_match=ok
```

The `--embedtest` previously hit a 150 s watchdog timeout; after the fix it is green in **2.6 s**, with real
pixels (`capture_colours=25`).

### What this made visible (and stayed open)

The gate (`verify.ps1`) now **runs** (the GPU-crash loop is gone), but does not reach "ALL SELF TESTS PASSED": it
stops at the 18th windowed test (`searchtest`), and `notetest` also fails. The common sign: mid-test the engine-host
reports `stdin closed`, then exits (`shutting down`, code 0), and after that the shell asks in vain — the
`note_save=FAILED (the engine-host exited (code 0))`, and `--addcardtest` gets stuck at the `evaluate_click_add` step.
This is **not a GPU bug**, but a shell↔engine stdio lifecycle bug that the GPU blocker had been masking. It deserves a
separate slice.

### What I do not claim

The gate's full green is **not measured** in this slice (because of the `searchtest` stall), and the measurement of the two-step
delete (ADR-068) via `--addcardtest` also stops on this stdio bug — so ADR-068 is now **measurable**, but not
green, rather "measured, and it shows the stdio lifecycle bug".

## 2026-11-20 (eighty-ninth slice) — the work is handover-able: `HANDOVER.md`

The slice's goal is not a product feature, but that the work be **handover-able** to a new workflow. The
[`HANDOVER.md`](../../HANDOVER.md) is done (English + Hungarian): status, the exact verification commands, the **measured blocker**
(CEF GPU), the next three slices, the non-negotiable rules, and a "where to look first" table. The `README.md`
also references it from its paragraph.

### Evidence

```
check-repo → ALL CHECKS PASSED (benne: every relative link resolves — a HANDOVER.md új hivatkozásai is)
```

### What remained open (unchanged)

The gate (37 self-tests) still did not run to completion on this machine, the two-step delete (ADR-068) is `not-measurable`. The first
step of the handover is therefore exactly this: run it on a host with a healthy GPU, and log the result.

## 2026-11-20 (eighty-eighth slice) — the freeze is not a freeze: the machine slowed down

After the eighty-seventh slice I put back a **heartbeat clock** (a 2-second tick to stderr) and exit/enter
markers on the `eval` calls. The result rewrites the diagnosis:

```
[selftest] tick  × 244        (≈ 488 s alatt — az eseményciklus VÉGIG fut)
[selftest] eval_enter × 25 / eval_exit × 24   (a hívások válaszolnak, csak lassan)
```

**So the main thread does not block**: there is no embedded blocking call, no silent freeze. The CEF GPU-crash loop
(a GPU process restarting every second) **sucks up the CPU**, so a 40-second test takes several minutes, and
the 150-second watchdog closed it early — the earlier "freeze" **was the watchdog's fault, not the code's**.

### What this slice provides

- The diagnosis became precise: there is **no blocking call** in the bridge write, in event handling and in `eval`; the
  cause of the slowdown is the GPU-crash loop.
- The 600-second watchdog variant and the tick markers **did not stay in** (introducing the tick marker corrupted the
  file, so `git checkout` restored the committed state — the lesson: line-by-line text rewriting in C++
  is the same trap as the PowerShell splice).

### What remained open

The measurement is due **on another machine** (or with a working CEF GPU): there the 37 self-tests run in ~1 minute, and the two-step
delete (ADR-068) also becomes proven. On this machine the slowdown remains because of the GPU-crash loop.

## 2026-11-20 (eighty-seventh slice) — the location of the freeze measured (not the bridge write)

The eighty-sixth slice's watchdog told *that* we freeze; this slice measured **where**. Two
diagnostic traces went into the code (off by default, switched on with `ANTIKOLT_ENGINE_TRACE=1`):

```
[selftest] step=evaluate_click_add
[engine-client] op=eval id=13 write bytes=317
[engine-client] op=eval id=13 wrote bytes=317
[engine-client] op=eval id=13 flushed bytes=317
   … majd: GPU process exited unexpectedly (ismétlődve), és semmi válasz, semmi timer
```

**Result:** the card write (frame + `write` + `waitForBytesWritten`) **completes**, so it is not the bridge write that blocks.
The shell stops under `loop.exec()` in such a way that **its own `QTimer`s do not run either** — this is only possible
if the main thread is in an *embedded, blocking* call inside a signal handler, in the midst of the CEF GPU-crash
events. The next slice's job is therefore the `MainWindow` event dispatcher (`handleEvent` → `onEngineEvent`),
not `send`.

### As an aside: `--coretest` was never faulty

The eighty-sixth slice's "unexplained" `core_ready=FAILED` **was my fault**: I passed the test the engine-host's
path instead of the core's. With the correct argument:

```
--coretest <target\debug\antikolt-core-service.exe>
core_start=ok · core_name=antikolt-core · cards=3 · session_ledger=ok · exit=0
```

### What is measured

```
pwsh -File scripts/check-repo.ps1 → ALL CHECKS PASSED
--coretest (helyes útvonallal)   → exit=0
--addcardtest                    → a fenti nyomok; watchdog: FAILED exit=1 (nem fagyás)
```

The gate still does not run (the CEF GPU fault is on the machine's side), the two-step delete (ADR-068) is still
`not-measurable`.

## 2026-11-20 (eighty-sixth slice) — a loud failure instead of a silent freeze (and the environment going down)

After a restart **the CEF GPU fault remained**, so it is not a passing condition: the environment cannot create a GPU
context (`Failed to create shared context for virtualization`, `GPU process exited unexpectedly:
exit_code=-2147483645`). Because of this every windowed self-test froze — the shell waited in a **blocking** engine call,
and the `QTimer` did not run either, because the event loop itself stopped.

**Fix (this slice's value):** every `--*test` run gets a **watchdog** (`std::thread`) that writes output
and **exits with code 1**, if no verdict is produced. The freeze can thus no longer be "silently green":

```
add_card_check=FAILED (no verdict in 150 s: the host stopped responding)   exit=1
```

Two further switches went into the engine-host (environmental, not product settings): beside `ANTIKOLT_ENGINE_DISABLE_GPU=1`
also `--use-angle=swiftshader --disable-gpu-sandbox` (the GPU-crash loop stopped: 0 `GPU process exited`),
and `ANTIKOLT_ENGINE_WINDOWLESS=1` (CEF `windowless_rendering_enabled`) for hosts without a compositor. **Neither is
enough**: creating the shared context still fails, and after the first evaluation of the page JS the shell blocks.

### What is measured in this slice

```
pwsh -File scripts/check-repo.ps1 → ALL CHECKS PASSED (SPDX, linkek, JS-parse, fmt, clippy, 78+21+2+1+15 Rust-teszt)
antikolt-core-service.exe --data-dir … → "[core] cards=3 sessions=0 containers=4 workspaces=3" (a mag önmagában jó)
--addcardtest → home_surface=1, add_card_before=10, majd watchdog: FAILED exit=1 (nem fagyás, nem zöld)
--coretest     → core_ready=FAILED … exit=1 (nyitott: a mag-bináris külön fut, a shell-teszten belül nem jön létre a kapcsolat)
```

### What I do not claim

The gate **did not run to completion**, the two-step delete (ADR-068) is still **`not-measurable`**, and the sudden
failure of `--coretest` is **not explained**. This slice did not fix the bug, but **made it visible** — which
is more than a silent freeze, but less than green.

## 2026-11-20 (eighty-fifth slice) — two-step delete (implemented, but NOT measured)

Deleting a card was previously a single click — even though the card is a large click target that the user clicks in order to
open it. Now (ADR-068) the meta-row's `×` **first arms** it (`Sure?`, red), and only the second click within five
seconds deletes; afterwards the arming expires on its own.

The implementation is done: `requestDelete()` on the page, the `armedDelete` state, the button's `data-armed` marker, CSS, and
the `--addcardtest`'s new step (`delete_armed`).

### The measurement, however, failed on the environment — and I state it

```
antikolt-shell --addcardtest …  → (nincs kimenet), 200 s után TIMEOUT
stderr: GPU process exited unexpectedly: exit_code=-2147483645
        GPU state invalid after WaitForGetOffsetInRange
ANTIKOLT_ENGINE_DISABLE_GPU=1 mellett is → nincs kimenet, TIMEOUT
```

The environment's CEF GPU process dies on every windowed run, so **the two-step delete gesture is
`not-measurable`**, not green. What I could measure:

```
pwsh -File scripts/check-repo.ps1 → ALL CHECKS PASSED
  ok  home/app.js parses      (a lap szintaxisa jó)
  ok  cargo fmt / clippy -D warnings / cargo test (78 + 21 + 2 + 1 + 15)
  ok  every relative link resolves, SPDX, nincs commitolt build/titok
```

This is the static truth, not proof of the behaviour. `21-ALLAPOT.md` and ADR-068 also record it this way; the next
measurement is due under a clean GPU state (after a restart), and if it is green then, the log gets another slice.

## 2026-11-20 (eighty-fourth slice) — the numbers aligned with the measurement (37 self-tests, not 41)

After publishing I looked up the **numbers** in the documents and compared them with the gate's actual output.
The "41 self-tests" claim **was inflated**: based on the gate's log (test-results, `ok` lines), **37 self-tests** run
(`gridtest` … `rulestest`), plus `check-repo`. The number of Rust tests, in contrast, was right: 78 + 21 + 2 + 1 + 15 = 117.

Fixed: `README.md` (both paragraphs), `docs/EN/README.md` (the gate line), `docs/HU/21-ALLAPOT.md` (the gate table, the
"how it can be verified" section and the CI explanation).

### Evidence

```
gate log (ok sorok, `theme=` al-sorok nélkül) → 37 teszt + check-repo
check-repo log                               → running 78 / 21 / 2 / 1 / 15 tests, mind ok
pwsh -File scripts/check-repo.ps1            → ALL CHECKS PASSED
```

### Lesson

In a public repo, **a number is also a claim**. The badge and the gate were green, and the document sat on a
rounding — exactly the kind of "green that lies" that the project's rule forbids. So from now on the
numbers in `21-ALLAPOT.md` must be taken from the gate's log, not from memory.

## 2026-11-20 (eighty-third slice) — the repo is public: Antikolt, MPL-2.0, in two languages

The project stepped out of the machine: **https://github.com/svandor/Antikolt** — a public repo, MPL-2.0, under the name Varga Sándor
(`@svandor`), with Hungarian–English documentation.

- **License:** `LICENSE` (MPL-2.0 full text), `NOTICE` → `Copyright © 2026 Varga Sándor (svandor) and
  contributors`, third parties (CEF BSD, Qt LGPL-3, SQLite, crates) and the declaration of AI contribution; `AUTHORS`
  with the maintainer's real name (the earlier `@antikolt` placeholder was removed).
- **Bilingual description:** at the top of `README.md` badges (CI, license, platform, measured state) and an **English
  paragraph** beside the Hungarian status line; `docs/EN/README.md` writes the **measured** state instead of the earlier "design phase, no runnable code"
  claim, with a "What is proven today" table.
- **`.gitignore`:** seven reason groups (build, user data, secrets, toolchain downloads, node/cache, raw
  measurements, editor/OS), not a blind extension list.

### Evidence (actual output)

```
gh repo view svandor/Antikolt → visibility PUBLIC, defaultBranch main,
   licenseInfo mpl-2.0 ("Mozilla Public License 2.0"), leírás kitöltve
távoli fa                    → files=149; build/, dist/, target/, node_modules/, cef/,
                               *.exe, *.dll, *.sqlite3 → NINCS
titok-szkennelés             → nincs találat (gho_ token, privát kulcs, api_key minta)
git check-ignore             → build/, dist/, target/, node_modules/, .env, cef/, screenshots/ ignorálva;
                               docs/, crates/, Cargo.lock, .github/ követve
CI (első push)               → completed success, 2m12s
```

### What remained open

The **installed instance's** `--addcardtest` measurement still failed: the engine's GPU process died
(`GPU state invalid after WaitForGetOffsetInRange`), timeout after 150 s — the same code on the `build/` binary is green,
so this is environmental, not a code bug. It must be run again under a clean GPU state; `21-ALLAPOT.md` states it.

## 2026-11-19 (eighty-second slice) — the grid order stays the same after the switch

The store swap would have silently changed the grid's order: the JSON store sorts in Rust, but the unsorted
SQL `SELECT` returns **insertion** order — so opening a card would not have re-sorted the grid.
Fix (ADR-067): `ORDER BY last_opened_at_ms DESC, LOWER(title) ASC`, and a test that deliberately inserts two cards in a reversed
insertion order.

### Evidence

```
cargo test -p antikolt-core-service
  the_database_returns_the_stores_order_not_the_insertion_order ... ok
  test result: ok. 21 passed + 2 passed
clippy -D warnings → tiszta
antikolt-shell --addcardtest … → add_card=ok, db_status=ok schema=2 cards=11 verified=yes, add_card_check=ok
verify.ps1 → ALL CHECKS PASSED + ALL SELF TESTS PASSED (a szelet kódján)
```

### An honest note — measuring the installed instance did not succeed this time

`dist.ps1 -Release` ran (`445.6 MB`, shortcut), but on the **installed** binary `--addcardtest` produced no
output: the engine's GPU process died (`GPU state invalid after WaitForGetOffsetInRange`,
`GPU process exited unexpectedly: exit_code=-2147483645`), and the shell did not advance even up to the 240 s wait. This is the
previously documented environmental fault (on the `build/` binary the same code was green minutes earlier), but by the
rule **I do not claim that the installed instance is verified**: this slice is `not-measurable` there, because of the
environment's GPU state. It must be run again on a restarted machine / under a clean GPU state.

## 2026-11-19 (eighty-first slice) — the mirror follows the writes row by row (and the fix of a silent staleness)

The eightieth slice rewrote the whole mirror on every list request (an O(n) write). Now (ADR-066) the **writes**
update their own row (`mirror_card`/`mirror_delete`), and the list only **reads** — except when the count
does not match, because then it rebuilds once and writes to stderr why it had to.

This slice also found a **silent staleness**: `container_delete` with `move_to` moved the cards in the JSON,
but the mirror kept the old container — the count matched, so nothing corrected it, and the
`--containertest` failed with the line `container_move_delete=FAILED container_gone=yes card_in=c-proba`. The fix:
after a move, every affected card's row is refreshed in the mirror.

### Evidence (actual output)

```
antikolt-shell --addcardtest <engine>
add_card=ok before=10 after=11 page=11
db_status=ok schema=2 cards=11 verified=yes
add_card_check=ok

antikolt-shell --containertest <engine>
container_move_delete=ok container_gone=yes card_in=c-private
container_switch=ok
```

```
verify.ps1 → ALL CHECKS PASSED + ALL SELF TESTS PASSED
dist       → 445.6 MB, a telepített binárison mindkét teszt zöld
```

### What remains open

The JSON store is still the **writer**. The next slice is the switch of writing to SQLite (the JSON becomes the export), with which the
sync of the two writes goes away — until then, every place where an op writes several cards must be mirrored by hand (this
slice showed exactly that).

## 2026-11-19 (eightieth slice) — the grid's list comes from the database

After the mirror, the sensible half of the swap (ADR-065): `cards_list` **mirrors first, then reads** — the card list
comes from SQLite. The write location did not change (the JSON store), so the switch does not risk data loss, and if the
count does not match, the core **does not serve uncertain data**: it serves the JSON list and
writes a `[core] db_list_mismatch` line to stderr.

### Evidence (actual output)

```
antikolt-shell --addcardtest <engine>
add_card=ok typed=submitted before=10 after=11 page=11 error=""
edit_card_title=ok … wrote_by=gesture
edit_card_url=ok gesture=typed
db_status=ok schema=2 cards=11 verified=yes
delete_card=ok answer=ok remaining=10 expected=10
add_card_check=ok
```

```
verify.ps1  → ALL CHECKS PASSED + ALL SELF TESTS PASSED
```

### What remains open

The mirroring is an O(n) write on every list request. The next slice is the switch of the **write** into the database (the JSON afterwards
is the export), with which this cost goes away.

## 2026-11-19 (seventy-ninth slice) — the mirror proves a full match (and revealed an append-only bug)

After the seventy-eighth slice the mirror only checked the **first** card. Now **every** row: `db_status`
walks through the JSON store's card list and compares it field by field with its SQLite pair (`mismatches`).

This immediately revealed the bug: `db_status=FAILED … verified=no`, because the mirroring was **upsert-only** — the
card deleted by the user stayed in the database forever (14 rows instead of the JSON's 11). The fix is the correct
form: **`mirror()`** — deletes the three tables in one transaction, then rewrites them from the JSON store's content. Afterwards:

```
antikolt-shell --addcardtest <engine>
db_status=ok schema=2 cards=11 verified=yes containers=4 workspaces=3
add_card_check=ok
```

### Gate

`verify.ps1` → **ALL SELF TESTS PASSED**; `check-repo` → ALL CHECKS PASSED (78 + 20 + 2 + 1 + 15 Rust tests,
`cargo fmt`, `clippy -D warnings`).

### What remains open

The **actual swap**: the grid and the filters still read from the JSON stores. The mirror now *provably*
shows a full match, so the next slice is the switch of the reading — and if that measurement fails, we will know
in which rows.

## 2026-11-19 (seventy-eighth slice) — the database is visible on the product path too

The seventy-seventh slice put the database into the core, but the page did not reach it. Now it does (continuation of ADR-064): bridge
`db.status`, page-side `antikoltDbStatus(sync)`, and `--addcardtest` asks the mirror **via the interface** —
the answer also includes the schema, the counts and the `verified` field.

### Evidence (actual output)

```
antikolt-shell --addcardtest <engine>
add_card=ok typed=submitted before=10 after=11 page=11 error=""
db_status=ok schema=2 cards=11 verified=yes containers=4 workspaces=3
delete_card=ok answer=ok remaining=10 expected=10
add_card_check=ok
```

On the installed instance the same is `schema=2 cards=13 verified=yes` (the profile had that many cards meanwhile).

### What remains open

The **actual swap**: the grid and the filters still read from the JSON stores; `db_status` is the mirror, not the truth.
The next slice is the JSON → SQLite switch (with import and a measurement comparison).

## 2026-11-19 (seventy-seventh slice) — the database arrives (the first half of the store swap)

The four JSON stores cannot answer any query: every "what is in this container / what expires" question walks the
whole list. The store swap started (ADR-064): **`rusqlite` with `bundled`**, a `user_version = 2` schema, WAL
journaling, and the JSON stores **for now remain the truth** — the `db_status` op mirrors with `sync`, then
reads back the first card and compares the fields.

### Evidence

```
cargo test -p antikolt-core-service     → 20 passed (17 + 3 új adatbázis-teszt)
  a_fresh_database_migrates_to_the_current_schema_and_keeps_it
  a_card_round_trips_with_every_field_the_json_store_has     ← page_text, tags, workspace is
  the_mirror_tables_take_the_containers_and_workspaces_too
cargo clippy -D warnings                → tiszta
```

### An honest note during the slice

A PowerShell splice **again deleted** `main.rs` (0 bytes): a `Replace` expression threw an exception, the
variable stayed empty, and `WriteAllText` wrote it out. The file was **rewritten in full**
from the content saved at the start of the slice (write tool), and the 20 green tests + clippy confirm that every op from today (page_text, container,
workspace, bundle, card_delete, db_status) is in place. The lesson this log records: **in PowerShell
one must never build the result of string building with `Replace` and write it out blindly** — the `write`/`edit` tool
throws an error, the splice deletes silently.

### What remained open

The actual swap: today the shell reads the JSON files, `db_status` is a sidetrack. The next slice's job is
switching the card list and the filters to SQLite, with importing the JSONs and a measurement comparison.

## 2026-11-19 (seventy-sixth slice) — the web address is also editable on the card

After the title and the note, the **web address** too (ADR-063): the meta-row's domain is the handle, double-click → field →
`Enter`/`Esc`, saving via the whole record's `cards.upsert`. The typed address is normalized (`https://` when there is no scheme),
and the displayed domain is recalculated from the **core's answer**. Here too, saving first writes, then restores the
DOM (the lesson of ADR-062).

### Evidence (actual output, twice in a row)

```
antikolt-shell --addcardtest <engine>
add_card=ok typed=submitted before=10 after=11 page=11 error=""
edit_card_title=ok gesture=typed wrote_by=gesture stored="Próba kártya — átnevezve"
edit_card_url=ok gesture=typed stored="https://proba.example/masik-cikk"
delete_card=ok answer=ok remaining=10 expected=10
add_card_check=ok        (exit 0, mindkét futásban)
```

### What remained open

The web-address change **does not reopen** the card (the next open uses the new address), and the frozen text of the old page
(ADR-049) stays the old content until the card is frozen again. The delete confirmation
remains open (the confirmation dialog is the same CEF problem).

## 2026-11-19 (seventy-fifth slice) — renaming the card in place (and an honest gap)

After ADR-061 one could add and remove cards, but not **rewrite their title**. Now one can (ADR-062): double-click on the
title, an input field in its place, `Enter`/`Esc`; saving carries the whole record over `cards.upsert`, so the
container, the tags, the note and the frozen page text are not lost.

### Evidence (actual output)

```
antikolt-shell --addcardtest <engine>
add_card=ok typed=submitted before=10 after=11 page=11 error=""
add_card_record=ok id=01M7… title="Próba kártya — címmel" workspace=w-home
edit_card_title=ok gesture=typed wrote_by=path stored="Próba kártya — átnevezve"
delete_card=ok answer=ok remaining=10 expected=10
add_card_check=ok
```

### The gap this slice revealed — and fixed

The first measurement was `wrote_by=path`, `editor=""`: the **confirmed** write happened on the page path, the editor's save
branch did not run at all. The cause was in my own code: `finish` restored the title **before** the
browser sent `blur`, its `finish("", false)` branch re-entered, and `replaceWith` blew up — the save
was skipped. The fix: **first write, then restore the DOM**, and the restore is in a `try`. Afterwards:

```
edit_card_title=ok gesture=typed wrote_by=gesture
   editor="{"id":"01M7TA9…","title":"Próba kártya — átnevezve"}"
   stored="Próba kártya — átnevezve" ui_error=""
```

This slice therefore provides two things: a working rename, and a lesson — without the `wrote_by`/`editor` pair the
bug would have stayed "green".

## 2026-11-19 (seventy-fourth slice) — the home page is editable: adding and deleting a card

Until now the user could not create a card in the interface — the grid showed the core's cards, a new card could only get in
via the core's `cards.upsert`. Now one can (ADR-061), and delete too.

- **A `＋` button beside the tab row**, an inline form on the grid (`webcím` + optional `cím`), `Enter`/`Esc`. No
  dialog (under CEF `prompt` would silently return `null`, ADR-053).
- **The new card goes to the active tab and the first container**: these are the two facts a form cannot ask
  sensibly. The id is ULID-shaped (`01M7T7…`), when the title is empty it is the domain.
- **`card_delete` in the core** (`CardStore::remove`) + bridge `cards.delete` + page path `antikoltDeleteCard`: delete is the
  pair of add.

### Evidence (actual output, twice in a row, entirely via the interface's gestures)

```
antikolt-shell --addcardtest <engine>
add_card_before=10
add_card=ok typed=submitted before=10 after=11 page=11 error=""
add_card_record=ok id=01M7T7J7A3FHXG6Q80596QQ3YN title="Próba kártya — címmel" url=https://proba.example/hu/cikk workspace=w-home
delete_card=ok answer=ok remaining=10 expected=10
add_card_check=ok        (exit 0, mindkét futásban)
```

### What remained open

Editing a card (rewriting the title/web address/note in the UI) is not there yet, only creation and deletion. The
deletion is immediate, without confirmation; the confirmation dialog is the same CEF problem, so this will be a separate decision.

## 2026-11-19 (seventy-third slice) — the bundle also reaches across the bridge

ADR-059 made the migration ready in the core, but the page did not reach it. Now it reaches it (ADR-060), and it is measured too.

- **Bridge:** `profile.export` / `profile.import`, with a `path` argument. The path is **given by the caller** — the bridge does not
  open a native dialog in the middle of a request (that would stall the page's response).
- **Page:** `window.antikoltProfileWrite(path)` / `window.antikoltProfileRead(path)` — the button and the
  file picker will go here, the bridge stays unchanged.
- **Test:** `--profiletest` opens a **window** after the two headless core pairs, exports through the page, then
  reads it back. It reads the answer from the page's own variable (`window.__profWrite`), because `evaluate` does not wait for
  the promise — the first run actually showed this bug (`write=""`), and the fix shows in the measurement.

### Evidence (actual output, twice in a row)

```
antikolt-shell --profiletest <engine>
profile_export=ok cards=3 source_cards=3 bytes=1842
profile_import=ok exported=3 read_back=3 containers=4 workspaces=3 error=
profile_refused=ok message="unsupported bundle schema: 99 (this core writes 1)"
profile_bridge=ok wrote_cards=10 read_cards=10 bytes=14530
   write="{"cards":10,"containers":4,"path":"…/ui.antikolt.json","schema":1,"sessions":14,"workspaces":3}"
profile_check=ok        (exit 0, mindkét futásban)
```

### What is still open

The **button** and the native file picker: the harness cannot click a dialog, so this step on its own would be
`not-measurable` — but the bridge and the page path are done and measured.

## 2026-11-19 (seventy-second slice) — the profile can be moved in one file

The user's data lived in four separate files; the documentation promised a "move", but there was no way to do it. Now there is
(ADR-059): `profile_export` / `profile_import`, in one JSON bundle (`schema`, `cards`, `containers`, `workspaces`,
`sessions`, `rules`).

- **Core:** `bundle.rs` (atomic write, `schema` check on read) + `replace_all` on all four stores. The
  import **validates first, then writes**: a half-succeeded import is worse than one never started. The imported
  cards go through the same normalization (so the tag rule cannot be bypassed even by import).
- **Test:** `--profiletest` with two throwaway data directories, with two core processes: export → import into a fresh
  profile → read-back from the *file*, plus the rejection of a deliberately "newer" (`schema: 99`) bundle.

### Evidence (actual output, twice in a row)

```
antikolt-shell --profiletest <engine>
profile_export=ok cards=3 source_cards=3 bytes=1842
profile_import=ok exported=3 read_back=3 containers=4 workspaces=3 error=
profile_refused=ok message="unsupported bundle schema: 99 (this core writes 1)"
profile_check=ok        (exit 0, mindkét futásban)
```

### What remained open

The bundle **also contains secrets** (session rows), so protecting the file is the user's business — encryption is
a separate decision. The UI button ("Save to a file" / "Open from a file") is not there yet; the bridge and the core path are done.

## 2026-11-19 (seventy-first slice) — a card's workspace can be switched on the card

After ADR-056/057 the workspace table and the tab-row lifecycle were done, but a card's workspace could not be
moved in the UI. Now it can (ADR-058): in the card's meta row, next to the container selector, a
second, narrower native `<select>` (`.workspace-select`), on the same path: `change` → `setWorkspace` →
`cards.upsert` → the core's answer draws. Not drag: in the virtualized grid, on scroll, the card's shadow host
is rebuilt, and drag is not usable with the keyboard.

### Evidence (actual output, twice in a row, all the way through the UI selector)

```
antikolt-shell --workspacetest <engine>
workspace_move=ok card=01SEED000000000000000001 target=w-home now=w-home answer=changed
workspace_core=ok stored=w-home          ← friss mag-kliens olvassa a fájlt
workspace_restored=ok answer=changed
workspace_used_delete=ok holder=w-work still_there=yes shown="1 kártya még ebben a munkaterületben van"
workspace_deleted=ok remaining=w-home:Otthon w-work:Munka w-read:Olvasnivaló
workspace_check=ok        (exit 0, mindkét futásban)
```

### An honest note

During the slice the test **deleted** the developer profile's "Otthon" workspace: the "used delete" step assumed
the card's *original* workspace, but the card had moved elsewhere in the meantime, so the `×` found an empty tab.
I restored the profile by hand (rewriting the three shipped workspaces), and from now on the test reads the
owner **from the card list**, then checks that after the refusal the tab **is there** — it does not
assume. A test that deletes the user's data is not a test.

## 2026-11-19 (seventieth slice) — the workspace lifecycle on the tab row

ADR-056 put the table into the core, but in the UI a workspace could not be created, renamed, or
deleted. Now it can, with the same three gestures as for containers (ADR-057): `＋` (inline field, `Enter`/
`Esc`), double click on the tab (rename), `×` (delete) — without a dialog.

- **Page:** `commitWorkspaceEdit` / `deleteWorkspace` / `reloadWorkspaces`; the new id is a slug derived from the name
  (`w-proba-ter`); the `in_use` answer to deleting a used workspace **appears** on the tab row.
- **Witness:** `window.antikoltWorkspaceState()` now also reports the editor's state and the UI error
  (`editing`, `uiError`), and the tab witness also reports the `removable` field.

### Evidence (actual output, twice in a row)

```
antikolt-shell --workspacetest <engine>
workspace_table=ok / workspace_cards=ok / workspace_move=ok / workspace_core=ok / workspace_restored=ok
workspace_created=ok id=w-proba-ter answer=submitted     ← a ＋ gomb + soros mező
workspace_renamed=ok                                     ← dupla kattintás + mentés
workspace_used_delete=ok shown="1 kártya még ebben a munkaterületben van"
workspace_deleted=ok remaining=w-home:Otthon w-work:Munka w-read:Olvasnivaló
workspace_check=ok        (exit 0, mindkét futásban)
```

### Gate

`pwsh -File scripts/verify.ps1` → **ALL SELF TESTS PASSED** (38 self-tests + `check-repo`).

## 2026-11-19 (sixty-ninth slice) — the workspaces are real, the "thirteenth card" bug fixed

The page guessed the workspace from the card's *position* (`index > 12 ? "w-read" : "w-home"`): every thirteenth
card silently landed on the "Olvasnivaló" tab. This was not an approximation but a bug — now the workspace is stored data
(ADR-056).

- **Core:** `workspaces.json` + `workspaces_list`/`workspace_upsert`/`workspace_delete` (with the three shipped
  workspaces), and a `Card.workspace` field. The empty field means the first workspace: a missing field does not
  move a card to a tab the user did not choose.
- **Shell:** `workspaces.list/upsert/delete` on the bridge, and the table goes to the page **by push**
  (`window.antikoltAcceptWorkspaces`), for the same reason as the container table (ADR-052).
- **Page:** `pageWorkspaces` is the core's table, the tab row is built from it; the card's `workspaceId` comes from the record; new
  witness: `window.antikoltWorkspaceState()`, and the product's own write path: `antikoltMoveToWorkspace`.

### Evidence (actual output, twice in a row)

```
antikolt-shell --workspacetest <engine>
workspace_table=ok core=w-home:Otthon w-work:Munka w-read:Olvasnivaló
                     page=…ugyanaz… tabs=…ugyanaz…
workspace_cards=ok count=10 first=w-home        ← nincs kitalált munkaterület
workspace_move=ok card=01SEED000000000000000001 target=w-work now=w-work answer=ok
workspace_core=ok stored=w-work                  ← friss mag-kliens olvassa a fájlt
workspace_restored=ok                            ← a teszt visszaállítja
workspace_check=ok        (exit 0, mindkét futásban)
```

### Gate

`pwsh -File scripts/verify.ps1` → **ALL SELF TESTS PASSED** (38 self-tests + `check-repo`, 16 Rust tests).

## 2026-11-19 (sixty-eighth slice) — tags: the search field's promise came true

The search field's placeholder had long promised a "tag", but the card record had no such field — the page's
`matchesTerm` checked a `card.tags` array that never existed. Now there is one (ADR-055).

- **Core:** `Card.tags: Vec<String>` (`#[serde(default)]`, the old `cards.json` loads without a problem), and the
  **normalization in the store**: trim, case-insensitive dedup (the first spelling stays), max. 12 tags,
  each max. 32 characters. Truncating to length is not a bug but data preservation.
- **Page:** chips on the card, next to them `＋tag` (an inline-opening field, `Enter`/`Esc`), `×` on the chip. Writing goes on
  the usual `cards.upsert` path, the grid shows the core's answer; new witness: `window.antikoltTagState()`.
- **Search:** tags go into the same haystack as the title/web address/note/page text, so the accent-insensitive
  fold also applies to them (`kavezo` → `kávézó`).

### Evidence (actual output, twice in a row)

```
antikolt-shell --tagtest <engine>
tag_card=01SEED000000000000000001
tag_add=ok typed=typed page=kávézó card=kávézó        ← a kártya saját szerkesztője
tag_core=ok stored=kávézó                             ← friss mag-kliens olvassa a fájlt
tag_search=ok filtered=2 titles=Antikolt projekt | Kávézó-próba — Árvíztűrő Tükörfúrógép term=kavezo
tag_remove=ok remaining=                              ← a chip ×-e kitakarít
tag_check=ok        (exit 0, mindkét futásban)
```

In the search the second hit is the test card left in the profile: the claim is therefore that **the tagged
card is among the hits** (and the fold works), not that there is exactly one hit — the profile is not a
fixture.

### An honest note during the slice

While editing the tag slice, a PowerShell splice **deleted** `crates/antikolt-core-service/src/store.rs`
(the file shrank to 2 bytes). No data was lost in the store: the file was rebuilt from the `git show HEAD:…` version and the
day's earlier slices (page text, `move_container`, tags), then **13 store tests** and the
full gate verified it. The lesson concerns the work process: the line-by-line splice, in two places, silently
forgot a `#[test]` attribute — the strict `clippy -D warnings` caught it, not the eye.

### Gate

`pwsh -File scripts/verify.ps1` → **ALL SELF TESTS PASSED** (37 self-tests + `check-repo`, 13 store tests).

## 2026-11-19 (sixty-seventh slice) — a used container can also be deleted, but only by moving

ADR-052 deliberately refused to delete a used container (`in_use`) so that nothing disappears silently —
except that this was a dead end: the user could not get rid of their old grouping. Now there is a way out (ADR-054).

- **Core:** `container_delete` with an optional `move_to`. If present, it **first** moves (`CardStore::move_container`,
  in one save), **then** deletes, and the answer says how many cards moved. If absent, the `in_use`
  error stays; if the target does not exist or is the deleted container itself, `invalid_argument`/`not_found`.
- **The order is deliberate:** the move is written out first. If the deletion fails, the cards are still somewhere.
- **UI:** after the `×` the `in_use` answer is not an error message but an **offer**: the legend opens a line
  ("1 card is still in this container"), with a selector for the other containers and a "Move and delete" button.

### Evidence (actual output, twice in a row)

```
antikolt-shell --containertest <engine>
container_created=ok id=c-proba answer=submitted
container_reload=ok chips=c-work c-private c-money c-dev c-proba
container_rename=ok page=c-proba:Próba 2
container_used_delete=ok shown="1 kártya még ebben a konténerben van" move=offered
container_move_delete=ok container_gone=yes card_in=c-private
container_switch=ok        (exit 0, mindkét futásban)
```

The test puts the card into the test container with the **card's own selector**, so that the refusal is real,
then moves and deletes it through the line — finally it puts the card back into its original container.

### Gate

`pwsh -File scripts/verify.ps1` → **ALL SELF TESTS PASSED** (36 self-tests + `check-repo`, 12 store tests).

## 2026-11-19 (sixty-sixth slice) — the container lifecycle is complete in the UI

ADR-052 put the table into the core, but in the UI a container could not be created, renamed, or deleted.
Now it can (ADR-053), without a dialog:

- **`＋ container`** at the end of the legend: an input field appears, `Enter` saves, `Esc` cancels. The new id is the
  slug derived from the name (`c-proba`), the core generates the color.
- **Rename**: double-clicking the chip opens the same field, pre-filled with the name.
- **Delete with `×`**: if the container still has cards, the core's `in_use` answer **appears** in the legend
  ("1 card is still in this container") — we do not swallow it.
- **Why there is no `prompt`**: for the CEF-side JS dialog the host would have to provide a `CefJSDialogHandler`, which
  is not there; `prompt` would silently return `null`, and the UI would "do nothing". The inline editing form avoids this,
  and it is also usable with the keyboard.

### Evidence (actual output, twice in a row, all the way through the UI path)

```
antikolt-shell --containertest <engine>
container_created=ok id=c-proba answer=submitted   ← a ＋ gomb + a soros mező
container_reload=ok chips=c-work c-private c-money c-dev c-proba
container_rename=ok page=c-proba:Próba 2           ← dupla kattintás + mentés
container_used_delete=ok shown=1 kártya még ebben a konténerben van
container_deleted=ok remaining=c-work:Munka c-private:Magán c-money:Pénz c-dev:Fejlesztés
container_switch=ok        (exit 0, mindkét futásban)
```

The test **does not call the core directly**: it clicks, types, `submit`s — and then, with a fresh core client,
checks that the file really contains what the UI shows.

### Gate

`pwsh -File scripts/verify.ps1` → **ALL SELF TESTS PASSED** (36 self-tests + `check-repo`).

## 2026-11-19 (sixty-fifth slice) — the container table moved into the core

Until now the containers' name and color lived in the page, in a constant; the core only stored an id on the card. Now the
table is the core's (ADR-052), and the page draws what the core says.

- **Core:** `containers.rs` + `containers.json` next to the cards; `containers_list`, `container_upsert`,
  `container_delete`. The first start seeds the four shipped containers (the old profile loses nothing).
  The color is `#RRGGBB`; a missing/bad color is generated deterministically (same id → same color).
  Deletion only when no card uses it: `in_use` error — a silent move is the "my cards disappeared" error
  class.
- **Shell:** `containers.list` / `containers.upsert` / `containers.delete` on the bridge, and the table goes **by push**
  to the page with the card list (`window.antikoltAcceptContainers`).
- **Page:** `pageContainers` is the core's table (the constant is only a fallback for the browser), and a new witness:
  `window.antikoltContainerTable()`.

### Two real bugs the measurement caught

1. **The bridge installs later than the page's script.** The page's `containers.list` request got a `mock` table (the
   bridge did not exist yet), so the legend drew four containers while the core had five. The fix: push with the
   cards, not a request.
2. **The shell's core is long-lived, the table lives in its memory.** The first version of the test wrote the file
   with a separate core process, which the shell's core did not see — the measurement measured our own copy, not the product. The fix: the
   test writes **through the page** (on the product's path), and reads the file back with a fresh core client.

### Evidence (actual output, twice in a row)

```
antikolt-shell --containertest <engine>
container_table=ok core=c-work:Munka c-private:Magán c-money:Pénz c-dev:Fejlesztés
                     page=c-work:Munka c-private:Magán c-money:Pénz c-dev:Fejlesztés
container_created=ok answer={"container":{"color":"#34B26A","id":"c-proba","name":"Próba"}}
container_reload_load=ok
container_reload=ok chips=… c-proba:Próba 0        ← a lap a mag tábláját rajzolja
container_rename=ok page=c-proba:Próba 2
container_used_delete=ok answer=error:1 kártya még ebben a konténerben van
container_deleted=ok answer={"removed":true}        ← a teszt kitakarít maga után
container_switch=ok        (exit 0, mindkét futásban)
```

### Gate

`pwsh -File scripts/verify.ps1` → **ALL SELF TESTS PASSED** (36 self-tests + `check-repo`, with 11 store tests inside).

## 2026-11-19 (sixty-fourth slice) — containers have a name, a color, and a filter

Until now the container was a colored dot on the card, without explanation. Now a **chip row** stands above the grid:
per container a colored dot, a name, and the **count according to the current search**; the chip itself is the filter (ADR-051).

- **Page:** `renderContainerLegend()` runs inside `applyFilter()`, so the numbers refresh together with the search;
  the filtering kicks in *after* the search. One container is active at a time, another click clears it; the chips
  signal the state with `aria-pressed`.
- **The filter is view state**, not a stored setting: a new start shows everything (a forgotten filter is a trap).
- **Witness:** `window.antikoltContainerState()` now outputs the filter, the filtered count, and the chips' state as
  well.

### Evidence (actual output, twice in a row)

```
antikolt-shell --containertest <engine>
container_card=01SEED000000000000000001 from=c-work
container_target=c-private
container_page=ok last=c-private error=
container_core=ok stored=c-private
container_restored=ok
container_legend=ok chip=Munka 1          ← a chip színes pontja + neve + a keresés szerinti szám
container_filter=ok filter=c-work filtered=1
container_filter_cleared=ok filtered=10   ← a második kattintás visszaadja a teljes listát
container_switch=ok        (exit 0, mindkét futásban)
```

### Gate

`pwsh -File scripts/verify.ps1` → **ALL SELF TESTS PASSED** (36 self-tests + `check-repo`).

## 2026-11-19 (sixty-third slice) — a card can be moved to another container

The roadmap's "card-container binding, container switching" item only *appeared* until now: there was a colored dot on the card, but
the binding could not be changed. Now a native `<select>` stands in the card's meta row, and the choice goes through the
core (ADR-050).

- **Page:** the card sends an `antikolt-container` event, the grid's `setContainer` writes with `cards.upsert`, and
  updates the model from the answer (the view holds a container object, the record holds an id). New witness:
  `window.antikoltContainerState()`.
- **Why a native select:** the keyboard, the screen reader, and the platform menu come for free; the colored dot
  stays next to it, and the menu also shows the container's **name** (the dot alone cannot be read out).

### Evidence (actual output, twice in a row)

```
antikolt-shell --containertest <engine>
home_surface=1
container_dom={"cards":10,"first":1}
container_card=01SEED000000000000000001 from=c-work
container_target=c-private
container_page=ok last=c-private error=
container_core=ok stored=c-private          ← friss mag-kliens olvassa vissza
container_restored=ok                       ← a teszt visszaállítja az eredeti konténert
container_switch=ok        (exit 0, mindkét futásban)
```

Three witnesses agree: the page's state after the switch, the core's record (read with a fresh client), and the model's
container map. At the end the test **restores** the original container so as not to rearrange the user's
cards.

### Gate

`pwsh -File scripts/verify.ps1` → **ALL SELF TESTS PASSED** (36 self-tests + `check-repo`).

## 2026-11-19 (sixty-second slice) — the note's focus: measured, and a real bug fixed

`19-FOLYTATAS.md` §3 left two things open: measuring **focus return** was "not possible", and measuring
the **rendered note** is missing. Both are done; along the way it turned out that not only the measurement was wrong.

1. **The probe asked the wrong element.** The old `note_focus` looked for the active element in the *first* card's shadow root,
   not in the one that owns the focus — it measured a working feature as `none`.
2. **Focus return could indeed be lost.** The page released the refocusing intent with a 2-second timer,
   but the core sends a second re-render, which can also arrive later; if the timer expired first, the focus fell
   on `body` (measured: `note_focus=not-host:BODY`). Fix: the identifier stays until the **next user event**,
   and the grid also returns the focus **synchronously after** the render (a background window does not
   necessarily receive an animation frame).
3. **The rendered note** probe now reads the card's `.card-notes` element by polling after the state answer.

### Evidence (actual output, twice in a row)

```
antikolt-shell --notetest <engine>
note_save=ok
note_rendered=probe note
note_focus_chain={"hasFocus":true,"activeTag":"ANTIKOLT-CARD","activeIsHost":true,"shadowActive":"card"}
note_id=01SEED000000000000000001
note_page=probe note
note_model=probe note
note_core=probe note
note_error=
note_focus=01SEED000000000000000001:card
note_write=ok
note_rendered_ok=ok
note_focus_ok=ok        (exit 0, mindkét futásban)
```

From now on `note_write=ok` asks for the **four** witnesses together: the editor's answer, the grid's model, the core's
record, and the rendered note — plus the focus.

### Gate

`pwsh -File scripts/verify.ps1` → **ALL SELF TESTS PASSED** (35 self-tests + `check-repo`).

## 2026-11-19 (sixty-first slice) — the page text is searchable too

ADR-047 revoked the page-text promise, because nothing indexed the articles' text. Now there is something to do it with:
the freeze measures the page's text, the core writes it into the card record, and the search sees it in all three layers
(ADR-049).

- **Core:** the card record's new `page_text` field (serde-default, so old stores stay readable unchanged),
  the `card_page_text` op (`{id_of_card, text}`), `set_page_text` + cap (16 000 characters, whitespace collapsed).
  `card_upsert` keeps the previous text if the incoming one is empty — the pin and the note cannot delete the capture.
- **Shell:** `freezeCard`, **before** discarding the surface, asks the page for its `document.body.innerText`
  (cut to 8000 characters), and saves it with `card_page_text`; the `cards.search` haystack also folds
  `page_text`.
- **Page:** the card model gets a `pageText` field, and `matchesTerm` also looks at it — also without accents.

### Evidence (actual output)

```
cargo test -p antikolt-core-service
test result: ok. 8 passed; …            ← új: page_text_is_captured_kept_and_capped

antikolt-shell --searchtest <engine>
search_page_text_written=ok            ← a mag `card_page_text` opja elfogadta
search_seeded=ok
search_fold=ok folded=arvizturo tukorfurogep lodz
search_total=10
search_accent=ok term=kavezo filtered=1 of=10
search_empty=ok empty_text=Nincs találat erre: „zzzz”. Próbálj kevesebb szót, vagy töröld a keresést.
search_page_text=ok filtered=1         ← a „rejtettoldalszoveg” szó csak a rögzített szövegben van benne
search_escape=ok filtered=10
search_check=ok        (exit 0)
```

The store test also measures what the UI cannot show: the whitespace collapsing, the cap, the unknown id's
answer, and that a **pin does not delete** the captured text.

### What this slice does not promise

- A live (non-frozen) card has no page text: the capture is made at the moment of freezing.
- The text is the state at freeze time; if the page changes later, the search sees the old one — this is deliberate (the
  card's essence is that it preserves the state on disk).
- The store remains a JSON document; the SQLite schema (`0001_init.sql`) does not know the field yet, because that is the next
  big step (replacing the store), and until then the JSON is the truth.

### Gate

`pwsh -File scripts/verify.ps1` → **ALL SELF TESTS PASSED** (35 self-tests + `check-repo`, with `cargo fmt`,
`clippy -D warnings`, 8 store tests).

## 2026-11-19 (sixtieth slice) — 500 cards measured, and the roadmap's checkmarks tied to reality

`04-UTEMTERV` promised: "Virtualized grid with 500 cards" — and until now nobody measured it. A new self-test
(`--gridscaletest`) and a document sync.

### The measurement

The test works in its **own core directory** (`ANTIKOLT_CORE_DATA`), so no 500 test cards land in the developer
profile — this new environment switch was made specifically for this. It writes 500 cards through the core, opens the
grid, and measures from the page's own state:

```
antikolt-shell --gridscaletest <engine>
scale_dir=C:/Users/info/AppData/Local/Temp/antikolt-scale-11696/core
scale_seeded=ok written=500
home_surface=1
scale_render_ms=20
scale_cards=503                     (500 + a mag 3 demókártyája)
grid_scale_count=ok
grid_scale_rendered=ok rendered=13 maxIndex=12     ← 13 DOM-elem 503 kártyából
grid_scale_scroll=not-measurable (the page had no layout height in this harness)
grid_scale=ok        (exit 0)
```

The **13 rendered elements** are the essence of the claim: the grid does not put 503 elements into the DOM, but one screenful, and
the rest live in the `#spacer`'s height. `scale_render_ms=20` is the time measured to the first render.

**What I did not measure, and I say so:** the *scroll* jump ("the 13 elements also reach the end of the page") cannot be
measured in this harness, because the embedded page has no layout height (`innerHeight=0` — the shell sizes the card
area, and the test window's surface width is 0). The test therefore writes `not-measurable`, not green.
Scrolling is otherwise already proven on the `--gridtest`/`--scrollzoomtest` paths, just not on 500 cards.

### Roadmap sync

`04-UTEMTERV`'s checkmarks are now tied to **evidence**: next to every checkmark is the name of the runnable self-test,
and for partial items the open part (e.g. for search, the page text). New checkmarks: card lifecycle,
MHTML+meta+preview, the 10-minute rule, the 500-card grid, search, session panel + ledger, keep/sweep/wipe,
four themes + WCAG. The "Download manager, Ctrl+F, PDF, PiP, printing" row changed only in that **zoom** is done,
the rest is open.

### Gate

`pwsh -File scripts/verify.ps1` → **ALL SELF TESTS PASSED** (35 self-tests + `check-repo`).

## 2026-11-19 (fifty-ninth slice) — the cleanup does not silence the playing card

`19-FOLYTATAS.md` §2's open item: hibernation respects the card playing media, but the **session cleanup
does not** — a due `wipe` could also silence a video playing in the foreground. Fixed
(ADR-048).

- **Shared script**: the media question (`video`/`audio`, `!paused && !ended`) moved to one place
  (`LiveMediaPlayingScript`), and besides hibernation, the cleanup also uses it.
- **The decision is the page's**: if the answer is `playing`, the **live** origin's deletion is skipped, the status bar says why;
  if the page does not answer (error), the cleanup runs the old way.
- **The structure**: the cycle now only *selects* the live origin, the deletion (and closing the ledger) happens after
  the page's answer — the non-live origins still wait for the next opening, unchanged.

### Evidence (actual output)

```
antikolt-shell --mediacleantest <engine>
origin=http://127.0.0.1:52723
session_written=ok
grid_loaded=ok loads=about:blank, file:///…/home/index.html, about:blank, http://127.0.0.1:52723/
card_surface=2
media_playing=unavailable (no audio device on this machine) storage_before=1|probe=1
media_probe={"paused":true,"ended":false,"readyState":0,"networkState":2,"muted":true,
             "currentSrc":"http://127.0.0.1:52723/tone.wav","duration":null,"error":0}
media_skip=not-measurable (no audio device: the page never starts its player)
media_wipe_when_idle=ok status=takarítás: 1 azonnal, 14 a kártya következő megnyitásakor
media_cleanup_check=ok        (exit 0)
```

**What this log says:** there is no audio device on this machine, so Chromium does not even start the player
(`readyState=0`, and the `/tone.wav` request **did not even start** — according to the probe server's log, only `/` and
`/favicon.ico` came in). The test says this (`not-measurable`) instead of lying green: what it measured is
that the cleanup runs **on the new, media-aware path**, and without `playing` the deletion happens. On a machine with an audio device
the same test also measures the `playing` branch.

### A lesson about the probe server

You must not wait for `loadFinished` for the card to open: a page containing a media element can have a delayed `on_load_end`.
The test therefore reads the surface from `window.liveCard()->surface()`. And the probe server should answer even when
the request arrived before the socket was handed over (in that case `readyRead` does not signal).

### Two bugs the gate caught (not the user)

1. **`metricstest` fell out of the command parser.** One of the text-block replacements during the slice removed the
   `parser.addOption(metricstest)` line, and the run stopped with `Unknown option 'metricstest'` — the gate signaled
   it immediately. This kind of bug (a test silently not running) is exactly why the gate lists thirty-three lines,
   and does not just write one aggregated green.
2. **`sessionpanel` compared against a magic number (`rows == 1`).** In the developer profile, earlier tests'
   due session rows piled up, and the panel test failed because of it — that is, it was about the *profile*, not the
   panel. Fixed: it compares the panel's row count to the due rows reported by the core
   (`panel_due_rows`), the content claims are unchanged. And `--mediacleantest` closes its own row at the end
   so as not to leave litter.

### Gate

`pwsh -File scripts/verify.ps1` → **ALL SELF TESTS PASSED** (33 self-tests + `check-repo`).

## 2026-11-19 (fifty-eighth slice) — the search adapts to the user's typing

Until now the grid's search compared with `toLowerCase()`: `kave` did **not** find the `Kávézó` card. The
placeholder besides that promised "page text", which is not indexed. Both fixed (ADR-047).

- **Accent-insensitive fold**, on the page (`foldSearch`) and in the shell (`FoldForSearch`, in the `cards.search` op):
  NFD decomposition + removing combining marks, plus manual mapping of the decomposition-stable letters (`ł`, `ø`, `đ`, `ß`).
- **The empty result names the searched term**: "No results for: 'zzzz'. Try fewer words, or clear the search." —
  the earlier silent emptiness suggested that the page had broken.
- **The placeholder does not promise page text**: "title, web address, note, tag (also without accents)". The page-text
  search remains §12's open item; until then the UI does not claim it exists.

### Evidence (actual output)

```
antikolt-shell --searchtest <engine>
core_executable=…/target/debug/antikolt-core-service.exe
search_seeded=ok
home_surface=1
search_fold=ok folded=arvizturo tukorfurogep lodz
search_total=10
search_accent=ok term=kavezo filtered=1 of=10
search_empty=ok empty_text=Nincs találat erre: „zzzz”. Próbálj kevesebb szót, vagy töröld a keresést.
search_focused=no                                        (a teszt ablaka nem előtérben; tájékoztató sor)
search_escape=ok filtered=10
search_check=ok        (exit 0)
```

The test raises the searched card **through the core** (`Kávézó-próba — Árvíztűrő Tükörfúrógép`), so it does not
depend on what is in the profile right now; the `kavezo` term, without accents, yields a single card. The Escape step
's `filtered=10` after it is the starting grid: the search does not stay "stuck".

### Gate

`pwsh -File scripts/verify.ps1` → **ALL SELF TESTS PASSED** (32 self-tests + `check-repo`).

## 2026-11-19 (fifty-seventh slice) — the shell's log file: the console that is not there

After discarding the console window (slice 55), the shortcut launch had no readable output left: the engine's
GPU errors and every `[shell] …` line went into the void. The fix is a log file in the profile
(`%APPDATA%\Antikolt Project\Antikolt\logs\shell.log`), with a timestamp, one-generation rotation
(`shell.log.1`, at 1 MB), and **only** for the plain launch: a program run from a terminal already has a stderr, and
the self-tests must not write into the user's log.

Log writing goes through the **existing** message handler (`ForwardQtMessages`), so it is not a new logging layer: every line
that used to go to stderr — the engine's redirected output too — is the same text, only with a timestamp and also on disk.
The limit is adjustable with an environment switch (`ANTIKOLT_SHELL_LOG_LIMIT`), so the test rotates with a few kilobytes,
not with a megabyte.

### Evidence (actual output)

```
antikolt-shell --logtest
log_path=C:/Users/info/AppData/Local/Temp/antikolt-logtest-20532/shell.log
log_lines=3
log_has_lines=ok
log_rotated=ok current=1098 previous=1952
log_stopped=ok
log_check=ok        (exit 0)
```

Live launch (shortcut mode, empty command line), then the log read back:

```
log_exists=True  log_bytes=4483  lines_after=70
2026-11-19T01:31:32.689 [shell] info: [shell] tray: ikon a tálcán (a program ikonjával)
2026-11-19T01:31:35.873 [shell] info: [engine] [engine-host] previews: antikolt://blob/ from …\engine\previews
```

### Gate

`pwsh -File scripts/verify.ps1` → **ALL SELF TESTS PASSED** (31 self-tests + `check-repo`).

## 2026-11-18 (fifty-sixth slice) — the pages also have several dark levels, and the theme switcher is an icon

The user's clarification: "there will also be several dark modes for the pages, not only for the browser", and they asked
that the theme buttons be **icon + tooltip**. Both are done (ADR-045).

### The four levels of page darkening

The `set_forced_dark` op now gets a `mode` field (`off`, `invert`, `dim`, `warm`); the old `enabled` boolean field
also works backwards (`true` → `invert`). The engine swaps or removes the same **named** style element,
so changing the level happens without reloading. The chosen level is kept
(`ui/page_dark_mode`), and the old `ui/forced_dark` key is switched over then deleted on the first start.

### The control bar: icons, tooltips

Instead of the four labels (`light grey dark amoled`), four **generated icons** (the theme's background + text color, 20 px,
drawn in code, no new asset), the word in the tooltip and the screen-reader name. The page-darkening
crescent icon is selectable from a menu, and its **fill** shows the active level.

### Evidence (actual output)

```
antikolt-shell --themetest <engine>
theme_chrome=light luma=244
theme_chrome=grey luma=58
theme_chrome=dark luma=40
theme_chrome=amoled luma=13
theme_contrast=ok
theme_button=light painted=85% ok        (mind a négy ikon 85%-ban fest, és négy különböző kép)
theme_icons=ok
theme_wcag=light  text=16.2 muted=6.0 surface=17.4 ok
theme_wcag=grey   text=11.2 muted=5.6 surface=10.0 ok
theme_wcag=dark   text=13.9 muted=6.1 surface=12.7 ok
theme_wcag=amoled text=17.9 muted=7.5 surface=16.9 ok
theme_wcag_aa=ok
grid_capture=light luma=240 → grid_capture=amoled luma=50, grid_theme=ok
dark_capture=mode_off    luma=251
dark_capture=mode_invert luma=4
dark_capture=mode_dim    luma=39
dark_capture=mode_warm   luma=33
dark_page_modes=ok
dark_mode_persisted=ok
theme_check=ok        (exit 0)
```

Two new measurements went into the test, because until now we only *claimed* them: (1) the theme buttons' icon **really paints**
(the ratio of visible pixels, and that the four icons are four different images — four identical crescents would not pass);
(2) `08-DIZAJN.md` §1's promise of WCAG AA contrast is **computed** for every theme (visible above the log:
the smallest value is 5.6:1, the threshold is 4.5).

### An environmental pitfall the test no longer swallows

The first run stopped with `CefInitialize failed`, because a **previous, interrupted run's** engine processes were still
alive (`antikolt-shell` + several `antikolt-engine-host`, from 17:53). The tests close the processes at their end;
the lesson stays in the log: "all green" is only true if there is no junk process beforehand.

### Gate

`pwsh -File scripts/verify.ps1` → **ALL SELF TESTS PASSED** (30 self-tests + `check-repo`).

## 2026-11-18 (fifty-fifth slice) — one window, and the dark pages

In the program launched from the Start menu, the user saw **two windows**: the browser and a black
console window with the engine's log, with the GPU errors in it. This rule was also born: "it is strictly forbidden
to open any unjustified extra window, and especially to keep it alive".

### The extra window: the console subsystem

Until now the shell was a console-subsystem exe, and Windows gives it its own console window for the shortcut
launch (measured evidence: among the running process's windows, `ConsoleWindowClass`, visibly). The fix
(`DropOwnConsoleWindow`): if only our process is attached to the console, then that console is ours — we
discard it (`FreeConsole`); if a shell is also there (every self-test), then it is the shell's, and it stays. The GUI subsystem
(`WIN32_EXECUTABLE`) was tried and **rejected**: PowerShell does not wait for a GUI program and does not return its
output (measured: `& $shell --traytest` returned immediately, with empty output and an exit code), which would have silently
disabled the entire gate — the harness has to learn this first.

### The dark modes: the UI is dark, the article is white

The user's image (amoled theme) measured: the control bar pixel is `#0A0A0A`, the status bar `#000000` — so the
**theme worked**; the Wikipedia article stayed white, because ADR-037 only *tells* the page that we want dark,
and the article has no dark theme of its own. Two bugs and one missing feature came out of this:

1. **The grid did not follow the control bar** (it had its own `data-theme` switch), and the shell push also **lagged by
   one click** (`currentTheme_` was set *after* the push). Fixed: the control bar's decision drives the grid,
   on theme change and on every grid load.
2. **Until now the self-test only read the stylesheet's text** — the user found the difference between the
   text and the screen. New `--themetest`: it renders, and measures pixels.
3. **New feature (ADR-044): "🌙 Dark pages"** switch (off by default, remembered). The engine's `set_forced_dark`
   op inserts a **named** style element into the page (inverts + re-inverts the images), and restores by removing
   the same — without reloading.

### Evidence (actual output)

```
antikolt-shell --themetest <engine>
theme_chrome=light luma=241
theme_chrome=grey luma=60
theme_chrome=dark luma=42
theme_chrome=amoled luma=15
theme_contrast=ok
grid_surface=1
grid_theme_attr=amoled
grid_capture=light luma=239
grid_capture=amoled luma=68
grid_theme=ok
dark_surface=2
dark_capture=no-force luma=251
dark_capture=forced luma=13
forced_dark_pages=ok
forced_dark_persisted=ok
theme_check=ok        (exit 0)
```

The `theme_chrome` lines are the **average** brightness of the control bar in the four themes (the palette order: 241 ≫ 60 > 42
> 15), `grid_capture` is the product's own grid page (239 → 68), and `dark_capture` is a white
test page with the toggle off/on (251 → 13). At the end the test **turns off** the toggle: a self-test must not
change the user's setting, and the `forced_dark_persisted=ok` line proves that a
second window (the next launch) reads the same value back from `QSettings`.

### The rule is fixed

`docs/HU/08-DIZAJN.md` §8–9 and `docs/HU/15-HOZZAJARULAS.md` §4: **no unjustified extra window, and none may
stay alive**; the engine and core processes talk over hidden pipes, the tray icon is a signal and not a
window, the program throws away its own console window. This is a review question for every change that creates a window
or a process.

### Gate

`pwsh -File scripts/verify.ps1` → **ALL SELF TESTS PASSED** (30 self-tests, including `themetest ok`, + `check-repo`).

## 2026-11-18 (fifty-fourth slice) — a tray icon that is also the running indicator, and one user = one Antikolt

The request was literally this: "put the launcher icon on the tray, the one that is also the running icon". Two things were missing
for it: the icon, and that the shortcut should not start a second browser. Both are done (ADR-043).

**Single-instance guard** (`instance_guard.h/.cpp`): before launching, the shell reserves a named local socket
(`antikolt-shell-<user>`); if someone already answers it, it asks the running window to come forward, and
**exits without a window, with 0** — without the engine, the profile and ~150 MB. The order is not accidental: first
you have to *connect*, then reserve, because on Windows two processes can both create a pipe instance for the same
name (I measured it: the variant built purely on `listen()` became primary even as the second one).

**Tray icon** (`MainWindow::setupTray`): the program's icon (the same one the shortcut shows), an "Antikolt —
running" tooltip, a click toggles the window in and out, its menu is "Open" and "Quit". The close button puts the window on the tray,
and says so once; the real exit is the menu's "Quit" item. In a session without a tray
everything is as before (close = quit); the other self-tests run in this mode
(`ANTIKOLT_NO_TRAY`), only `--traytest` turns the tray on.

### Evidence (actual output)

```
antikolt-shell --traytest
instance_primary=ok
instance_second=instance=secondary exit=0
instance_raised=ok
tray_icon=ok
tray_menu=Megnyitás|Kilépés
close_returns=hidden
tray_hides=ok
tray_menu_items=ok
tray_restores=ok
tray_quit=ok
fallback_tray=absent close=closed
tray_and_instance=ok        (exit 0, három futásból három zöld)
```

The `instance_second` line is the answer of a **real second process**: the test launches the same exe
with `--ping-instance`, just as the shortcut would. The `fallback_tray=absent close=closed` line proves the
no-tray branch: there, close really closes.

Live trial with the finished program: the first launch runs (engine, window), the **second launch is `exit=0`** and the
log says `már fut egy Antikolt: a meglévő ablak előhozva, ez az indítás kilép` — no second engine started.

### What the measurement shows (and what the user needs to know)

A window put on the tray **does not give memory back**: the engine and the grid live until "Quit" happens. An open
card, however, hibernates on its own under the 10-minute rule, so a browser waiting on the tray also releases the
card's memory (a consequence of ADR-043).

### The portable instance updated

`dist\Antikolt` and the Start menu/desktop shortcut were still running the **yesterday** build; at the end of the slice
`scripts/dist.ps1 -Release` rebuilt it, and `README.txt` now tells the truth too (the engine starts on its own, the
close puts it on the tray, the second launch brings the running window forward).

### Live trial with the finished program (and the bug that only this revealed)

`--traytest` was green, but at a **real** launch the tray did not appear. Cause: the condition for the two switches
(`ANTIKOLT_NO_TRAY`, `ANTIKOLT_NO_PREVIEW_WORKER`) was "this is not that test", which is also true for a *plain launch*
— so in the shipped browser both the tray **and** the background-preview queue were switched off.
The fix: the switches only take effect when the program **named a self-test** on the command line
(`QCoreApplication::arguments().size() > 1`); a plain launch gets the tray and the queue. Measured with the real exe:

```
hwnd=21828818 visible=True
after_close: alive=True window_exists=True window_visible=False     # WM_CLOSE → tálcára
ping_exit=0 (instance=secondary)
after_ping: alive=True window_visible=True                          # a parancsikon előhozta
```

The same in the log: `tray: ikon a tálcán (a program ikonjával)`, then the queue works on its own
(`preview worker: preview-worker-abort … kép a profilban`).

### An environmental flake, fixed

Once in the full gate `capturetest=FAILED` happened (while the GPU process was restarting, the
`Page.captureScreenshot` answer did not arrive). The product retries in such cases (`kCefCaptureAttempts`), so the
test does too: two retries, each asking for a new frame with a fresh `navigate` (`capture_retry=`).
After that 3/3 green.

### Gate

`pwsh -File scripts/verify.ps1` → **ALL SELF TESTS PASSED** (29 self-tests, including `traytest ok`, + `check-repo`).

## 2026-11-17 (fifty-third slice) — the background-preview worker, and what the second surface brought to light

Slice 52 gave the guards; this slice gives the **execution**: the worker lives in `MainWindow`, and the engine house
learned what it had been missing for it.

**What the worker is** (`main_window.cpp`, with the guards of `previews.h`): `startPreviewWorker(id, url)` →
`previewWorkerCanStart` → a hidden surface on the `persist:preview` partition → `navigate` → `loadFinished`
(filtered by the *worker's* id) → `capture` (png, 80) → moving the image under `previews/<id>.png` →
`previewConsumeBudget` → `cardUpsert` with the `preview_ref` → `destroySurface` → `Idle`. It is aborted by
a card opening, leaving the grid and the deadline; the queue starts the next card 1.2 s after the worker.
The deadline can be set with `ANTIKOLT_PREVIEW_DEADLINE_MS` (like the hibernation clock), so that the test can measure it.

**What the engine house is** (ADR-042): until now there was a single `HostState`, and the `surface` field was only checked by the wire
— every command ran on the grid. Now there is per-surface state (the `create_surface`
id is the key), `WM_CLOSE` closes its own surface, `bridge_reply` looks for the request's owner,
an unknown surface gets `surface_not_found` as the answer, the third surface is refused with `engine_busy`, and the
`hidden` flag can be given per surface (`create_surface { …, hidden }`).

### Evidence (actual output)

```
antikolt-shell --previewworkertest <engine>
grid_ready=ok
deadline_start=ok
deadline_worker=preview-worker-abort
deadline_fired=ok
deadline_state=idle
deadline_no_preview=ok
deadline_surface_gone=not-asked
worker_start=ok attempt=1
worker_card=preview-worker-probe
worker_attempts=1
worker_produced=ok
worker_file=ok
worker_pixels=1579x942 colours=13
worker_ref=preview-worker-probe.png
worker_state=idle
worker_budget=1
worker_surface_gone=no live surface
abort_start=ok
abort_state=idle
abort_no_preview=ok
abort_surface_gone=no live surface
user_card_loaded=ok
preview_worker=ok        (exit 0)
```

The `deadline_*` lines prove that the 5 ms deadline **on its own** closes the work down: there is no image, the
budget does not grow, the state is `idle`, and the late-arriving surface answer does not stay live either (the second phase
tries the same with a card open, before the answer arrives). The `worker_surface_gone` question is the engine's
own answer, not the shell's belief.

### Memory, measured (instead of "4 MB")

In the same run, sampling the engine house's process tree every 150 ms: **8 processes / 143 MB** standing on the grid,
**9 processes / 167 MB** for the worker's lifetime — so the second surface is **a separate renderer
process, ~24 MB**. ADR-041's 4 MB promise was wrong; the correction is in ADR-042.

### What the stricter engine brought to light (ten self-tests)

The engine now *does not* serve a command addressed to a non-existent surface, and this immediately failed ten self-tests
— each one was a real bug that the previous "serve-everything" engine had been hiding:

- **stale surface id**: `lifecycletest` addressed an `eval` to the *old* grid surface after hibernation
  (the grid came back on a new surface) → the test now follows the new id;
- **dangling `id`**: deferred lambdas captured a load-handler's `id` parameter by `[&]`, and the timer
  later pointed at garbage (an `eval` to `surface=140703778610427`) — `embedtest`, `metricstest`,
  `timeouttest`, `notetest`, `pintest`, `bridgetest`, `previewtest`, `previewgentest`, `scrollzoomtest`.
  The fix everywhere is `[&, id]`, within the lifetime of the creating closure.

`metricstest`, besides this, **polls** the page's answer (`page_poll_ticks`) instead of a fixed 1200 ms, because the
bridge request's three round trips can be longer on a loaded machine than any baked-in delay — this was the
cause of the earlier "measured zero".

### The capture that sometimes waits (environment, not product)

The hidden surface's first `Page.captureScreenshot` on this machine (no GPU, software compositing)
sometimes waits **more than 20 s** for a frame; on this machine the GPU process dies at startup with `-2147483645`
(see `18-F0-JELENTES.md`). This is not a bug in the product: the 8 s deadline closes the worker down, the card is skipped,
and the queue retries in the next round. The test does the same (at most three rounds, `worker_attempts`),
because the contract is "sooner or later it fills in", not "it succeeds on the first try". I tried two things and
**rejected them**, because measurement did not support them: making the worker surface visible, the `fromSurface=false`
capture (a white image, `colours=1`) and the background-throttling switches.

### My own mistake in the slice

I wanted to insert a temporary debug line into `main.cpp` with a one-line PowerShell command; because of a wrong index,
the file's first 1717 lines were lost. Recovery: `git checkout` from HEAD + concatenating the saved tail, then
re-applying the fixes — the file's line endings also went back to LF. The lesson: **a file must be edited with the `edit` tool**,
not by line index from PowerShell.

### Gate

`pwsh -File scripts/verify.ps1` → **ALL SELF TESTS PASSED**: 29 checks (28 self-tests, including
`previewworkertest ok`, and the `check-repo`: SPDX, links, hygiene, `node --check`, `cargo fmt/clippy/test`
— 78+7+2+1+15 tests).

### The panel also shows it (slice 42)

The `session_list` answer's rows now also carry the quantity measured by the core (`size`: cookies, bytes, service
workers; `loses_login`), and the panel's sixth column prints this: `7 süti · 5.0 kB`, with the
`· belépés` suffix for a logged-in row. The UI counts nothing: the core decides, the shell only displays.

### Evidence

```
--sessionpanel
panel_row=https://example.org | expired | sweep | 7 | lejárt | 7 süti · 5.0 kB
session_panel=ok
```
## 2026-11-17 (fifty-second slice) — ADR-041's guards, proven

The first piece of the background-preview worker is the **four conditions** without which it may not start, and the rule
by which it must immediately abort. Both are in `previews.h`, in one place, so that the window cannot
forget any of them:

- `previewWorkerCanStart(state, has_live_card, budget_ok, has_candidate)` — only in the `Idle` state, **without a card**
  (the user is on the grid), with budget and a candidate;
- `previewWorkerMustAbort(state, has_live_card)` — the running worker **immediately** aborts when a card opens.

### Evidence (actual output)

```
antikolt-shell --previewqueue
queue_first=no-preview
queue_second=stale
queue_none=empty
queue_guards=ok
preview_queue=ok        (exit 0)
```

The `queue_guards` line proves five cases: starting on the grid; it does **not** start over a card; it does **not** start without budget;
it does **not** start a second worker; and the running worker aborts when a card opens.

One mistake in my own work: the first insertion landed **after the closing brace** of `namespace antikolt`
(`C2039`), so the compiler could not find it; the block is now inside the namespace.

### What still remains

- the worker's **execution** (creating a surface with the `persist:preview` partition, 8 s deadline, capture,
  writing `preview_ref`, teardown) — the next slice, with `--previewworkertest`.
## 2026-11-17 (fifty-first slice) — who gets a background preview: the decision

The background preview (I1) can be split into two parts: **which card** should get an image, and **when** it is even allowed to
take a photo (one live surface, not disturbing the user). The first part is a pure decision, and it is done:

`antikolt::nextPreviewCard(cards, previews_dir, now, max_age_ms)` — the order a user would state it:
**first the one without an image**, then the **stale** one (7 days), the fresh one gets no new image; the `file:`/non-http
cards are skipped (there is nothing to photograph). At equal rank, the card opened longer ago goes first.

### Evidence (actual output)

```
antikolt-shell --previewqueue
queue_first=no-preview
queue_second=stale
queue_none=empty
preview_queue=ok        (exit 0)
```

`--previewqueue` was added to the `verify.ps1` gate.

### What still remains

- the **worker** (a second, temporary surface outside the grid, one at a time) — this affects I1, so
  it needs a separate decision/ADR before it becomes code;
- the condition for the background start: there must be no live card, and the daily budget must allow it (`previewBudgetAllows`).
## 2026-11-17 (fiftieth slice) — MSIX: the Store branch's package

ADR-040 fixes two channels; for the Store branch only the decision existed so far, no package. Now there is the
`packaging\msix\AppxManifest.xml` (a full-trust Win32 package, `internetClient` + `runFullTrust`, five generated
tile and splash images) and `scripts\msix.ps1`, which packages the `dist\Antikolt` folder **unchanged**,
with `makeappx`, and at the end **verifies by unpacking** that the three processes, the page and the CEF
runtime are inside.

### Evidence (actual output)

```
pwsh -File scripts/msix.ps1
  packed   : 200.8 MB
  payload  : complete
  msix=D:\Szerver\Antikolt\dist\Antikolt.msix size_mb=200.8 payload=ok
  exit 0
```

The package compressed is 200.8 MB (the portable folder is 442 MB), the payload check: `antikolt-shell.exe`,
`antikolt-engine-host.exe`, `antikolt-core-service.exe`, `AppxManifest.xml`, `home\index.html`, `libcef.dll`,
`icudtl.dat` — all present in the unpacked package.

### What still remains (Store branch)

- **Identity from Partner Center**: the `Identity` (`Name`, `Publisher`, `Version`) is now a placeholder; the Store
  gives the real values at submission, and the `Publisher` must match the signing certificate.
- **Local install test**: `Add-AppxPackage` only works if the certificate is trusted on the machine —
  in this environment writing the certificate store is forbidden (see `20-ALASAS.md`), so the install is **not
  verified**, only the packaging and the payload.
- **Submission**: account + Store verification (a human step).
## 2026-11-17 (forty-ninth slice) — portable build, shortcut, Release, icon, auto-start

Eight slices in one block (42–49), because inserting the log entries silently failed — the
`Replace` anchors did not match (the log's headers had a different numbering), and despite the "docs written" message
the file did not change. This entry makes up for it, with the **facts**, not the original intent.

### 42. The panel shows how much would be lost
The `session_list` rows carry the quantity measured by the core (`size`) and the `loses_login` flag; the panel's
sixth column: `7 süti · 5.0 kB`, `· belépés` for a logged-in row. Evidence: `--sessionpanel`
`panel_row=… | 7 süti · 5.0 kB`, `session_panel=ok`.

### 43. The saved note appears (and the focus)
The note was missing from the card's re-render **signature**, so the saved note did not update on the
screen. The signature now includes it; the focus-return is implemented (post-render, id-based). Measuring the focus
is not possible because the test window lacks focus (`note_focus=none` / `document.activeElement`
does not even exist) — the trial remains without an assertion, and this is also recorded in the log.

### 44. Portable build + desktop shortcut (`scripts/dist.ps1`)
`dist\Antikolt`: shell + engine house + core + page + Qt (`windeployqt`) + CEF runtime, and a desktop
shortcut. Two errors only came out here: the CEF **resources are in a separate folder** (`Resources\`:
`icudtl.dat`, `*.pak`, `locales\`), and the page in the portable folder is `appDir\home`. Evidence from the package:
`grid_source=core`, `grid_count=6`, `grid_matches_core=ok`, `grid_click_open=ok` (exit 0), 443 MB.

### 45. Our own icon
`assets/antikolt.ico` (four sizes, generated from code), `--selftest` → `icon=ok`, the shortcut's
`IconLocation` points to it.

### 46. `--capturetest` shutdown crash fixed
`0xC0000005` **after** printing the results, 1 time out of 4 runs: the `destroy_surface` answer arrived at a dying
client. The test now waits for the answer (max. 2 s). Measurement: **8/8 runs green**.

### 47. Release package (`dist.ps1 -Release`)
A separate `build-release\` (`-Config Release`), `cargo build --release`; the developer `build\` is untouched.
Lesson: `cmake` is not on the shell's PATH — so the script calls `build.ps1`, and does not duplicate the
toolchain. The `check-repo` and `.gitignore` now treat `build-release\` as generated output too
(which is why the SPDX count went 55 → **52**, the correct value).

### 48. Double-click = browser
On a plain launch the engine starts **on its own** (the "Start engine" button is only needed when the engine is stopped).
Evidence from the package, without switches: `protocol server ready`, `op=hello`, `op=install_bridge`.

### 49. Start menu + absolute paths
`dist.ps1` also writes a Start menu entry (the same `.lnk`), and normalizes the target folder to a full path —
the shortcut's icon was previously `…\scripts\..\dist\…`.

### The state of the gate

```
scripts/verify.ps1  →  27/27 self-tests, ALL SELF TESTS PASSED
cargo test          →  78 tests
check-repo          →  ALL CHECKS PASSED (52 SPDX files)
```

### What still remains

- **Signing + installer** (SmartScreen on an unsigned exe; a code-signing certificate is needed for it);
- functional remainder: background previews (I1), memory budget (F6), per-card session override,
  the cleanup's media exception, Linux/Wayland (F7), safe/passkey;
- measuring the focus and the rendered note (the correct way to measure is in the log).

## 2026-11-17 (forty-first slice) — how much would be lost: `SessionSize`

`LossEstimate` already existed, but it describes the **friction** (password, TOTP, captcha) — not the **quantity**.
The panel needs both: "3 cookies" and "you must log in again" are two different bads. So the ledger's row
now gives a `SessionSize` (`size()`: cookies, bytes, service workers) and `loses_login()`; every number comes from the row's
own fields, no estimation.

The first attempt **duplicated** `LossEstimate` (the same name, different fields) — the compiler stopped it with `E0428`,
and that was right: one type, one meaning. The final name is `SessionSize`, because it answers a different
question.

### Evidence (the actual output)

```
cargo test -p antikolt-core
test session::tests::session_size_counts_what_a_wipe_removes ... ok
test result: ok. 78 passed; 0 failed
```

### What is still open

- The panel's **display** (`LossEstimate` + `SessionSize` in the rows) — the core side is ready, the wire and the
  UI come next.
## 2026-11-17 (fortieth slice) — giving the place back, end to end (three bugs)

The previous three attempts got stuck at the measurement. Now the test **passed**, and in the process it found **three real bugs**
— none of them was a test bug:

1. **The grid you return to was dead.** `ensureSurface()` only *navigated* the existing surface to the
   home URL, but it did not install the bridge there (only `surfaceAttached` did that, for a new surface). So
   returning home from a card, the grid's cards were **not clickable** — in the user's eyes "the browser
   stopped". In the existing-surface branch there is now `installBridge` + setting `home_surface_`.
2. **The save raced the navigation.** `rememberScroll()` started an asynchronous `evaluate`, and the
   caller immediately navigated on: the read value became the *next* page's 0. Now the caller only continues
   **after** the save (`rememberScroll(std::function<void()>)`), with `loadHome`/`openCard` chained.
3. **The grid's zero overwrote the card's.** After returning home the surface is the grid, but `current_card_id_`
   was still the card's: the next save wrote 0 in place of 700 (in the log `saving 700` then `saving 0`).
   Now the save is skipped if the live surface is the grid, and `loadHome` clears the card id.
4. **The restore is absolute and delayed.** `loadFinished` is not "layout ready", and it fires **twice**
   for one page: the relative scroll ran twice (`restored_scroll_y=1400`). Now `window.scrollTo(0, N)`,
   300 ms after the load.

### Evidence (the actual output)

```
antikolt-shell --scrollrestore <motor>
first_scroll_y=700
restored_scroll_y=700
scroll_restore=ok      (exit code 0)
pwsh -File scripts/verify.ps1 → 27 checks, ALL SELF TESTS PASSED
```
## 2026-11-17 (thirty-ninth slice) — the card's place: where the reader left off

Reopening a card we started at the top of the page: in a long article this is not a "return", but a lost place.
Now the shell remembers the scroll position per card in the profile's `scroll.json`
(on the model of `previews.h`, `scroll_book.h`: `loadScrollBook`, `scrollPositionFor`, `saveScrollPosition`), when leaving the card
(`openCard`/`loadHome`) it asks the engine for `window.scrollY`, and on return it
restores it.

**Why not on the card record?** Because this changes on every visit, and the record is the user's data —
we do not rewrite it just because someone scrolled. A zero position **deletes** the entry, so the book only
keeps places that mean something.

### Evidence (the actual output)

```
antikolt-shell --scrollbook
scroll_before=0
scroll_a=640
scroll_b=120
scroll_a_after_top=0      ← a tetejére állás törli a bejegyzést
scroll_b_after_top=120    ← ...és nem viszi magával a másik kártyát
scroll_reloaded=120       ← lemezről újraolvasva is megvan
scroll_book=ok      (exit code 0)
```

The wiring is covered by the existing card-opening tests (`--opentest`, `--scrollzoomtest`: the card still
opens and is scrollable alongside the restore); there is no **standalone end-to-end test** for the restore yet —
that is the next slice.

### Attempt at the end-to-end test — with a negative result

I wrote the two-phase test (open → scroll to 700 → home → reopen → read). The first phase **measurably
works**: `first_scroll_y=700`. In the second phase, however, after the grid reloaded, the page's DOM did not
find the card (`clicked_phase1=no-card`), even though I waited 2.5 seconds — that is, the card list's
*painting* on the second return arrives later than the test would click. I did **not** leave the test in the
Gate: I removed it from the code, because a check that fails unreliably is worse than a missing one. The lesson is the
starting point of the next slice: for the grid card list to load, one must wait for an **event** (the page should signal that it
received the list), not for a timer.

### What is still open

- End-to-end test for the restore (because of the negative result above: waiting for the grid's "card list received"
  signal).
- Per-card session override, `LossEstimate`, memory budget (F6).
## 2026-11-17 (thirty-eighth slice) — the preview-production daily budget is persistent

The ≤100/day limit was until now a run-level counter: after a restart it started from zero, so it was not a limit.
Now the budget lives in the profile (`previews/budget.json`: `{"day":"…","count":N}`), the day is the whole state — there is
no stored clock arithmetic —, and a counter from another day reads as **zero**. The rule is in
`previews.h`: `previewUsedToday`, `previewConsumeBudget`, `previewBudgetAllows`; the shell asks before the capture,
and counts after a successful save. A refused preview is not an error: the card keeps what it had.

### Evidence (the actual output)

```
antikolt-shell --previewbudget
budget_start=0
budget_after_three=3
budget_after_five=5
budget_allows_at_three=true
budget_refuses_at_five=true
budget_other_day=0        ← a tegnapi számláló nem a mai
budget_fresh_allows=true
preview_budget=ok      (exit code 0)
```

### What is still open

- Previews made **in the background** (I1): today only the card opened by the user gets an image.
- The 7-day refresh threshold and the daily budget of 100 are **not** tunable from the UI.
## 2026-11-17 (thirty-seventh slice) — the preview goes stale, and then it is made again

The preview was until now made **once**: the card's image never refreshed. Now its age decides: if the file does not
exist, or is **older than 7 days**, the next open re-photographs it. The age is the file's own modification
time (`previews.h: previewNeedsRefresh`) — a preview is a photograph, so "when it was made" is exactly
what the filesystem already records; a second timestamp on the card record would only be another thing to keep in sync.
A future modification time (a clock set forward) counts as **fresh**, otherwise every open would
re-photograph.

### Evidence (the actual output)

```
antikolt-shell --previewrefresh
refresh_missing=true    ← nincs fájl: újra kell
refresh_fresh=false     ← friss: nem kell
refresh_stale=true      ← 30 nappal visszaállítva: újra kell
refresh_aged=true       ← a visszaállítás sikerült (a mérés előfeltétele)
refresh_future=true     ← jövőbeli időbélyeg: nem csinálunk belőle fényképezőgépet
preview_refresh=ok      (exit code 0)
```

### An error in the measurement, fixed

The test first asked again, *after the reset*, whether "it is fresh", and compared it with its own earlier assertion
— a different question gets a different answer. Measuring "fresh" now happens **before** the reset
(`fresh_ok`), and the flag (`refresh_fresh`) reflects that too.

### What is still open

- The **persistent daily ≤100** limit (the counter is still run-level), and the previews made in the background (I1).
- The 7-day threshold is **not** tunable from the UI (intentional: for now it is in one place).
## 2026-11-17 (thirty-sixth slice) — the orphaned previews are deleted

The preview image lives in the profile, and the card record only carries its name (ADR-035) — if the card disappears, the
image would stay there forever. Now a **patrol** runs as part of the cleanup round: we collect the referenced names from the core's card list,
and every other image in the `previews/` folder is deleted (`previews.h`, a pure function).

### Evidence (the actual output)

```
antikolt-shell --previewclean
preview_removed=orphan.png,orphan.webp   ← két árva, mindkét kiterjesztéssel
preview_kept=ok                          ← a hivatkozott fájl marad
preview_orphan_gone=ok
preview_stranger_kept=ok                 ← a nem-előnézet fájlhoz nincs közünk
preview_missing_dir=ok                   ← hiányzó mappa nem hiba
preview_cleanup=ok      (exit code 0)
```

The decision is made by filename and extension (the patrol never opens the file), so the test's
`orphan.webp` is only a *content-free* file: the behaviour is measurable even without a WEBP encoder.

### What is still open

- The preview **refresh** (today it is made once), the persistent daily ≤100 limit, and the previews made in the background
  (I1) are still outstanding.
## 2026-11-17 (thirty-fifth slice) — the session rules are adjustable

The policy was until now only displayable. Now it can be set on the panel: expiry in hours (1–720), an "delete at app close"
checkbox, a Save button; on the wire the new `session_rules` op, in the core the `SessionStore::set_rules`, which
**checks before writing** (1 hour – 30 days), and the refusal gets its own error variant so that the answer
says what is wrong. The decision is in ADR-039.

### Evidence (the actual output)

```
antikolt-shell --rulestest <mag>
rules_ttl_ms=7200000        ← 2 óra mentve
rules_lock_wipe=true
rules_refused=refused       ← az 1000 ms-os TTL-t a mag elutasítja ("the TTL must be …")
rules_reread_ttl_ms=7200000 ← és a tárolt szabály maradt az első, nem az elutasított
session_rules=ok      (exit code 0)
```

### An error in the measurement, fixed

`session_rules` first returned `SessionError::Rejected(LedgerError::UnknownState)` for a TTL error —
the answer would have been "unknown state" for a typo. A separate `SessionError::Rules` variant was made, with its own
message.

### The editor fields show the core's state

The panel now **fills** the fields from the `session_list` answer (hours, delete at app close), and sends the container
default (`container_default`) back unchanged — the shell has no opinion about it, so
it cannot overwrite it. Thus a refused edit never looks saved: the panel is a *view of the policy*,
not its own memory.

`--sessionpanel` measures this: `panel_ttl_hours=2`, `panel_lock_wipe=true` (the test sets the
rules first, so it does not read the leftover of an earlier run — the shared store was a trap here too).

### What is still open

- A **per-card** (origin-level) override alongside the container rule.
- Because of the hours unit, a non-whole-hour expiry cannot be given (an intentional simplification).
## 2026-11-17 (thirty-fourth slice) — scrolling and zooming: the contract redeemed

`proto/README.md` had long advertised the `scroll` and `set_zoom` ops, the engine answered `not_supported`.
Both are now done, with the decisions in ADR-038: `scroll` scrolls the **document**
(`window.scrollBy`, deterministic, without a pointer), and `set_zoom` **reflows** the page
(`Emulation.setDeviceMetricsOverride`), **multiplying** the scale by the screen's own DPR.

### The shell side is also ready: a zoom button and remembering

On the toolbar `−` / `🔍 100%` / `+` (25 % steps, between 25–500 %), and the choice **remembers**: we re-apply it at every
surface attach and load (like the page theme), because zoom is the browser's
setting, not a page's. The buttons call the same public `setZoomPercent()` the test does — one
code path, not two.

### Evidence (the actual output)

```
antikolt-shell --scrollzoomtest <motor>
scroll_y=600
dpr_at_150=1.875      ← 1,25 (a kijelző saját DPR-je) × 1,5
dpr_at_100=1.25       ← a felülírás törlése a természetes értéket adja vissza
dpr_at_125=1.5625     ← a shell saját útja (toolbar-metódus): 1,25 × 1,25
scroll_zoom=ok      (exit code 0)
```

### Three errors from the measurement, both fixed

1. **The shared store again.** The test clicked on the `127.0.0.1` prefix and hit the
   `preview-probe` card of earlier runs, whose URL pointed at an already-dead port
   (`ERR_CONNECTION_REFUSED`) — the log's `load_failed` line revealed it. The two probe tests now click
   **by their own card id** (`scroll-probe` and `preview-probe`, respectively).
2. **The absolute-DPR trap.** Zoom originally set `deviceScaleFactor = zoom`, which on HiDPI
   (this machine is 125 %) *shrinks*: instead of 150 % it gave a DPR of 1.5 instead of the natural 1.875. The engine now
   first measures the page's DPR, and multiplies — and the test sets the **quotient** (1.875/1.25 = 1.5), which is
   independent of the display's resolution.
3. An `#include` line slipped to the end of the preprocessor directive (`#include <functional>#include <QGuiApplication>`)
   — the C4067 warning caught it; without the `#include` the build would have died.

### What is still open

- `scroll` scrolls the document, not the inner container in focus (gesture-based scrolling = a separate
  slice).
- The shell does not use this op pair yet: there is no zoom button and no scroll-position restore.
- Zoom overrides the viewport, so the page gets a `resize`; the shell must not write it back with `set_bounds`.
## 2026-11-17 (thirty-third slice) — the page theme: a dark page, where the page knows it

The first, honest slice of F7 (page themes): the browser **signals** `prefers-color-scheme` to the page,
it does not force colours. The engine's new `set_media` op sets it with `Emulation.setEmulatedMedia` (`auto` = no
override, `dark`, `light`), the shell's toolbar button cycles it (auto → dark → light), and **remembers** the
choice: it re-applies it at every surface attach and load, because the emulation only holds for the currently
visible page. The decision is in ADR-037.

### Evidence (the actual output)

```
antikolt-shell --mediatest <motor>
media_dark=dark       ← a lap matchMedia-je igazat mond
media_light=light     ← ...és vissza is vált
site_theme=ok      (exit code 0)
```

The witness is the page's own `matchMedia('(prefers-color-scheme: dark)').matches` value — the shell cannot
claim anything the page does not see. We measure both directions, because a page that always reports dark
is not following the browser, it is simply dark.

### An error in the measurement, fixed

The test first stopped with an **access error** (0xC0000005), because the `ask` lambda defined inside the
`loadFinished` handler was captured by reference by the timers that ran later: the handler's stack was no longer alive when the
timer fired. The lambda now lives **outside** the handler, and is copied **by value** into the timers (the
`surface` id is received as a parameter, not by reference). A lesson for the log: the Qt timer + C++ lambda
combination silently makes a dangling reference if the lambda is declared in a short-lived scope.

### A second, slower-revealing bug: `capture` can get stuck

In the full `verify.ps1` run `--previewgentest` **sometimes** failed, but on its own it always passed. The
tracing revealed: the engine **never answered** the `capture`, the shell waited until its own 20 s
request timeout, and the test moved on after 10 s — and in the end the error arriving after `shutdown`
(`a motorház kilépett (kód 0)`) masked the real cause. In the log the GPU process's
`ContextResult::kFatalFailure` loop appears in the same second, so `Page.captureScreenshot`
can get stuck while the GPU process restarts.

Two fixes: (1) the shell **retries once** the stuck capture (`kPreviewAttempts = 2`, with a 1.5 s
pause) — a GPU error must not cost a preview; (2) the test now **waits longer** than the shell's timeout
(60 × 400 ms) and prints the length of the wait (`preview_wait_ticks`). Five consecutive runs: all
`preview_generation=ok`.

### What is still open

- A **per-page exception** ("this page should stay light") — today the setting is global.
- **Forced** darkening of pages that do not know `prefers-color-scheme`: intentionally absent (the image,
  the brand and the contrast would suffer); if needed, with a separate ADR.
- `scroll` and `set_zoom` are still `not_supported`.

## 2026-11-17 (thirty-second slice) — the memory number is real: the engine measures, the page sees

The product's promise is a memory-saving browser, but the UI showed nothing: the page's `system.memory`
call got zero from the mock. Now the engine's `metrics` op measures — the page's own DOM nodes and
JS heap (`Runtime.evaluate`), as well as its own working set (`GetProcessMemoryInfo`) —, and the shell
answers this at the page's **request** over the two-way channel. The budget is intentionally 0 until the profiles
carry a memory budget (F6): the page then only prints the used value, it does not invent a budget.
The decision is in ADR-036.

### Evidence (the actual output)

```
antikolt-shell --metricstest <motor>
asked=ok
dom_nodes=57
js_heap_bytes=10000000
engine_working_set_bytes=117497856
page_used_bytes=103763968
metrics=ok      (exit code 0)
```

Two witnesses: the engine's `metrics` answer (DOM nodes, working set) and the page's `system.memory` answer, which
came back over the bridge. A note on the numbers: the **JS heap is quantized** — Chromium rounds it for privacy reasons
(`10 000 000` exactly), so it is an indicator, not accounting data; the working set and the page's answer differ
slightly in time (117 MB is the engine's process, 103 MB at the moment of the request), because the working set moves.

### What is still open

- The **budget** (F6) and the per-card memory breakdown: `metrics` today measures the live surface, not per card.
- `metrics` also answers for the **home surface** (57 nodes) — measuring the cards is the same path, but the
  status bar and the log do not use it per card yet.

## 2026-11-17 (thirty-first slice) — the preview gets made: opening the card gives it an image

Slice thirty served the preview; now the shell **also produces it**. The card's record got an additive
`preview_ref` field (the old store remains parseable), the grid adapts it
(`antikolt://blob/<name>`), and when the user opens a card, the shell takes an image from the **real page**
(`capture`), moves it into the profile's `previews/` folder, and writes the name back into the record.
A card's preview is made once (it does not overwrite an existing one), and one run produces at most 40.

### Evidence (the actual output)

```
antikolt-shell --previewgentest <motor>
card_written=ok
card_opened=ok
preview_file=ok
preview_ref=preview-probe.png
preview_page=loaded:1560x857
preview_generation=ok      (exit code 0)
```

### Two bugs from the measurement, both fixed

1. **Stale core binary.** The shell wrote `preview_ref`, but the core's `card_upsert` ran on the old
   binary (it dropped `preview_ref` as an unknown field) — the response was successful, the store did not
   contain it. This was a **tooling** bug: neither `build.ps1` nor `verify.ps1` built the Rust
   core. Now `verify.ps1` starts the core with `cargo build -p antikolt-core-service`, and reports this as a
   separate check.
2. The three `Card` initializers (the seeded cards) did not know the new field — `cargo build` caught this
   immediately, and the field is now `String::new()` in all three.

### Still open

- The preview **refresh**: today it is generated once; a stale image is not replaced.
- The **daily ≤100** limit: the counter is run-scoped (in memory); for a durable budget the core would need
  to keep a record.
- **Cleanup** of the preview files (the 24-hour policy does not touch them), and previews generated in the background (without
  the user's card) — this is a separate slice because of I1.

## 2026-11-17 (thirtieth slice) — the preview path: `antikolt://blob/` from the engine

The page has long referenced `antikolt://blob/<ref>`, but no one served it. Now the engine
registers the `antikolt` scheme (`STANDARD | SECURE | CORS_ENABLED | FETCH_ENABLED`) and answers
the `blob/<name>` request from the `<user data>/previews/<name>` file; the name is resolved **within the directory**
(empty name, `..`, separator → rejection). The decision is in ADR-035, the wire-side description in
`proto/README.md`.

### Evidence (the actual output)

```
antikolt-shell --previewtest <motor>
preview_file=probe.png 4x3 png
asked=ok
preview_load=loaded:4x3        ← the page's own witness: naturalWidth×naturalHeight
preview_traversal=refused      ← `antikolt://blob/..%2Fhost.cpp` cannot be loaded
preview_delivery=ok      (exit code 0)
```

Two bugs during compilation, both fixed: (1) the include of `cef_stream_resource_handler.h` was
placed **inside** `namespace antikolt`, so the linker looked for `antikolt::CefStreamResourceHandler`
(`LNK2019`) — the include was moved to the top of the file; (2) `DISALLOW_COPY_AND_ASSIGN` declares a private copy
constructor, which swallows the implicit default constructor (`C2512`) — an explicit
`PreviewHandlerFactory() = default;` was needed.

### Still open

- **Generation**: the shell does not generate a preview yet. Needed: opening the card in the background (I1!),
  `capture`, saving into the profile's `previews/` folder, and an additive `preview_ref` field on the card record,
  so the grid can reference it. A scheduler (one card at a time, ~8 s, ≤100 per day) is also still pending.
- **Cleanup** of the preview files (the 24-hour policy does not touch them).

## 2026-11-17 (twenty-ninth slice) — exactly one answer per request (watchdog)

The protocol v2 contract stated that **exactly one** answer comes for every page question — but until now this
was only upheld by the code's paths; there was nothing to enforce it. Now, when a request arrives, the shell
adds the request to a set and starts a **5-second watchdog**; `replyToPage` ensures, by removing from the set,
that two answers cannot go out for one request (neither the watchdog nor a later path can
duplicate). The watchdog itself also answers through this path, so the "exactly one" rule lives in one place.

For the measurement, a deliberately unanswered method was needed (`test.hang`), which is available **only**
with `ANTIKOLT_BRIDGE_TEST=1` — so in a normal run no page can hang the
shell, yet the test still goes through the real watchdog path.

### Evidence (the actual output)

```
antikolt-shell --timeouttest <motor>
asked=ok
answer_ms=6540            ← the answer arrived after the ~5 s deadline (the measurement reads at 6.5 s)
timeout_answer=rejected
timeout_message=the shell did not answer in time
timeout_test=ok      (exit code 0)
```

## 2026-11-17 (twenty-eighth slice) — the note is writable on the card

The third write path on the same channel: on the card, the **note editor** opens inside the card
(not in a dialog), and "Save" goes to the core via `cards.upsert`. The editor opens **in place**,
not with a re-render — a re-render would discard the typed text. The keyboard and
mouse events stop with `stopPropagation` at the textarea boundary, otherwise the grid's arrow-key
navigation and the card's "opens on click" behavior would leak in while typing (one of the two
would certainly have caused a bug).

| Area | Content |
|---|---|
| Card | `.note` button + `.note-editor` (textarea, Save, Cancel) in the shadow root; the note text is in the body |
| Page | `HomePage.setNote(id, notes)` → `cards.upsert`, then a re-render **from the core's answer** |
| Witness | `window.antikoltNoteState()`: the save, the model and the error in one place |
| Self-check | `--notetest`: the editor opens, the text is typed in, Save goes through the bridge, the shell and the core |

### Evidence (the actual output)

```
antikolt-shell --notetest <motor>
note_save=ok
note_id=01SEED000000000000000001
note_page=probe note
note_model=probe note
note_core=probe note
note_error=
note_write=ok      (exit code 0)
```

Three independent witnesses agree: the editor's answer, the grid's model, and a **fresh** core-client record.

### Still open

- **Deleting** the note goes via Save as empty text (there is no separate "delete" button).
- While editing the note, the grid cannot be scrolled with the keyboard (the focus is the textarea's) — this
  is intentional, but giving the focus back to the card is not solved yet.

## 2026-11-17 (twenty-seventh slice) — the grid writes: the pin badge reaches the core

The twenty-sixth slice handed over the channel, but the grid had not used it yet. Now the card's **pin badge
is clickable**: the badge is a `button` in the card's shadow root, with `stopPropagation` (otherwise the
click would also open the card), and reaches the page via an `antikolt-pin` composed event, which passes it
to the core with `cards.upsert`. The grid re-renders **only after the core's answer** — so a failed
write does not leave the grid in a state the store does not contain.

On the page side, two pitfalls had to be handled: (1) the `withNativeOpen` composition had only translated `open`
so far, so `cards.upsert` did not exist (`the bridge cannot write (no engine)`); (2) the model
view, but the core's record has a **different shape** (in the model a `container` object, in the record a string) — therefore
the write sends the core's record (`coreRowById`), not the view.

### Evidence (the actual output)

```
antikolt-shell --pintest <motor>
pin_click=ok
pin_id=01SEED000000000000000001 before=pinned
pin_page=unpinned pinned_count=2
pin_error=
pin_core=unpinned
pin_write=ok      (exit code 0)
```

The assertion is deliberately the **reversal**, not "the pin becomes true": the store is shared with the other self-tests (the
`--bridgetest` also toggles the pin), so the test clicks the first card and demands that the page
**and** the core end up in the same opposite state. Two runs in a row: `pinned → unpinned`, then
`unpinned → pinned` — both `pin_write=ok`. Two independent witnesses agree: the page's own
`window.antikoltPinState()` and a **fresh** core-client `cards_list`.

## 2026-11-17 (twenty-sixth slice) — the page can ask: the bridge is bidirectional (protocol v2)

ADR-032's one-way bridge was enough to fill the grid, but the page could not get an **answer** from the core.
This slice handed over the request direction: `window.antikoltNative.request(method, args)` is a **persistent
`cefQuery`**, which the engine holds onto, forwards as a `bridge_call` event with a request number, the shell
answers with `bridge_reply`, and the page's promise settles with the answer. This is **protocol v2** (ADR-034).

| Area | Content |
|---|---|
| Contract | `crates/antikolt-ipc`: `Message::BridgeReply { request, json, error }`, `PROTOCOL_VERSION` 1 → 2, `expects_reply()` false for it; roundtrip test |
| Engine | `HostState::pending_bridge` (request number → callback), `OnQuery` request branch (no immediate `Success`), `bridge_reply` op, `OnQueryCanceled` discards the pending requests; `kProtocolVersion = 2` in `protocol.h`, the parser and the `welcome` use this too |
| Shell | `EngineClient::bridgeReply`, `bridgeCall` signal + `request` number, `MainWindow::replyToPage`; `cards.search` from the core, **rejection** for an unknown method |
| Page | `kBridgeScript`: `request()` + `cards.search()` helper, `version: '2'` |
| Self-check | `--bridgetest`: the page asks, the shell answers from the core, the page's own variable receives it |

### Evidence (the actual output)

```
antikolt-shell --bridgetest <motor>
asked=ok
search_cards=1
search_first_title=Wikipedia — homepage
unknown_method=rejected
bridge_request=ok      (exit code 0)
```

Two bugs from the measurement, both fixed: (1) the engine's `ParseCommand` **rejected the v2 envelope**
(`unsupported protocol version 2`), because the version check still expected `1` — the version now lives in one place
(`protocol.h kProtocolVersion`), and the `welcome` sends the same; (2) the `C4457` warning
(shadowing of the `request` parameter) was an error under `/WX`, so the variable is `bridge_request_id`.

### Writing the card goes through the same channel too (`cards.upsert`)

The request direction is only worth anything if the page can also **write**: `cards.upsert` carries the whole card record
(pin, note, new card in one call), the shell passes it to the core (`card_upsert`), the core validates and stores.
The `--bridgetest` measures this too: the page looks up the "wiki" card, **flips its pin**, sends it, then
**asks again** — and the second answer already carries the flipped value, because the answer always comes from the core,
not from the page's own copy.

```
asked=ok
search_cards=1
search_first_title=Wikipedia — homepage
unknown_method=rejected
upsert_pinned=true
upsert_persisted=ok
bridge_request=ok      (exit code 0)
```

### Still open

- The **grid's own search** is still local (the page receives the full list) — the channel was built for the
  questions the page cannot answer on its own. `cards.upsert` is ready; the **grid UI**
  (clicking the pin badge) does not use it yet.
- The request-lifetime timeout: if the shell does not answer, the page's promise waits for the page's lifetime; the
  engine-side discarding on page disappearance is done, the shell-side obligation to answer is not enforced yet.

## 2026-11-17 (twenty-fifth slice) — the policy **finally** runs too: the panel cleans up

Since the twenty-fourth slice, the engine can delete, but no one asked it to. Now the panel has
**"Clean up now"**, and the shell itself carries out the core's judgment: it fetches `session_due`, and for every
due origin it either deletes **immediately** (if that card is currently open) or **queues it**, and
deletes it on the card's next opening — because the shell has a live surface (I1), and a cleanup must not
steal the page the user is reading. The deletion goes with the plan's storage list (ADR-033), after
success the row disappears from the ledger with `session_close`, and the panel refreshes.

### Evidence (the actual output)

```
antikolt-shell --cleanuptest <motor>
origin=http://127.0.0.1:50114
session_written=ok
page_wrote=ok            ← the page itself confesses that it wrote (not the "there was nothing" case)
storage_after=|          ← both the cookie and localStorage are empty
rows_left=1 origin_rows=0   ← our row disappeared (the remaining 1 is another test's row)
cleanup=ok      (exit code 0)
```

Two lessons from the measurement, both incorporated:

1. **A running client reads its own, old copy.** I first did the ledger check with the running oracle
   and got `origin_rows=1` — yet the row was no longer in the file. `SessionStore` reads at startup;
   therefore the test reads the file back with a **fresh client**, the one the shell has just written.
2. Because of the **shared data directory**, the ledger is not empty (`--sessionpanel` also writes rows), so the test
   does not require "0 rows", but the disappearance of **its own origin**.

### Still open

- The `session_rules` op (setting rules on the wire) and editing the 24 hours / `wipe_on_app_lock`
  on the panel.
- Showing `LossEstimate` (what the user loses with a wipe) in the rows.
- The cleanup's **media exception**: if the card is playing, the deletion must wait (the hibernation exception is
  ready, the cleanup's is not yet).

## 2026-11-17 (twenty-fourth slice) — executing the wipe (the engine deletes, not only decides)

The twenty-second slice laid down the decision, the twenty-third showed it; this slice **executes**.
The engine had returned `not_supported` for `clear_origin_data` — now it calls the `Storage.clearDataForOrigin`
DevTools method, on the same `DevToolsCall` as the snapshot, the capture and the eval, so
the cleanup is not a second, parallel path toward Chromium (ADR-029).

| Area | Content |
|---|---|
| Engine | `ClearOriginCall` (`Storage.clearDataForOrigin`, with the plan's storage names), real execution instead of `not_supported`; logs an unknown CDP name |
| Name mapping | the plan's `indexed_db` → CDP `indexeddb`; the rest pass through (Chromium ignores the unknown ones, so the log tells you) |
| Wire | `clear_origin_data`'s answer `{cleared: true, origin}`; `Command` now also reads an `origin` + `storages[]` field |
| Shell | `EngineClient::clearOriginData(surface, origin, storages, handler)` |
| Self-check | `--wipetest`: a **real `http://` origin** from a local socket (`file://` has none, `data:` has an opaque origin), cookie + localStorage write, read, delete, re-read |

### Evidence (the actual output)

```
antikolt-shell --wipetest <motor>
origin=http://127.0.0.1:49673
engine_start=ok
surface=1
load_status=200
storage_before=1|probe=1
clear=ok
storage_after=|
wipe=ok      (exit code 0)
```

The evidence is the **difference**, not the answer: first it must be readable (`1|probe=1`), then both
stores must be empty (`|` = empty localStorage + empty cookie). Because of the local HTTP origin, the test does not depend on the
network — for this the shell now gets `Qt6::Network` (for the tests only, a socket in its own
process).

### Still open

- The **cleanup launched from the panel** is still missing: `clear_origin_data` expects a live surface (the
  browser of that partition), but the panel does not open a card. The execution is the next slice: opening the
  due row's card (in the background), deleting, then back to the grid.
- `session_storage` and `code_cache` are not separate types in CDP: `sessionStorage` goes away with the page's
  discarding (the hibernation already does this), and `code_cache` is part of `cache_storage`. The log says
  this out, the code does not lie about it.
- The `scroll`, `set_zoom`, `set_media`, `metrics` ops still return `not_supported`.

## 2026-11-17 (twenty-third slice) — the Sessions panel

The twenty-second slice's wire was ready, but nothing showed it. This slice gave its **visible** side:
on the shell's toolbar the "🗂 Sessions" button opens a right-side dock with the ledger's rows
(origin, state, policy, cookies, **expired**), and the rows come from the core's answer — the list
from `session_list`, the "expired" indicator from `session_due`, with the core's clock and rules (ADR-033). The
panel also exposes its own text (`sessionPanelSummary`), so it can be tested without relying on the
model or the status line.

### Evidence (the actual output)

```
antikolt-shell --sessionpanel <motor>
core_start=ok
session_written=ok
panel_grab=ok
panel_grab_size=1500x975
panel_rows=1
panel_row=https://example.org | expired | sweep | 7 | lejárt
session_panel=ok      (exit code 0)
```

The test first **writes a row** into the service's store (on the same wire the shell uses),
then opens the window's panel and reads back the **panel's own text**: the row and the
policy judgment (`lejárt`) must both be there. The window is also saved to PNG (`panel.png`), because an
empty table is correct in the model, but not in the image.

### Still open

- **Execution**: the `clear_origin_data` engine op is still `not_supported`, so the panel only
  shows what would be due; the "clean up now" button does not exist.
- Editing the rules (`SessionRules`) on the panel (24 hours, `wipe_on_app_lock`) is still missing.
- `LossEstimate` (what the user loses with a wipe) is still not shown in the rows.

## 2026-11-17 (twenty-second slice) — the session ledger in the core (24-hour policy)

The `keep`/`sweep`/`wipe` logic had so far been just a library (`antikolt-core::session`, 13 tests),
which nothing called. This slice wired it into the core service — the **core** makes the decision, the
shell only executes (ADR-033).

### What was built

| Area | Content |
|---|---|
| Store | `crates/antikolt-core-service/src/sessions.rs`: `sessions.json` (a **separate file** from the card store, atomic write), `SessionStore::{open,rules,list,upsert,close,due}`, `DueEntry` (row + its `CleanupPlan`) |
| Ledger rule | `validate()` runs **before the write**: a false row cannot reach the disk (`Rejected` → `invalid_argument` on the wire) |
| Wire ops | `session_list`, `session_upsert`, `session_close`, `session_due` (the `now_ms` and the `app_locked` are the caller's) |
| Shell | `CoreClient::{sessionList,sessionUpsert,sessionDue,sessionClose}` |
| Self-check | `--coretest` walks the ledger after the card: write → list → due (with a clock of 2100) → close → empty list |

### Evidence (the actual output)

```
cargo test -p antikolt-core-service
test sessions::tests::a_fresh_ledger_is_empty_and_survives_a_reopen ... ok
test sessions::tests::the_ledger_refuses_rows_that_would_make_it_lie ... ok
test sessions::tests::a_row_is_due_only_after_its_ttl_and_a_kept_row_is_never_due ... ok
test sessions::tests::closing_a_row_removes_it_once ... ok
test result: ok. 7 passed   (+ 2 integration tests: ok)
cargo clippy -p antikolt-core-service --all-targets -- -D warnings   → clean

antikolt-shell --coretest <core>
session_upsert=ok
session_rows=1
session_due_rows=1
session_close=ok
rows_after_close=0
session_ledger=ok      (exit code 0)
```

The `session_due_rows=1` is the point: the service does **not** compute in the shell — the 24-hour rule is given by
`antikolt_core::session::plan_cleanup`, and the plan (`CleanupPlan`) comes back together with the row, so
the caller cannot delete the wrong set of storages.

### Still open

- The `clear_origin_data` engine op is still `not_supported`, so **executing the plan** (the
  actual `wipe`) does not work yet: the decision is ready, the execution is the next slice.
- The rules (`SessionRules`) cannot be set on the wire yet (there is no `session_rules` op); the
  `sessions.json` starts with the library's defaults.
- The **Sessions panel** (`10-MUNKAMENET.md` §5) is still missing; the bridge is ready for it.

## 2026-11-17 (twenty-first slice) — the pinned card stays alive, measured

The twentieth slice wired in the `pinned` flag, but left it unmeasured. The measurement (`--lifecycletest`
phase 2) brought two things: a **real bug** in the shell, and a **fact about the core's data**.

1. **Bug:** `live_card_pinned_` was set at the bridge call, but during the card swap the **grid**'s own
   `detach` erased it — the flag was lost, the pinned card also hibernated (`pinned_closed=4`,
   `pinned_honoured=FAILED`). Fix: the flag waits in `pending_pinned_`, and is transferred **on the card surface's
   attachment** (`surfaceAttached`, non-home branch).
2. **Fact:** among the core's seeded cards the first ("Antikolt project") is **pinned** — so the test's
   phase 1, which clicked the first grid card, failed after the flag was turned on
   (`hibernated=FAILED`). The test now looks for a **non-pinned** card in the grid, and phase 2
   specifically opens a pinned one.

### Evidence (the actual output)

```
antikolt-shell --lifecycletest <motor>
card_surface=2
card_closed=2                                       ← the non-pinned card hibernates
grid_back=file:///D:/Szerver/Antikolt/home/index.html
hibernated=ok
grid_returned=ok
pinned_open=sent
pinned_surface=4
pinned_honoured=ok                                  ← the pinned card is alive even after 4 clock ticks
exit code 0
```

The `--gridtest` is still green (`grid_matches_core=ok`, `grid_click_open=ok`). The lesson is the same
as in the earlier slices: **carrying the flag through is not proof** — only that the two branches, measured,
behave differently.

## 2026-11-17 (twentieth slice) — one command for every check

The six self-tests had so far been six separate, memorized commands — and a session that forgets one
reports a green state it did not actually measure. Now there is `scripts/verify.ps1`: build + `--gridtest`,
`--lifecycletest`, `--opentest`, `--embedtest`, `--capturetest`, `--coretest` + `check-repo.ps1`, and at the
end a table and a single `ALL SELF TESTS PASSED`. It reports a missing binary as **FAIL**, it does not
silently skip it; `-SkipBuild` / `-SkipRepo` for partial work.

Measured:

```
Check         Result Detail
-----         ------ ------
gridtest      ok
lifecycletest ok
opentest      ok
embedtest     ok
capturetest   ok
coretest      ok

ALL SELF TESTS PASSED
```

This script is not a gate (the CI gate remains `check-repo.ps1`), but the command a
session runs before making its claims.

## 2026-11-17 (nineteenth slice) — the card goes to disk on its own (10 minutes of inactivity)

The essence of the memory promise had not been wired in yet: freezing only worked on a manual button. Now the shell
keeps an **inactivity clock** over the open card: 10 minutes by default (per `02-ARCHITEKTURA.md` §4.2),
it can be shortened with `ANTIKOLT_HIBERNATE_MS` (for the test). Activity counts as the user's **mouse and
keyboard events** (anywhere in the shell window), not the engine's traffic.

On timeout, the shell first asks the page whether it is **playing media** (the documented exception), and if
not, then snapshot → `destroy_surface` → **the grid comes back**. It re-requests the grid only after the `destroy_surface`
*acknowledgment*: if it asks for a new surface earlier, the engine rejects it with `engine_busy` (the measurement
showed this, see below).

### Evidence (the actual output)

```
antikolt-shell --lifecycletest <motor>
card_surface=2
card_closed=2
grid_back=file:///D:/Szerver/Antikolt/home/index.html
hibernated=ok
grid_returned=ok      (exit code 0)
```

The engine's log from the same run (on the shell's stderr, in chronological order):

```
op=eval        → bridge_call surface=1 method=open_card
op=snapshot    → snapshot written: 17065 bytes MHTML, 411 bytes state
op=destroy_surface surface=1
op=create_surface → surface=2 → navigate https://github.com/antikolt/antikolt
   … 1.5 s inactivity (the test's clock) …
op=eval        → '(media?)' → idle
op=snapshot    → snapshot written: 4419063 bytes MHTML, 526 bytes state
op=destroy_surface surface=2
op=create_surface → surface=3 → home (the grid came back)
```

### Two observations that also affect the next slices

1. **The engine loads a real HTTPS page** (github.com loaded, with a 4.4 MB MHTML), while the
   `curl` and `cargo` TLS on this machine fail (`schannel SEC_E_NO_CREDENTIALS`). CEF uses its own
   TLS stack, so **the browser is capable of the real web** — the network-dependent tests (and
   web development) are no longer to be avoided from now on; only the crate download is blocked.
2. **`create_surface` cannot precede `destroy_surface`**: the grid's re-request must be made after the destroy
   acknowledgment, otherwise `engine_busy`. This is also true for the earlier (non-hibernation) card switch, where it
   did not collide by accident.

### Still open

- The **pinned card exception** is not wired in yet: the core knows it (`pinned`), the hibernation
  does not ask about it for now.
- The **24-hour `keep`/`sweep`/`wipe`** policy and the session ledger are still missing; the bridge's
  `cards.upsert` (a new card from the page) too.
- The media-exception code is ready, but it **has no test** (there is no media-playing card in the set).

## 2026-11-17 (eighteenth slice) — the shell's log is visible

It was the fourteenth slice's open item: "the engine's `stderr` is not visible under the shell (the `qInfo`
path does not reach the console)". This slice wired it in: the shell writes the Qt messages to **stderr** with `qInstallMessageHandler`,
and forwards the engine's `stderr` through `qInfo` — so the engine's log
is readable in the same terminal where the shell runs.

Measured, during a `--gridtest` run on the shell's output (not with a separate probe):

```
[shell] info: [engine] [engine-host] op=eval id=9 surface=1
[shell] info: [engine] [engine-host] bridge_call surface=1 method=open_card
[shell] info: [engine] [engine-host] op=snapshot id=10 surface=1
[shell] info: [engine] [engine-host] state: 411 bytes written
[shell] info: [engine] [engine-host] snapshot written: 17065 bytes MHTML, 411 bytes state
[shell] info: [engine] [engine-host] op=create_surface id=12 surface=0
```

This visibility is not a matter of convenience: the sixteenth slice's two bugs (the rejected module script,
`ShadowRoot.dataset`) could be found **only** because the page's `console` ended up in the engine's log —
and until then that log was lost under the shell.

## 2026-11-17 (seventeenth slice) — clicking a card actually opens the card

The sixteenth slice left it as an open question: the synthetic click runs, but does not reach the page's
handler. There were two separate bugs, both measured.

### 1. The pinned row's cards were dead

The DOM chain (measured with the engine's `eval`):

```
chain: ["ANTIKOLT-CARD", "pinned", "pinned-section", "MAIN", "BODY", "HTML"]
insideViewport: false
```

The click listener was delegated on `#viewport`, but the **pinned row lives outside it**
— so the most conspicuous cards did not react to clicks at all. Fix: the delegation is on the
`document`, the `closest(".card, antikolt-card")` decides what was hit. One listener
for both places.

### 2. The page opened the sample card, because it decided about the bridge at startup

The fixed listener already ran, but the engine's log showed this (the page's `console`):

```
console level=2 file:///…/home/app.js:101 [mock] open card 01MOCK… https://mail.google.com/
```

So the click called the **sample's** `cards.open`. The cause: the page decided at script startup
whether `window.antikoltNative` exists — but the engine injects it **after** loading, so the page
always stuck to the sample branch. Fix: the bridge is decided **at call time** (`withNativeOpen` →
`nativeBridge()`), and if there is no native bridge (the file was opened in a plain browser), it falls back
to the sample behavior.

### Evidence (the actual output)

```
antikolt-shell --gridtest <motor>
core_cards=3
grid_source=core
grid_count=3
grid_card=Antikolt project / Hacker News / Wikipedia — homepage
grid_matches_core=ok
grid_click_url=https://github.com/antikolt/antikolt
card_click=clicked
card_surface=2
grid_click_open=ok      (exit code 0)
```

From the engine's side the same: `bridge_call surface=1 method=open_card`, twice (on the pinned row's card and the grid
card too) — the probe measured both separately.

### Still open

- The click phase in `--gridtest` is now an **expectation**, not an observation: if the bridge or the delegation
  breaks, the test fails.
- The request direction (the page asks: search, refresh, new card) is still protocol v2 (ADR-032).

## 2026-11-17 (sixteenth slice) — the grid shows the core's cards (and the page's script had not run until now)

The grid had so far shown the home page's own **sample cards**. This slice wired in the core's card store — and
in the process it turned out that the home page's script **had not run in the engine at all**.

### What was built

| Area | Content |
|---|---|
| Shell → page push | after the home page loads, the shell asks the core for `cards_list` and passes it **as data**: `window.antikoltAcceptCards(JSON.parse("…"))`, the JSON is an escaped string literal (ADR-032) |
| Page | `pageCardFromCore` adapter (core record → grid model), page-side search on the loaded list, `window.antikoltGridState()` — the page's own witness of what it shows |
| Shell | `MainWindow::pushCardsToGrid` + `home_surface_` (only the home page gets a list); `cards.open` takes the URL from the list shown on the page, not from the mock |
| Self-check | `antikolt-shell --gridtest <motor>`: the list requested from the core is the oracle, the page confesses through `antikoltGridState` |
| Engine | `OnConsoleMessage` → `stderr`: the page's `console` and the JS errors are now visible (until now they stayed in the renderer's console) |

### Evidence (the actual output)

```
antikolt-shell --gridtest <motor>
core_executable=D:/Szerver/Antikolt/target/debug/antikolt-core-service.exe
core_start=ok
core_name=antikolt-core
core_cards=3
home_loaded=file:///D:/Szerver/Antikolt/home/index.html
grid_source=core
grid_count=3
grid_card=Antikolt project
grid_card=Hacker News
grid_card=Wikipedia — homepage
grid_first=Antikolt project
grid_matches_core=ok      (exit code 0)
```

The other self-checks are still green: `--embedtest` `embed/paint/focus/viewport_match=ok`,
`--opentest` `card_opened=ok`, `--capturetest` `capture=ok capture_size=ok`, `--coretest` `cards=3
card_open=ok`.

### The bug this slice found (and the reason the grid was empty)

`home/index.html` loaded the script **as a module** (`<script type="module" src="app.js">`), but the
engine opens the page from `file://` — and Chromium **rejects the module script from a file: origin because of CORS**.
So the page's script had never run in the engine; the "sample grid" only
existed if someone opened the file by hand in a browser. Measured, before and after the fix
(the same probe, `window` diagnostics):

```
before:  {"accept":"undefined","state":"undefined","native":"object","cards":0}
after:   {"accept":"function","state":"function","native":"object","cards":16,
          "grid":{"source":"sample","count":28,"firstTitle":"Google — work",…}}
```

Fix: a classic, `defer`-ed script (`<script src="app.js" defer>`); `app.js` never used
ESM syntax, so only the loading path of the behavior changes. **Correction to the tenth slice:**
there, "the grid asks for cards through the bridge" was actually the self-check's `eval` call, not the page's
own code — the page's code had not run then. The second lesson is diagnostics: the engine from now on
logs the page's `console` messages, because without this a dead script looks exactly like an empty grid.

### Second bug: the card render failed on every card

The page's `console` (which the new `OnConsoleMessage` now prints) gave the same thing 33 times:

```
console level=4 file:///D:/Szerver/Antikolt/home/app.js:250
  Uncaught TypeError: Cannot read properties of undefined (reading 'signature')
```

`AntikoltCard.render()` read `this.shadowRoot.dataset` — but `ShadowRoot` is a
`DocumentFragment`, not `HTMLElement`, so it **has no `dataset`**: the error arose at the very *beginning* of the render,
so **not a single card was drawn** (the grid consisted of empty `antikolt-card` elements).
The sign is visible in the capture size: the home page's image was **23 KB** before the fix and **264 KB** after
(the same 1104×742 image). The signature now goes on the host element (`this.dataset`), where it must be.

This bug was present in the earlier slices too: the "grid renders" claim only applied to the number of DOM elements,
not to the visible content. The lesson is the same as with the module script: **without the page's
console messages the bugs are invisible**, so the engine logs them from now on.

### Still open

- **The request direction** (the page asks: search, refresh, new card) comes with the persistent `cefQuery`, and
  with the `bridge_result` message it is **protocol v2** (ADR-032); until then the push remains.
- **The click path** was solved in the seventeenth slice (two separate bugs: the delegated listener was on
  `#viewport`, but the pinned row lives outside it; and the page decided about the bridge at startup,
  before the engine injected it). The `--gridtest` click phase returned there, now measured.
- **The page's `cards.open` opened the sample card** — the same startup decision; the details and the
  fix are in the seventeenth slice.
- The grid's **previews** do not exist yet (the `capture` primitive is ready, the 260×150 WebP pipeline is not).
- Editing/tagging/pinning cards on the page is still missing: the core knows it (`cards_upsert`), the bridge
  does not lead a path to it.

## 2026-11-17 (fifteenth slice) — the vault's shape is ready, encryption is blocked

Per ADR-027's order, the vault comes after the DevTools layer. This slice built the vault's **shape**
— and, measured, established that **encryption** cannot be built in this environment. There was no
hand-written AES in its place, and no silent plaintext storage: the seam behaves closed (ADR-031).

### What was built

| Area | Content |
|---|---|
| `crates/antikolt-core/src/vault.rs` | `Vault` pledge: open/close, 5-minute idle auto-lock (`tick` signals exactly once), `touch`, `require_open` gate, `open_for_ms`; `SecretKind` (`password`/`totp`/`passkey`, strict parse); `VaultStatus` (never contains a secret); `VaultError` with the wire codes (`vault_locked`, `not_supported`, `internal`) |
| Encryption seam | `SecretBox` trait: `seal`/`open` with the **record's AAD** (origin + identifier, ADR-010) + `available()`. A single implementation in the tree: `UnavailableSecrets`, which refuses everything and states the reason |
| Tests | 10 new unit tests: 4:59 open / 5:00 closed (boundary-inclusive), activity restarts the clock, a clock stepping backward does not extend, the seam never returns plaintext, the `status` contains no secret |

### Evidence (the actual output)

```
cargo test --offline
  antikolt-core (lib)       77 passed   (from 67; +10 the vault)
  antikolt-core-service      3 passed
  antikolt-ipc              14 passed
  protocol/integration       2 + 1 passed
```

### The blocker, measured (not an opinion)

```
$ curl -sS -m 20 -o config.json https://index.crates.io/config.json
curl: (35) schannel: AcquireCredentialsHandle failed: SEC_E_NO_CREDENTIALS (0x8009030e)

$ ls ~/.cargo/registry/src/index.crates.io-*/ | wc -l
14        # serde, serde_json, thiserror and their dependencies — no cryptographic crate among them
aes-gcm, argon2, sha2, zeroize, rand_core, getrandom, rusqlite, windows, winapi   → MISSING
```

The crate is `#![deny(unsafe_code)]`, so the platform key store (DPAPI) cannot be reached even via FFI. Writing AES by hand
is not an option. So the vault **cannot store secrets** today, and this is stated, not hidden, by `VaultStatus.can_store = false` +
`unavailable_reason`.

### Still open (exact order)

1. **Vendoring the crates** (`cargo vendor` on a machine with network): `aes-gcm`, `argon2`,
   `zeroize` (+ the `rand_core`/`getrandom` chain), then an `AesGcmSecrets: SecretBox` implementation with the
   record AAD, and around the DEK the system key store (DPAPI, enabling `unsafe` in a single, isolated
   module, or with the `windows` crate).
2. Then the vault's **storage**: the `credential` table (the schema is already in `0001_init.sql`), an
   atomic write following the `pending_maintenance` pattern.
3. Then the **passkey**: `WebAuthn.enable` / `addVirtualAuthenticator` / `getCredentials` /
   `setCredential` — all on the now-ready `DevToolsCall` type (ADR-029), the credentials in the vault.
4. Then TOTP (RFC 6238) and autofill (isolated world, `Page.createWorld`).

## 2026-11-17 (fourteenth slice) — `capture`: the engine gives its own image

The thirteenth slice stated that painting must not be measured from the screen. This slice built the
layer needed for it and the measurement itself: the **DevTools layer goes through a single type**, and the `capture`
protocol op asks for an image from the engine's compositor.

### What was built

| Area | Content |
|---|---|
| `apps/engine-host/src/devtools.{h,cpp}` | **`DevToolsCall`**: method + parameters + reply callback, with a unique message id; the callers chain the sequences. **`DevToolsReply`**: the answer parsed once (`success`, `error`, `from_protocol`, `result`, `raw`), and it **owns the parsed JSON document** (ADR-029) |
| Engine | `MhtmlCapture` and `EvalCapture` switched to the common type (behavior-identical, ~180 lines of duplication removed); new `ScreenshotCapture`; the answer parsing lives in one place |
| `capture` op | `Page.captureScreenshot` (`png`/`jpeg`/`webp`, `quality` 0..100, `fromSurface:true`, `captureBeyondViewport:false`); the file `<user_data>/screenshots/shot-N-M.<fmt>`, the answer: `{ok, path, bytes, width, height, format, surface}` — the **width/height from the image header** (PNG/JPEG/WebP), not computed from the viewport |
| Shell | `EngineClient::capture`; new self-check `--capturetest <motor>`; `--embedtest`'s paint measurement **switched to capture** (the screenshot stays as a `screenshot_colours=… (informational)` line, but does not decide a transition) |

### Evidence (the actual output)

`antikolt-shell --capturetest <motor>` — the card on a real, embedded surface, on a page with three color bars:

```
engine_start=ok
capture_capability=announced
surface=1
load_status=200
page_css=700x460
dpr=1.25
expected_physical=875x575
capture_path=C:/Users/info/AppData/Roaming/Antikolt Project/Antikolt/capture-selftest\screenshots\shot-1-0.png
capture_bytes=2775
capture_size=875x575
capture_image=875x575
capture_colours=4
capture=ok
capture_size=ok      (exit code 0)
```

So the image is the **page's own viewport × its device scale** (700×460 CSS → 875×575 device px), and the three
bars + the white background are really there in the image.

`antikolt-shell --embedtest <motor>` — the same measurement on the embedded card, together with the other measurements:

```
page={"w":900,"h":640,"dpr":1.25,"screenW":4096,"screenH":1152,"outerW":0,"outerH":0}
screenshot_colours=20 (informational)
capture_colours=25
capture_size_match=ok
page_has_focus=true
embed=ok  viewport_match=ok  paint=ok  focus=ok      (exit code 0)
```

The `--opentest` (opening a card across the bridge) is green again: `bridge=object`, `open_card=ok`, `card_opened=ok`;
the `--coretest`: `cards=3`, `card_open=ok`. The `scripts/check-repo.ps1`: **ALL CHECKS PASSED** (46 SPDX
source files, link check, `node --check`, `cargo fmt`/`clippy -D warnings`, 87 Rust tests).

The protocol probe (a manual stdio client) also measured the formats and the error path on the same surface:

```
capture png  -> {"bytes":2495,"format":"png","height":442,"width":779,"ok":true,...}
capture webp -> {"bytes":1704,"format":"webp","height":442,"width":779,"ok":true,...}   (quality=70)
capture gif  -> {"code":"invalid_argument","message":"format must be png, jpeg or webp"}
snapshot     -> {"bytes":1189,"mhtml":true,"state_bytes":390,...}
```

The content of `state.json` (the second half of the snapshot, which had remained empty until now):

```json
{"url":"data:text/html,…","title":"probe","scrollX":0,"scrollY":0,
 "anchor":{"selector":"html > body:nth-of-type(1) > div:nth-of-type(1)","dy":0},
 "forms":[],"session":{},"media":[],"viewport":{"width":623,"height":354,"dpr":1.25}}
```

### Three bugs this slice introduced (all fixed)

1. **The `DevToolsReply` returned an empty dictionary** (`type=''`, `value=''`, `state_bytes=0`), while the raw
   response was correct: `{"result":{"type":"string","value":"…"}}`. CEF's internal dictionaries live in the parsed
   JSON-**document**; when the document was freed, the dictionary came back *empty* —
   it didn't error, it lied. This is exactly what the earlier code's comment described ("a real bug that cost a
   debugging round"), and now it resurfaced **a second time** when the document's lifetime moved from the caller to this
   type. The fix: the response is a **member** of the document (`document`), not a local variable.
2. **The snapshot's `state.json` had silently remained empty until now** (`state_bytes=0`). The same bug;
   after the fix `state_bytes=390`, `state: 390 bytes written`.
3. **In a `data:` URL, `#` counts as a fragment**: the first version of `--capturetest`
   wrote `background:#06c`, the page got cut off at the `#`, and the image became a single colour (`capture_colours=1`).
   Percent-encoded (`%2306c`) it's 4 colours. This kind of trap is itself a source of error in the measurement.

Incidentally: `QScreen::grabWindow` is not `const`, so the diagnostics helper receives a `QScreen*`.

### A measurement observation that obligates the next slice

The **standalone, covered** engine window (`parent_window=0`, behind the console window) had a **single-colour** `capture`
(`capture_colours=1`), while the embedded, visible card had 25 colours. So the compositor does not paint a covered
window. Consequence: **preview generation must be given a visible surface** (or it must use the
`fromSurface:false` path), otherwise the grid would get silent, empty previews. This is recorded in
ADR-030.

### What is still open

- The **preview pipeline** (260×150, WebP q=80, blob store + luminance/dhash, 100 refreshes per day) has not been
  built yet: the `capture` primitive is done, the processor is not.
- `capture` has no `from_surface` switch, and there is no `metrics` op yet (the `capture` image size is now the
  measurement; the DOM-node-count/spread probes come later).
- The **vault** is the next slice (ADR-010, ADR-027): the DevTools layer is already ready for it, and the passkey
  calls (`WebAuthn.*`) will go through this same `DevToolsCall` type.
- The engine's `stderr` is not visible under the shell (the `qInfo` path doesn't reach the console), so the bug was
  exposed by a manual, PowerShell-based stdio probe that showed the raw frames and the engine's stderr too.
  It's worth wiring up this channel (or using `crates/antikolt-probe` for it).

## 2026-11-17 (thirteenth slice) — the paint measurement is unreliable: `capture` is needed

The `--embedtest` paint check makes a copy from the screen (`QScreen::grabWindow`) and
requires that it not be a single colour. This is **environment-dependent**: the same binary, the same code gives
`screenshot_colours=25` (green) in one run and `colours=1` (fails) in another, while the geometry and
the focus are correct throughout:

```
embed=ok  viewport_match=ok  focus=ok     — the page sees 900×640 CSS pixels, the focus goes through
paint=FAILED                              — the image taken from the screen is a single colour
```

A retrying measurement (6 attempts, 200 ms) didn't improve it either, so I **withdrew** the experiment — a
"sometimes green" test is worse than one that fails honestly.

**Decision:** painting must not be measured from the screen. An image must be requested inside the engine
(CDP `Page.captureScreenshot`), which is independent of the compositor, deterministic, and proves the same thing.
This will be the implementation of the `capture` protocol op — and this same path will also provide the grid's
**preview images**, so it's not a test aid but a product feature.

Until that exists, the `paint` check is a **known uncertainty**: the other measurements (`embed`,
`viewport_match`, `focus`) remain valid independently of it.

## 2026-11-17 (twelfth slice) — the shell talks to the core

A new `CoreClient` in the shell: it starts `antikolt-core-service`, handshakes, card list, card opening —
with the same framing as the engine link. `antikolt-shell --coretest <mag>`:

```
core_start=ok
core_name=antikolt-core   core_version=0.1.0
cards=3
first_id=01SEED000000000000000001
opened_at_ms=1.7949e+12
core_ready=ok  cards_list=ok  card_open=ok      (exit code 0)
```

One bug: `welcome` is its own message type, so it echoes the `hello` **request id** in the `to` field —
without this the client waited forever (10 s timeout, then "timed out waiting for the core's reply").

**What is still open:** the grid doesn't yet show this list (the bridge's `cards_list` response, with a persistent
`cefQuery`), and the shell doesn't yet start the core automatically when the main window launches.

## 2026-11-17 (eleventh slice) — the core in its own process: cards from a real store

Until now the shell didn't talk to the Rust core: the card list was the home page's mock. Now the core started
**as its own process** (`antikolt-core-service`), and it serves real cards.

| Area | Content |
|---|---|
| `antikolt-ipc` | `encode_json_frame` / `decode_json_frame`: the length-prefixed framing now works not only for the `Envelope`, but also for the core's smaller message set — the strict input rules live in one place |
| `antikolt-core-service` | `hello/welcome`, `cards_list`, `card_open` (timestamp + reordering), `card_upsert`, `shutdown`; the store is `cards.json`, with atomic writes (temp file → rename), 3 sample cards on first launch |
| Tests | 3 store unit tests + 2 integration tests that start a **real process**, ask over a real stdin/stdout link, and still see the saved timestamp after a restart |

The store is JSON for now, not SQLite: `rusqlite` is unavailable in the offline environment, but the migration schema
(`antikolt_core::migrations`) already exists, and the card's shape is decided in one place — so switching to SQLite
will be a one-file change, not an API change.

**What is still open:** the shell still doesn't start the core; the bridge's `cards.list` response (persistent
`cefQuery`, the shell → core → page path) is the next slice's main item. Until then the grid works from the mock,
but card opening is already real.

## 2026-11-17 (tenth slice) — the home page opens a card on click

The product's loop now works **end to end**: the home page (the grid) requests a card across the bridge, the
shell hibernates the grid, and opens the requested page as a live card.

### What was built

| Area | Content |
|---|---|
| Engine (C++) | CEF `CefMessageRouter` on both sides (render + browser); the `install_bridge` command; the bridge script lives in the engine; the `bridge_call` event; the bridge reinstalls after every main frame load; the unauthorized surface rejects every request |
| Rust contract | `EngineEvent::BridgeCall { surface, method, args }` (the `args` is intentionally opaque) + a roundtrip test |
| Shell | `EngineClient::installBridge`/`bridgeCall`; `MainWindow::openCard`: snapshot → destroy → **new** surface with a new partition → navigate; only the home page gets the bridge |
| Home page | `withNativeOpen(...)`: if `window.antikoltNative` exists, card opening is real, the list still comes from the mock for now |
| Self-test | `antikolt-shell --opentest <motor>`: the bridge's presence, the call, and the new surface are all checked |

### Evidence (the actual output)

```
load_finished surface=1 status=200 url=file:///D:/Szerver/Antikolt/home/index.html
bridge=object
open_card=sent
load_finished surface=2 status=200 url=data:text/html,<title>card2</title><h1>card two</h1>
first_surface=1 second_surface=2 live=2
home_loaded=ok  bridge=ok  open_card=ok  card_opened=ok      (exit code 0)
```

That is: the grid's surface **1** was hibernated (snapshot to disk), and the requested card opened as surface
**2**, with a **new partition** — not by navigating in the same surface.

### Two bugs this slice introduced (all fixed)

1. **`CefMessageRouter` only exists if the render process also receives it** (`CefRenderProcessHandler`
   `OnWebKitInitialized` + `OnContextCreated`). Without this, `window.cefQuery` is not a function, and the bridge
   looks exactly like a broken home page.
2. **The shell crashed on shutdown (0xC0000005).** The cause: after the `MainWindow` destructor, the engine
   client still fired a pending response callback that wrote into an already half-torn-down window. Now the
   client sets a `destroying_` flag in its destructor, clears pending calls, and blocks
   signals — and the shutdown order is: close surface → `shutdown()` (this is the barrier) → Qt teardown.

### What is still open

- The bridge is intentionally **one-way**: the page doesn't wait for a reply from the shell (the `cefQuery` acknowledges immediately).
  The persistent query (e.g. "did it open?") is left to the freezing slice.
- The card list is still the home page's mock; the core's SQLite card table and the `cards.list` bridge are not
  wired up yet — that's the next slice's main item.
- Card switching **saves to disk** in every case (correct, but slower than an in-memory switch);
  the "fast switch" optimization comes later.

## 2026-11-17 (ninth slice) — the card really lives in our own window

Until now the engine showed the page in its **own** window, and the shell drew a placeholder. This
slice breaks through that wall: the engine draws **embedded** in the shell's window, while remaining in a separate process.

### What was built

| Area | Content |
|---|---|
| Protocol | `create_surface` `config.parent_window` (native window handle, `0` = own window); new `set_bounds` and `focus` messages; the unit is **DIP** (the page's CSS pixels), not physical pixels |
| Rust contract | `SurfaceConfig.parent_window` (default `0`, so an additive, version-stable change), `Rect`, `Message::SetBounds`, `Message::Focus`; 2 new tests (accepting the old `create_surface`, the signed origin of `set_bounds`) |
| Engine host (C++) | `CefWindowInfo::SetAsChild(parent)`, DIP→physical conversion with the monitor's DPI, `ApplyPendingEmbeddedBounds` in the pump loop, **never** tears down the parent window |
| Shell (Qt) | `LiveCard` native widget: `winId()` to the engine, `set_bounds` on every resize, focus handoff; the main window uses this instead of the placeholder |
| Self-test | `antikolt-shell --embedtest <motor>`: the embedded child window and the page's viewport are both checked |

### Evidence (the actual output)

```
card_screen_origin=150,151
set_bounds=ok applied=1
load_status=0
child_windows=2
child=Chrome_WidgetWin_1 window=1125x800 at 150,151 client=1125x800
child=Chrome_RenderWidgetHostHWND window=1125x800 at 150,151 client=1125x800
chromium_child=found
card_logical=900x640
card_physical=1125x800
dpr=1.25
page={"w":900,"h":640,"dpr":1.25,"screenW":4096,"screenH":1152,"outerW":900,"outerH":640}
embed=ok
viewport_match=ok        (exit code 0)
```

That is: Chromium's window is **exactly at the card's position and size** (1125×800 physical = 900×640
DIP at 125%), and the page sees **900×640 CSS pixels** — no more. So the image goes into the card, not into a
separate window.

However, geometry is not yet proof that **painting actually happens**: a window can be in the right place and
the right size and still be empty. So the self-test also takes a screenshot of the card's area
(`QScreen::grabWindow(card->winId())`) and requires that it not be a single colour:

```
screenshot=C:/Users/info/AppData/Local/Temp/antikolt-embed.png
screenshot_size=1125x800
screenshot_colours=25
paint=ok
```

In the capture the `<h1>embedded</h1>` text is readable in the card's area, without a window frame and browser chrome
— that is, the engine really paints into our window, it didn't open in a separate window.

The fourth link is **focus**: another process's child window doesn't inherit activation, so the
shell sends a `focus` message, and the page must agree:

```
page_has_focus=true
embed=ok  viewport_match=ok  paint=ok  focus=ok        (exit code 0)
```

### Four bugs this slice introduced (all fixed, all measured)

1. **The embedded rectangle's unit is DIP, not physical pixels.** Sending physical pixels made Chromium
   scale up a *second* time: for a 900×640 card the window became 2756×1622, the page saw a 1575 px viewport,
   and the height could be cut off by the screen's work area. The log states the boundary: the
   `SetAsChild` and `set_bounds` are to be understood **in DIP**, the DIP→physical conversion is the engine's job (only it knows
   the monitor's scale). *This is the most important lesson: the wrong unit doesn't cause discolouration, it
   silently causes a wrong layout.*
2. **The CEF child window's class name is `Chrome_WidgetWin_1`**, not `_0`. The search filtered to `_0` silently
   returned `nullptr`, so `set_bounds` never found the surface — the size stayed correct, but the
   window ended up off-screen. Now there's a prefix match (`Chrome_WidgetWin_`).
3. **`set_bounds` often arrives before Chromium's window.** `create_surface` answers
   when the browser still only exists at the *request* level. So the rectangle is stored, and
   the pump loop places it as soon as the child window exists (`applied: false` = "remembered", not an error).
4. **The test window ended up off-screen** because I didn't specify its position: so the child window's
   `-2066,198` coordinate looked like an error, although CEF was working correctly. The measurement now gives an explicit
   position (`window.move(120, 90)`), and compares the card's physical screen origin (`ClientToScreen`) with
   the child window's `GetWindowRect`.
5. **The `eval` response returned only text.** The boolean value of `document.hasFocus()` arrived empty
   (`page_has_focus=`), because the engine read the DevTools result with `GetString("value")`.
   Now the primitives (boolean, integer, fraction) become their JSON text, and `value` remains
   always text on the protocol — so the response's shape did not have to, and must not, change.

### What is still open

- The embedded surface's focus is **measured** to work (`page_has_focus=true`), but I haven't yet sent a real key press
  through the child window — the first interactive test will be in the next slice.
- Card switching (save → destroy → new surface) works via the shell's `❄ Freeze` button, but
  "opening another card" does not yet.
- The GPU must still be disabled in this environment (`ANTIKOLT_ENGINE_DISABLE_GPU=1`), otherwise
  the GPU process crashes; this also affects the measurements.

## 2026-11-17 (eighth slice) — the loop closed: save → restore offline

The browser's most important promise is now proven **end to end**: saving a page to disk and **restoring it
offline** with the same engine, without a network.

### What was built

| Area | Content |
|---|---|
| `eval` command (C++) | DevTools `Runtime.evaluate {returnByValue}` → `{type, value}` response; the restore is built on this |
| `antikolt-probe --restore <state.json>` | opening the MHTML at a `file://` address (offline), then writing the state back: scrolling, form values (with `input`/`change` events), `sessionStorage`, media position |
| Verification | after the restore a second `eval` confirms the address, the scroll, the field count, and the displayed text |

### Evidence (the actual output)

```
pre-scroll: scrollY=600
snapshot bytes=1440193 state_bytes=4452 ...
state loaded: title="Webböngésző – Wikipédia" scrollY=600 forms=18
navigate=file:///C:/…/surface-1-0.mhtml
load_finished status=Some(200) url=file:///…/surface-1-0.mhtml
restore applied: forms=18 session=0 media=0 scrollY=600
verified: title="Webböngésző – Wikipédia" url=file:///… scrollY=600 fields=18
          text="Ugrás a tartalomhoz\nFőmenü\nKeresés\nAdományok\n…"
probe=ok        (exit code 0)
```

That is: **the page's text appears offline**, the **scroll (600 px) is restored**, the **18 form fields
get their values back** — with the engine, but without a network.

### Two more bugs this slice introduced (all fixed)

1. **The DevTools message id must be unique.** I used a constant id, so the *second*
   `Runtime.evaluate` response was silently lost — it looked exactly like a stuck engine. Now
   every DevTools call gets its id from the `NextDevToolsId()` counter, and matches the response to its own id.
2. **PowerShell-reserved variable names**: after `$host`, `$profile` also failed (my script was at fault, not
   the program). The measurement scripts now deliberately avoid these.

### Honest limitation

The restored MHTML is a **static document**: the DOM, the styles, the images, and the form values are present,
but the page's own JavaScript doesn't run again as a "live" SPA. So the restore is good for **reading and
resuming**, not for exactly continuing a complex web application's runtime state —
this was already stated at the start by section 5.1 of `02-ARCHITEKTURA.md`.

---

Saving a card to disk now writes **two files**: the full page (MHTML) and the state beyond the DOM
(JSON). This is the complete hibernation package.

### What was built

| Area | Content |
|---|---|
| `kStateExpression` (JS) | scroll (x/y), **anchor** (the topmost visible element's selector + offset), form fields (`input`/`textarea`/`select`, max. 2000 characters per field), `sessionStorage`, media position (`currentTime`, `paused`), viewport/DPR |
| `MhtmlCapture` state machine | `Page.enable` → `Page.captureSnapshot` → `Runtime.evaluate {returnByValue:true}` → two files, one response |
| Security rule | **password and hidden fields never go into the snapshot** — the snapshot is not a credential store |
| Response | `path`, `bytes`, `state_path`, `state_bytes`, `url`, `title`, `surface` |

### Measured/verified result (Hungarian Wikipedia article)

```
surface-1-0.mhtml       1 440 191 bytes   (From: <Saved by Blink>, embedded image + CSS)
surface-1-0.state.json      4 386 bytes
title=Webböngésző – Wikipédia
scrollY=0  viewport=1263x754  dpr=1,25
anchor=html > body:nth-of-type(1) > div:nth-of-type(2)
forms=18  session_keys=0  media=0
```

The 18 form fields are Wikipedia's search field and the page's other controls — the password fields were left out, as they should be.

### Another CEF-interop pitfall (worth remembering)

**`CefValue` owns its internal dictionary**: if a helper function returns the
`CefDictionaryValue` extracted from a `CefValue`, the dictionary is **emptied** when the `CefValue` goes away — `HasKey()` becomes false,
`GetKeys()` empty. This silently causes a "no data" error. The solution: keep the parsed `CefValue` alive
until we've read the dictionary out of it (which is why I removed the helper function).

---

The snapshot is no longer just the DOM: the **full page, with embedded resources**, in a single MHTML
file — this is what must really be preserved when saving a card to disk, because it can be read back
without a network and without the engine.

### What was built

| Area | Content |
|---|---|
| `MhtmlCapture` (C++) | CEF DevTools channel: `Page.enable` → `Page.captureSnapshot {format: mhtml}`; the response arrives in `CefDevToolsMessageObserver::OnDevToolsMethodResult`, with base64 detection (CEF gives the finished MHTML, other transfers give base64), then `<user_data>/snapshots/surface-N-M.mhtml` |
| Fallback | if the DevTools method can't be started, the old `GetSource` DOM save runs — there's never "no snapshot" |
| Diagnostics | the first 240 raw bytes of every DevTools response on stderr (the channel stays clean) |

### Measured result (2026-11-17, Wikipedia article)

| Phase | Median | Note |
|---|---|---|
| loaded | 179.0 MB | |
| during snapshot | 184.6 MB | the peak +5.6 MB: the MHTML is assembled in memory before it goes to disk |
| on disk (frozen) | 117.8 MB | |
| **RAM returned** | **61.2 MB** | 179.0 → 117.8 MB |

The snapshot file is **1 440 191 bytes**, its header is `From: <Saved by Blink>`, and the measurement verified that
it **contains an embedded image and CSS too** — so it can be displayed offline, without the engine.

### A lesson worth remembering

The DevTools `OnDevToolsMethodResult` returns the **finished MHTML** (not base64), and receives the **result
object**, not the `{"id":..,"result":{..}}` wrapper. These two small things silently cause a "no data" error,
so the code accepts both formats and logs the raw response.

---

The product's most important mechanism now **works and is measurable**: the engine dumps the page's DOM
to a file, the surface is destroyed, and the measurement shows how much RAM is freed.

### What was built

| Area | Content |
|---|---|
| `snapshot` command (C++) | `CefFrame::GetSource` → serialized DOM → `<user_data>/snapshots/surface-N-M.html`, then a response (`path`, `bytes`, `url`, `title`). The response is sent by `SnapshotVisitor` when the DOM arrives. |
| `capabilities` | the engine now also reports the `snapshot` capability |
| `antikolt-probe --freeze` | a process broken into phases (`loaded` → `snapshot` → `frozen` → `done`), with `phase=<name> ts=<ms>` markers |
| `perf/measure-freeze.ps1` | samples the process tree every 400 ms, computes the median per phase from the phase markers, and writes the report |

### Measured result (2026-11-17)

| Page | Loaded | On disk | RAM returned | Snapshot |
|---|---|---|---|---|
| `example.com` | 127.3 MB | 107.7 MB | **19.6 MB** | 2.4 KB |
| Wikipedia article | 182.6 MB | 115.9 MB | **66.7 MB** | 345 KB |

The measurement verified the snapshot file's content: real HTML with the `<title>Webböngésző – Wikipédia</title>`
title. The remaining ~116 MB is the **engine baseline** — this is what the T2/ultra-low mode returns by stopping
the engine host, exactly as planned.

### Open (for the next slice)

The snapshot is still **only the DOM** for now. Full disk hibernation still needs: MHTML with the embedded
resources, scroll position + anchor, form and `sessionStorage` state, media player position.
Without these, the restore is at the "static reading" level, not a full state restore.

---

## 2026-11-16 (fourth slice) — the shell ↔ engine-host protocol is **live**

This slice connects the browser's two halves: the Rust side (the future core) and the C++ engine host now
speak the same protocol that `crates/antikolt-ipc` defines.

### What was built

| Area | Content |
|---|---|
| `apps/engine-host/src/protocol.{h,cpp}` | stdio channel (4-byte length prefix + JSON), reader thread + command line, **with CEF's own JSON parser**; `hello`/`welcome`, `ping`, `create_surface`, `navigate`, `destroy_surface`, `shutdown`; events: `surface_created`, `surface_closed`, `load_finished`, `load_failed`, `title_changed` |
| `crates/antikolt-ipc/src/transport.rs` | `EngineLink`: starting a child process, framing, handshake, request–response matching with a time limit, event waiting |
| `crates/antikolt-probe` | a standalone binary that **carries through** the process: spawn → handshake → create_surface → navigate → load_finished → destroy → shutdown |
| Protocol extension | `Message::Shutdown`, `EngineEvent::SurfaceCreated/SurfaceClosed` (+ tests matched to the JSON forms) |

### Evidence (the actual output)

```
$ antikolt-probe antikolt-engine-host.exe https://example.com
spawned pid=32488
engine=cef version=158.0.3+g8c29be2+chromium-158.0.8127.18 chromium_major=Some(158) capabilities=4
surface=1
navigate=https://example.com
load_finished surface=1 status=Some(200) url=https://example.com/
surface_destroyed=1
engine_host_exited
probe=ok
exit code = 0
```

This means that **real page loading over the protocol** works: the client requests, the
engine host loads with CEF, sends an event, the client closes. The browser's two halves **talk to each other**.

### Six bugs this slice surfaced (all fixed)

1. **CEF's JSON writer flag is `JSON_WRITER_*`** (not `JSONWRITER_*`), and it's in `cef_types.h`.
2. **Integers must be written as integers.** `SetDouble(1.0)` → `1.0` in the JSON, which Rust's `u64`
   cannot accept. Fix: `SetInt` + `JSON_WRITER_OMIT_DOUBLE_TYPE_PRESERVATION`. This kind of bug
   silently stops the protocol, so now a **test** guards the JSON forms sent by C++.
3. **`GetKeys()` is out-parameterized** in this CEF version.
4. **My own client ran into an infinite loop**: `request()` put the non-matching event back into the
   waiting queue, then took it out again — while the response was waiting in the channel. Fix: it sets the events
   aside and puts them back **once** after the response arrives. (This was the "why doesn't a reply come when the
   log says everything replied" mystery.)
5. **PowerShell's `$host` is a reserved variable** — my own test command passed the wrong argument.
6. **An earlier trial process stayed alive** and locked the exe (`os error 5` at build time) — the
   tests now always stop leftover processes.

Plus: the `ANTIKOLT_ENGINE_DISABLE_GPU=1` environment switch, because in this environment the
**GPU process crashes six times**, and CEF's restarts disturb the measurement. This is not a product setting,
only a tool for deterministic runs (it's listed among the reservations in `18-F0-JELENTES.md`).

### Next slice

1. The **freezing** prototype: the `snapshot` command on the engine (MHTML), then destroying the engine and
   measuring the memory drop — the product's most important mechanism.
2. Investigating the GPU process stopping (a working GPU measurement).
3. Wiring the Qt shell onto this same `EngineLink` (the first real shell ↔ engine call).

---

## 2026-11-16 (third slice) — a real browser window and the first memory measurement

### Interactive mode: closing works too

By the end of the third slice the window **loads a real page and closes cleanly**:

```
$ antikolt-engine-host --url https://example.com
load_end url=https://example.com/ status=200
(the user closes the window → WM_CLOSE)
load=ok
url=https://example.com/
status=200
title=Example Domain
exit code = 0, leftover processes = 0
```

Three more bugs had to be solved for this:

1. **`CefQuitMessageLoop` was ineffective** alongside the blocking `CefRunMessageLoop` (the `WM_CLOSE`
   arrived, yet the loop didn't stop). Solution: **external message pump** — the host pumps the
   Windows messages itself and calls `CefDoMessageLoopWork`. This is also the **form needed for Qt integration**,
   so it's not a workaround but the correct architecture.
2. **CEF creates the browser asynchronously**, so the "while there is a browser" loop condition exited immediately
   (the browser didn't exist yet). Solution: a `browser_created` flag, set by `OnAfterCreated`.
3. **The report lied**: it wrote "load=ok" even when no load happened. Now `load=none`, and
   the measurement mode signals this with exit code 1.

### What was built

| Area | Content | Status |
|---|---|---|
| `apps/engine-host` | `CefInitialize` + a **real native window** + one CEF surface + load/title callbacks + `--selftest` / `--url` / `--size` / `--hidden` | ✅ runs, exit 0 |
| `libcef_dll_wrapper` | the CEF C++ wrapper wired up (`libcef_dll_wrapper` target, `CEF_STANDARD_LIBS`) | ✅ |
| `perf/measure-engine.ps1` | measuring the process tree's private memory, markdown + JSON report | ✅ |
| `docs/HU/18-F0-JELENTES.md` | **the F0 decision-gate material, with real numbers** | ✅ |

### The F0 measurement result (this is the point)

| Page | Engine median | Processes |
|---|---|---|
| `about:blank` | 119.0 MB | 7 |
| Wikipedia article | 177–185 MB | 7 |
| Google Maps | 142.9 MB | 7 |
| YouTube homepage | 386–390 MB | 10 |

A full browser (engine + Qt shell + core) is expected to be **270–510 MB** → the 1 GB target is **easily held**.
The detailed analysis and the reservations (the absence of the GPU process in the measurement, commit vs working set):
[`18-F0-JELENTES.md`](18-F0-JELENTES.md).

### Five bugs the real CEF engine surfaced (all fixed)

1. **Our own argument parser killed the helper processes** (`--type=gpu-process`), so the GPU
   stopped. Fix: the `--type=` prefix is caught before the parser → `CefExecuteProcess`.
2. **`CefRunMessageLoop` + fast load = permanent block**: the `data:` URL loaded *before* the loop started,
   the quit signal was lost. Fix: a deterministic pump loop (`CefDoMessageLoopWork`).
3. **`CefShutdown` called with a live browser = access violation** (this is what the project owner saw in the
   error dialog). Fix: `CloseBrowser(true)` + pumping before shutdown.
4. **`WS_POPUP`, a never-shown window** → GPU compositing error. Fix: a normal window type,
   without `ShowWindow`.
5. **CEF 158: there's no `settings.user_data_path`** → `cache_path` / `root_cache_path`.

Plus: `SetErrorMode` turns off the error dialog so that a failing run signals with an exit code.

### Open point for the next slice

The **GPU process stops** in this environment (`0x80000003`), Chromium falls back to software compositing.
So the measurement reflects a tree without a GPU process — on a working GPU +50–150 MB is expected. We'll
investigate this before considering the measurement "final".

---

## 2026-11-16 (continuation) — toolchain installation, first native build

At the project owner's request, I **installed** the tools under `D:\Tools` (the machine's tool directory).

### Installed

| Tool | Version | Location |
|---|---|---|
| **Qt 6** | 6.8.3 (MSVC 2022 x64; qtbase + qtdeclarative + qtsvg) | `D:\Tools\Qt\6.8.3\msvc2022_64` |
| **CEF** | 158.0.3 (Chromium 158.0.8127.18, beta) | `D:\Tools\cef\cef_binary_158.0.3+…_windows64_beta` |
| MSVC / SDK / CMake / Ninja | 19.44 / 10.0.26100.0 / 3.31.6 / from the BuildTools | — |

Details, pitfalls, and re-running the installation: [`17-ESZKOZLANC.md`](17-ESZKOZLANC.md).

### What was built

| Area | Content | Status |
|---|---|---|
| `CMakeLists.txt` | root build: Qt required, CEF optional (`ANTIKOLT_CEF_ROOT`), configuration summary | ✅ |
| `apps/desktop-shell` | Qt window, control bar, **four themes** (light/grey/dark/AMOLED), `--selftest` | ✅ runs |
| `apps/engine-host` | engine-host skeleton: **links to CEF**, `--selftest` verifies `libcef.dll` loading | ✅ runs |
| `scripts/build.ps1` | automatic tool discovery (vswhere + disk), manual `PATH`/`INCLUDE`/`LIB`, Ninja, self-tests | ✅ |
| `scripts/install-cef.ps1` | CEF download + extraction + verification (numeric version selection) | ✅ |

### Verified (the actual output)

```
=== Self test (shell) ===
antikolt-shell 0.1.0
qt=6.8.3
platform=offscreen
quick=1
theme=light ok / grey ok / dark ok / amoled ok
ok

=== Self test (engine host / CEF) ===
antikolt-engine-host 0.1.0
cef=158.0.3+g8c29be2+chromium-158.0.8127.18
chromium=158.0.8127.18
cef_runtime=ok
ok
```

`cef_runtime=ok` means that the headers compile, the import library links, the `libcef.dll`
loads from the output folder, and a CEF C function (`cef_string_utf16_set`) is callable. This is the
riskiest integration step, and it **works**.

### Four pitfalls I found along the way (all coded into `build.ps1`)

1. The `vcvars64.bat` here is a 39-byte forwarder and doesn't create a compiler environment → the script manually
   sets `PATH`/`INCLUDE`/`LIB`.
2. The "Visual Studio 17 2022" CMake generator doesn't find the Build Tools → Ninja generator.
3. A backslash path given to the CMake cache causes a syntax error (`rc.exe`) → forward-slash paths.
4. CEF's index is not chronological: the first script version picked the 2016 Chromium → numeric
   sorting.

### What is still missing from the engine host

The **browser window** (native child window + one CEF surface + `CefInitialize`/`CefRunMessageLoop`),
wiring up `libcef_dll_wrapper`, enabling the **CEF sandbox**, and the measurement. This is the F0 spike's scope —
the next slice.

---

## 2026-11-16 — First development slice: the Rust core and the repo's foundation

### What I built

| Area | Content | Status |
|---|---|---|
| Cargo workspace | `crates/antikolt-ipc`, `crates/antikolt-core`, shared profiles | ✅ compiles |
| **IPC contract** | 20 messages, 4-byte length-prefixed framing, version check, error codes, capability query | ✅ 10 tests |
| **Identifiers** | ULID-like, time-ordered, Crockford base32, validating (de)serialization | ✅ 8 tests |
| **Clock** | injectable `Clock`, `ManualClock`, saturating time arithmetic | ✅ 4 tests |
| **Resource profiles** | 5 profiles (Fast/Balanced/Economical/Ultra/Manual), memory-pressure bands, automatic selection | ✅ 9 tests |
| **Lifecycle machine** | `Saved→Warming→Live→Freezing→Saved`, purge/archive branch, exceptions, freezing deadline, the I1 invariant | ✅ 15 tests |
| **Session policy** | `keep`/`sweep`/`wipe`, TTL, delete-on-lock, ledger row, "what we lose" estimate | ✅ 13 tests |
| **Snapshot manifest** | digest type, fields, validation (URL, control characters, luminance, version) | ✅ 14 tests |
| **Migrations** | `0001_init.sql` (14 tables/indexes/FTS), embedding, validation | ✅ 8 tests |
| **Home page** | `<antikolt-card>`, virtualized grid, 4 themes, search, keyboard, mock bridge | ✅ syntax-checked |
| **Repo** | LICENSE (MPL-2.0), NOTICE, AUTHORS, SECURITY, CONTRIBUTING, CODEOWNERS, PR template, CI, .gitignore | ✅ |

### How I verified (not a claim, but a command)

```
cargo test --offline          ->  67 passed (core) + 10 passed (ipc)  = 77
cargo clippy --all-targets --offline -- -D warnings   ->  clean
cargo fmt --all -- --check    ->  clean
node --check home/app.js      ->  OK
SPDX header check             ->  16/16 source files in order
markdown link check           ->  0 broken links
git commit (root)             ->  57 files, 9498 insertions
```

**My own CI rule caught my own bug:** after the first commit, the local check found that
`antikolt-ipc`'s four source files were missing the SPDX header (the `core` files had it, these didn't).
This is exactly what the CI `hygiene` job is for — so I added the header, and normalized the line endings
with `.gitattributes` (Windows checkout / Linux CI should see the same bytes). The lesson for the
process: **the local check must run the same thing as the CI**, otherwise the bug only shows up
after the push.

The 77 tests record the *behaviour*, not the code's form: the 10-minute hibernation at the 9:59 and 10:00
boundary, the exceptions (pinning/audio/video/download/popup), the degraded
save after the freezing timeout, `sweep` doesn't mean logout, `wipe` does, the manifest rejects the `javascript:` URL
and control characters, the id generation is unique over 10,000 samples.

### What blocks me — and why I didn't write it blind

| Obstacle | Measurement | Impact | Solution |
|---|---|---|---|
| **No Qt 6** | `C:\Qt`, `D:\Qt`, `C:\Program Files\Qt` — none exists | the Qt shell can't be compiled | Qt Online Installer → Qt 6 MSVC 2022 64-bit (only Core/Gui/Quick, ~2 GB) |
| **No CEF** | `C:\cef`, `D:\cef`, `C:\vcpkg` doesn't exist | no engine, no real memory measurement | CEF standard build (Windows x64, ~1–2 GB) under `third_party/cef/` |
| **There is a C++ toolchain, though** | `cl.exe`: `…\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe`; `cmake.exe`: `…\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` | they're just off the PATH | the build starts with `vcvars64.bat`, as do the Rust tests |
| **No crates.io access** | `SSL connect error ... SEC_E_NO_CREDENTIALS` | new crates can't be installed | `--offline` from the local cache (263 crates); **rusqlite / libsqlite3-sys / sha2 unavailable** |
| **No SQLite binding** | `rusqlite` is missing from the cache | the DB layer can't be written | the schema and migration validation are done; the `antikolt-db` crate is the next slice |
| **No SHA-256 provider** | the `sha2` 0.10 chain (`generic-array`) is missing | the `Digest` type is done, but there's no filler | next slice (sha2 or a self implementation tested with FIPS vectors) |
| **WMI is forbidden by the sandbox** | `Get-CimInstance` → access denied | the 32 GB RAM can't be read, it's user data | `09-CELHARDVER.md` marks it as user data |

### What must be installed for the shell (two things, ~1–2 hours)

The C++ toolchain **is present**, so the missing part is two things:

1. **Qt 6** — Qt Online Installer, "Qt 6.x → MSVC 2022 64-bit", of which *Qt Core / Gui / Quick* is enough
   (Qt WebEngine is **not** needed: the engine is provided by CEF). After installation: `C:\Qt\6.x\msvc2022_64`.
2. **CEF** — the standard Windows 64-bit build unpacked into the repo's `third_party/cef/` folder
   (the binary is not put under version control, the `.gitignore` excludes it).

As soon as these two are present, the next slice: `CMakeLists.txt` + the minimal `main.cpp` (an empty Qt window),
then the first F0 spike: embedding a CEF view and **measuring the memory** on the three reference pages.

This list is not an excuse, it's the work log: every row has a concrete next step.

### Decisions made while writing the code

1. **Deserializing the identifier validates.** `Id` is not `#[derive(Deserialize)]`, because that would let a raw
   string in; the self implementation calls `Id::parse`, so an identifier coming from disk or the network
   is always canonical.
2. **The `pending_maintenance` table.** The 24-hour deletion needs an engine; if there isn't one right now, the task is
   queued (`docs/HU/02` §4.3). The plan didn't name this table, the implementation demanded it.
3. **The freezing deadline is a separate event.** `Tick` switches to `FreezeTimedOut` after 2.5 s, which
   means a degraded save and engine destruction — so a slow page doesn't hang the browser.
4. **Leaving during `Warming` aborts the load** (nothing to save), it doesn't freeze.
5. **The home page virtualizes uniform M-sized cards**, the pinned cards appear in L size, in a non-virtualized
   row (max. 8). Free layout is left to the user's own home page
   — this is a deliberate compromise in favour of the memory promise.

### Known shortcomings in the code (not hidden TODOs)

- `snapshot::Digest`: the type and the validation are done, the SHA-256 filler is **not**.
- `migrations`: the lists and the validation are done, the **applier** (`apply`, transaction, `user_version`)
  comes in the DB crate.
- The `session` ledger is only the *logic*; there's no SQLite query and no UI panel behind it.
- The home page runs with a **mock bridge**; injecting the real `window.antikolt` is the shell's job.
- The written spec of `proto/v1` exists (`proto/README.md`), but the C++-side implementation doesn't.

### Next slice (the first half of F1)

1. `antikolt-db` crate: rusqlite (once available), WAL pragmas, migration runner, transactional tests.
2. SHA-256 provider + blob store (content-addressed, without zstd in the first round).
3. The `perf/` measurement bench skeleton (measuring the process tree's private working set), so the budget is measurable.
4. Then the shell (CMake + Qt 6 + CEF) — this is the first step that needs procurement on VECTOR.
