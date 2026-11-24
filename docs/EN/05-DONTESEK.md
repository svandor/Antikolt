# 05 — Decision log (ADR)

Every irreversible or expensive decision is recorded here: **context → decision → consequence**.
Changing a decision requires a new ADR (the old one gets the "Deprecated" status).

## ADR-097 — De-risking the views switch: views loads, but rejects the partitions' cache path (2026-11-24)

**Context:** according to ADR-096 the PiP window crashes in windowed mode, so the maintainer chose the views switch
(`CefBrowserView`/`CefWindow`). The first brick: measure whether the views framework loads
a page in the engine at all.

**Decision / result (measured, `--viewstest`):**

1. **Views loads:** `CefBrowserView::CreateBrowserView` + `CefWindow::CreateTopLevelWindow` (CHROME
   style, the delegate `GetBrowserRuntimeStyle`/`GetWindowRuntimeStyle` → CHROME) with the **default profile**
   (`nullptr` request context) → `views_load=ok` (the data: page loads, exit 0). The views framework
   **works** in CEF 158.
2. **But CHROME-style views rejects the custom cache path:** `CreateBrowserView` returns **null**
   if the request context has a **custom `cache_path`** (`views_partition_context=rejected`).
   The rejector is isolated **to the cache path itself** (not the handler, not `persist_session_cookies`):
   a context with empty settings (no cache path) is **accepted**, a cache-path-only context is **rejected**.
3. **Why:** according to the CEF source, `CefBrowserViewImpl::Create` calls `request_context_impl->
   VerifyBrowserContext()`, which returns false for the secondary-profile (custom cache path) context.
   CHROME-style views is bound to the **default profile**.

**Consequence:** (+) the views switch is **viable** at the "loads a page" level (the basis for hosting the PiP window
is ready); (−) CHROME-style views **conflicts with partition isolation** (ADR-009/071: the
per-container storage isolation stands precisely on the custom cache-path request contexts); (−) the views switch
is therefore **not drop-in**: either the partitions must be rethought as Chrome profiles, or the PiP window
hosting stays on the windowed path (with the crash). **Decision (2026-11-24): the maintainer chose option 2
— PiP deferred** on the windowed path with the documented crash; the views switch (Chrome profiles) is not
continued now, we focus on the remaining M7 items.

## ADR-096 — The PiP window spike: `requestWindow()` opens the window, but windowed shutdown dies (2026-11-24)

**Context:** according to ADR-095, Document-PiP is **present** in the secure context (no views switch is
needed for the exposure). The question: does `documentPictureInPicture.requestWindow()` actually open a
PiP window in CEF 158's **windowed** (not views) mode.

**Decision / result (measured):**

1. **`--pipwindowtest`** in the engine host: `requestWindow({width:320,height:180})` with CDP `userGesture`
   (synthetic gesture) + `awaitPromise` + `returnByValue`.
2. **`pip_window=ok`** — the Promise resolves, the window has a document (`ok:doc`): Document-PiP
   **opens** on the current windowed engine, without a views switch.
3. **Shutdown dies** (`0xC0000005` access violation), because Document-PiP, via `OnBeforePopup`
   (`url=about:blank`), creates a second browser, **plus** further internal windows, which
   close via `OnBeforeClose`, but never appear in `OnAfterCreated` (measured with pointer identifiers:
   besides the two opening ones, three different closing browsers). The host's one-popup model this cascade
   cannot track, so `CefShutdown` faces a live internal window.
4. **The obvious fix attempt did not solve it either:** closing the second browser before `CefShutdown` and the
   precise assignment of the closing browser, shutdown still dies — the internal windows close in `OnBeforeClose`
   but never appear in `OnAfterCreated`, so the host cannot track them (the
   attempt was reverted).

**Consequence:** (+) the PiP window **opens** (measured), so the feature is not a principled obstacle; (−) in windowed
(not views) mode the Document-PiP window lifecycle is **broken** in CEF 158 (internal window cascade +
`0xC0000005` in shutdown); (−) hosting the actual PiP window is possible with the views framework
(`CefBrowserView`/`CefWindow`) or with deeper CEF work — a **Pro model is needed** for the remaining
crash; `--pipwindowtest` is diagnostic, **not** a gate step (because of the crash).

## ADR-095 — PiP de-risking fixed: Document-PiP is present in the secure context, no views switch needed (2026-11-24)

**Context:** according to ADR-094, Document-PiP requires a views architecture switch, because `--pipchecktest`
measured `pip_api=absent` on the `data:` URL. The `data:` URL, however, is an opaque (non-secure) origin, and
`documentPictureInPicture` is `[SecureContext]`-gated in the Chromium IDL — so ADR-094's measurement could not separate the "absent
in the runtime" case from the "secure context required" case. Besides, in CEF 158 there is already
**no** `chrome_runtime` setting: the Alloy runtime was discontinued in **CEF 128** (last supporting: CEF 127; the
`chrome_runtime` field was removed from the headers in CEF 128, because there is nothing to switch) — the engine **already runs** on the
Chrome runtime.

**Decision / result (measured):**

1. `--pipchecktest` rewritten: the probe runs on the **secure** `antikolt://` scheme (SECURE, ADR-093), on the **loaded**
   page (`OnLoadEnd`, not `OnAfterCreated`), and measures both `window.isSecureContext` and
   `'documentPictureInPicture' in window`.
2. **`--pipchecktest` (without the flag): `pip_secure=1 pip_api=present`, exit 0.**
3. **`--pipchecktest` (with the `DocumentPictureInPictureAPI` flag): `pip_secure=1 pip_api=present`, exit 0** — the flag
   is a no-op, because the feature is enabled by default since M116.
4. The gate is green: `pipchecktest ok` in `scripts/verify.ps1` (the gate grew from 80 to 81 checks).

**Consequence:** (+) the **views architecture switch is not necessary** for Document-PiP — the API is present in the secure
context on the current (Chrome runtime) engine; (+) ADR-094 is **Deprecated** (the `data:` URL measurement
was confounded); (+) `--pipchecktest` is part of the gate, so the "Document-PiP is present" invariant is guarded; (−) the
actual PiP is still a separate slice: wiring up `documentPictureInPicture.requestWindow()` and measuring the pop-up PiP window's
behavior (CEF's Chrome runtime displays the PiP window as a fully styled popup — CEF #3448).

## ADR-094 — [Deprecated, see ADR-095] De-risking PiP: Document-PiP is missing in the Alloy runtime, the flag is not enough either (2026-11-23)

**Context:** for PiP (F7) the maintainer chose the views architecture switch. The question: is
Document-PiP (`documentPictureInPicture`) available in the current Alloy/windowed runtime with the
`--enable-features=DocumentPictureInPictureAPI` flag, or is the Chrome/views runtime really needed.

**Decision / result (measured):**

1. **`--pipchecktest`** in the engine host: it measures the `'documentPictureInPicture' in window` expression
   on a data: URL page (Runtime.evaluate, returnByValue).
2. **Baseline (without the flag): `pip_api=absent`.**
3. **With the `--enable-features=DocumentPictureInPictureAPI` flag: `pip_api=absent`** — the flag alone
   does NOT make the API available in the Alloy runtime.

**Consequence:** (+) the necessity of the views switch is **empirically confirmed** (not an assumption);
(+) `--pipchecktest` is the tool for re-measuring after the switch (in the Chrome/views runtime it must report `present`);
(−) the views architecture switch (CefBrowserView/CefWindow + `chrome_runtime`) remains a separate, large
slice.

## ADR-093 — The passkey flow: the virtual authenticator command, the full `create` requires a focus gesture (2026-11-23)

**Context:** the passkey F0 spike (`--webauthntest`) proved that the CDP virtual authenticator
(`WebAuthn.addVirtualAuthenticator`) works in CEF 158. The question: does the full
`navigator.credentials.create` work too.

**Decision / result:**

1. **`webauthn_setup` command** on the engine host's stdio protocol: `Emulation.setFocusEmulationEnabled`
   → `Page.bringToFront` → `WebAuthn.enable` → `addVirtualAuthenticator`, `{authenticator_id}` response.
   Measured: `passkey_setup=ok`.
2. **WebAuthn's origin check is stricter than the secure context:** the custom `antikolt://` scheme (although
   `CEF_SCHEME_OPTION_SECURE`) gives `SecurityError: …invalid domain` on `create` — WebAuthn only accepts
   HTTPS, `localhost` (127.0.0.1), or chrome-extension://.
3. **The full `create` requires a focus gesture:** the `NotAllowedError: …does not have focus` looks at the
   real focus of `RenderWidgetHostView`, which the hidden/offscreen surface cannot provide (the
   `SetFocus`, `setFocusEmulationEnabled`, and `bringToFront` are not enough either).

**Consequence:** (+) the passkey mechanism is de-risked: the authenticator can be set up via command, the origin
and focus requirements are measured; (+) `--passkeyflowtest` in the gate (`passkey_flow=de-risked`); (−) the
full `create`/`get` works on the real (focused) surface, in the offscreen harness it is
`not-measurable` (focus gesture); key export/import into the vault is a separate slice.

## ADR-092 — The vault panel: metadata-only list, the shell asks the core (2026-11-23)

**Context:** the vault (ADR-083) lives in the core, the secrets (username, password) only come from an open vault
(ADR-010). The shell panel must display the credentials without copying a secret.

**Decision:**

1. **The panel works with `vault_list`, not `vault_get`:** the list is `CredentialMeta` (site, type,
   label) — the username and password are not in it, so the panel never sees a secret.
2. **Unlocking goes through the panel's own field:** the `VaultPanel` calls `CoreClient::vaultUnlock` with the
   typed master password, then `refresh` — the vault-lock state machine stays in the core.
3. **`--vaultpaneltest`** in the gate: the closed panel is empty, after unlocking 1 row, the site is visible, the secret is not.

**Consequence:** (+) the vault panel is measured (`vault_panel=ok`, exit 0); (+) secret-leak-freeness is
measured at the panel level too; (−) adding/editing/TOTP display in the panel, the full
`navigator.credentials` flow, and PiP are still separate slices.

## ADR-091 — Filling as a command: `fill_credential` in the engine host (2026-11-23)

**Context:** the ADR-089/090 spikes proved that filling works in the isolated world, and
the origin-based lookup too. The missing piece: the shell can actually command the engine to fill an
arbitrary credential.

**Decision:**

1. **`fill_credential {username, password}` command** on the engine host's stdio protocol: the engine writes
   the credential into the login form in the isolated world (getFrameTree → createIsolatedWorld →
   filling with a native value setter → verification from the main world), and responds with `{filled: true}`.
2. **The credential is embedded in the script with JSON quoting** (`CefWriteJSON`), so the `"` and `\` characters cannot
   break the JavaScript.
3. **`--fillcredentialtest`** in the shell: data: URL form → `fill_credential` → the main world reads the
   filled value, and measures that the secret did not leak.

**Consequence:** (+) autofill is commandable end-to-end from the shell (`fill_credential=ok`, exit 0);
(+) the command, combined with `CoreClient::vault_get_for_origin`, gives the full "open the page, fill
it" flow; (−) the graphical vault panel, the full `navigator.credentials` flow, and PiP are still
separate slices.

## ADR-090 — The autofill lookup is origin-based, with exact- then subdomain matching (2026-11-23)

**Context:** autofill (ADR-089) must find the credential in the vault based on the page's origin.
The question: what counts as a match, if the credential was saved e.g. for `https://pelda.hu`, and the page is
on `https://login.pelda.hu`.

**Decision:**

1. **`vault_get_for_origin`:** the core returns the credential matching the page's origin — **first exact**
   origin match, **then subdomain** (the stored host is a suffix of the page's host, at host boundaries).
2. **The suffix match is safe:** `pelda.hu` matches `login.pelda.hu`, but NOT
   `pelda.hu.evil.example` (the suffix must start with a `.`).
3. **No match → `credential: null`, not an error** (so the autofill UI knows there is "no saved
   credential").

**Consequence:** (+) the autofill lookup is measured (exact + subdomain + foreign host + suffix attack);
(+) the lookup stays in the core, the secret, like `vault_get`, only comes from an open vault; (−) the
`fill_credential` command in the engine host and the shell orchestration are still separate slices.

## ADR-089 — Filling works in the isolated world: the form fills up, the secret does not leak (2026-11-23)

**Context:** the ADR-088 spike proved that a CDP isolated world is created in CEF 158. The
next question: does the actual password-filling mechanism work too — the isolated
world writes the credential into the shared DOM, the page's main world sees the filled value, but never the secret.

**Decision (the spike's result):**

1. **The filler script runs in the isolated world:** `--autofilltest` (getFrameTree →
   createIsolatedWorld → Runtime.evaluate in the world with the filler script → Runtime.evaluate in the main
   world with the verification script) measures that the password/password field fills up with the native value setter,
   and from the main world the probe variable is `undefined`.
2. **The secret does not leak:** `autofill=ok verdict=ok` (exit 0) — the fields fill up, `typeof
   window.__antikolt_fill_probe` is `undefined` in the main world.

**Consequence:** (+) the secure autofill mechanism is de-risked: in the isolated world the
credential can be written, the page does not see it; (+) `--autofilltest` measures it in the gate; (−) the shell↔core
orchestration (fetching the credential by origin + the `fill_credential` command) and the graphical panel
are still separate slices.

## ADR-088 — Filling in the isolated world: the CDP isolated world works in CEF 158 (2026-11-23)

**Context:** the secure form of password filling (autofill) is the isolated world: the secret typed by the shell
is invisible to the page's main world (according to ADR-053 there is no dialog on the page, and the secret never
gets into the main world). The question was whether CEF 158 can create such a world via CDP.

**Decision (the spike's result):**

1. **`Page.createIsolatedWorld` works:** `--isolatedworldtest` (getFrameTree →
   createIsolatedWorld → Runtime.evaluate in the world → Runtime.evaluate in the main world) measures
   that the probe set in the isolated world is `undefined` from the main world.
2. **The isolation actually holds:** `isolated_world=ok main_sees=undefined` (exit 0) — so
   password filling in the isolated world is viable without a CEF patch.

**Consequence:** (+) the basis of secure autofill is de-risked; (+) `--isolatedworldtest` measures it in the
gate; (−) the actual filling (fetching the credential from the core + the form-filler script)
is still a separate slice.

## ADR-087 — The vault is reachable from the shell too: CoreClient vault methods + `--vaulttest` (2026-11-23)

**Context:** the vault lives in the core (ADR-083/084/085), but the shell could not control it until now — only the
core's Rust tests and the service integration tests could measure it. The shell surface (vault panel, filling)
requires the shell to talk to the core.

**Decision:**

1. **`CoreClient` carries the `vault_*` operations** (`vault_status`/`unlock`/`lock`/`list`/`create`/
   `get`/`delete`), with the same length-prefix JSON transport as the cards/sessions.
2. **The `--vaulttest` self-test in the gate:** the shell starts the core, unlocks the vault (master password),
   creates a credential, lists it (without the secret), retrieves it, locks it, and measures that the locked vault
   rejects the get — down to the `vault_locked` error.
3. **Hermetically:** the test deletes the `vault-selftest` directory at startup, so an earlier (crashed)
   run does not leave behind a counted credential.

**Consequence:** (+) the vault is measured end-to-end from the shell (`vault_check=ok`, exit 0); (+) the `CoreClient`
vault methods are the basis of the future vault panel and filling; (−) the graphical vault panel and the isolated-
world autofill are still separate slices.

## ADR-086 — The passkey F0 spike: the CDP virtual authenticator works in CEF 158 (2026-11-23)

**Context:** according to 12-PASSKEY.md the "B" path of the software passkey is built on the CDP
`WebAuthn.addVirtualAuthenticator` virtual authenticator, but the F0 spike had to decide whether
this works in CEF 158 — because the CDP WebAuthn domain is tied to the embedder's WebAuthn implementation,
which CEF in principle does not provide.

**Decision (the spike's result):**

1. **The WebAuthn domain is reachable, but `WebAuthn.enable` must come first:** the first attempt's
   direct `addVirtualAuthenticator` call gave the `-32000 „The Virtual Authenticator Environment has not been
   enabled"` error; the `WebAuthn.enable` → `addVirtualAuthenticator` order, however, **works**.
2. **The virtual authenticator is created:** `--webauthntest` ends with the `webauthn_authenticator=ok id=<authenticatorId>`
   output (exit 0) — so the B path is viable in CEF 158, without a CEF patch.

**Consequence:** (+) the passkey B path is de-risked: the CDP virtual authenticator works, no CEF patch or helper flow
is needed; (+) `--webauthntest` measures it in the gate; (−) the full
`navigator.credentials.create/get` flow (secure-context test page, key export/import into the vault) is still
separate slices.

## ADR-085 — The password CSV import with name-based column mapping, in the core (2026-11-23)

**Context:** Chrome and Firefox export passwords to CSV, but with different column sets:
Chrome `name,url,username,password`, Firefox `url,username,password,…`. The import must write into the vault
(the secret is in the core, ADR-083), and it comes via the `vault_import_csv` IPC.

**Decision:**

1. **Column mapping by name, not by position:** we look for the `url`/`username`/`password`/`name`
   headers (also handling the UTF-8 BOM), so both exporters and any order
   work.
2. **The import is in the core (`import.rs`) + `vault_import_csv`:** the parse is a pure function, the rows
   become `kind = password` credentials; the vault must be open for the call (like the other secret operations
   ).
3. **We skip the incomplete row, we do not import it:** without a URL or password the row cannot be a credential,
   because it would mislead the later autofill.

**Consequence:** (+) the Chrome/Firefox export is importable and measured (`cargo test` **187** green);
(−) Firefox's "URL only, no password" rows are left out (that is reality: such a row is not a password);
(−) the import is for now only reachable via the `vault_import_csv` IPC, the shell menu comes with the
shell surface slice.

## ADR-084 — TOTP is in the core, RFC 6238, and the code is computed by the service (2026-11-23)

**Context:** the vault store (ADR-083) already holds the TOTP secret (`totp_secret_ct`), but the code
generation was missing. TOTP is the middle leg of the "passwords + TOTP + passkey" decision, and the code must be born on the core
side: the secret must never leave the core.

**Decision:**

1. **RFC 6238 in the core** (`totp.rs`): `totp_code(secret, digits, period, algorithm, now_ms)` — the
   HMAC-SHA-1/256/512 + dynamic truncation (RFC 4226) + base32 secret; `now_ms` is a parameter, so the
   RFC vectors are testable without a clock.
2. **The code is computed by the service** (`vault_totp` IPC): the shell passes the `id_of_credential`, the
   core opens the secret and returns `{code, seconds_remaining}` — the secret stays in the core.
3. **The parameters are in `totp_params_json`** (digits 6|8, period, algorithm), with the RFC defaults
   (6 digits, 30 s, SHA-1).

**Consequence:** (+) the TOTP code is measured with the RFC 6238 vectors (the "classic trap" avoided: the
vectors use a different secret per algorithm, 20/32/64 bytes); (+) the secret stays in the core;
(−) passkey (WebAuthn) and CSV import are still separate slices. `cargo test` is now **180** green.

## ADR-083 — The vault store is SQLite-only: the secrets have no JSON export (2026-11-23)

**Context:** ADR-082 wired up the encryption (AES-256-GCM + Argon2id + DPAPI), but the vault did not store
anything yet. According to the ADR-069/070 rule, every store works with one writer (SQLite), the `*.json`
documents are derived exports — a secret (password, TOTP key, passkey), however, must never
get into an export, because that is portable, copyable, and not encrypted.

**Decision:**

1. **The vault is SQLite-only:** the `credential` table (schema 0011) holds the secret fields in `*_ct` columns
   (AES-256-GCM, AAD = origin + id), and there is **no** `vault.json` export — the `db_status` sync does not write out
   a secret into any derived document either.
2. **The key is DEK/KEK** (ADR-010): the `vault_meta` single-row table holds the mode (`master` | `dpapi`), the salt, and the
   wrapped data key; the first unlock fixes the mode, subsequent ones only read it.
3. **The list is meta, the get is secret:** `vault_list` only gives metadata (id, origin, kind, label, last_used_at),
   even in the locked state; `vault_get` returns the plaintext only with an open vault; the wrong password,
   the tampered record, and the blob replayed to another origin all give `vault_locked`.
4. **The 5-minute idle auto-lock is in the service loop:** every request `tick`s the vault, the secret
   operations (`get`/`create`/`delete`) `touch` it — so an unused vault closes on its own
   (ADR-010), not only on a manual `vault_lock`.

**Consequence:** (+) passwords, TOTP keys, and passkeys can now be stored and read back,
measured (`cargo test` **174** green: the 7 credentials unit tests + the end-to-end
`the_vault_seals_serves_and_persists_credentials`); (+) the secrets never reach a JSON export; (−) the
TOTP code generation, passkey (WebAuthn), and Chrome/Firefox CSV import are still separate slices; (−) the
profile bundle (`profile_export`) cannot carry the vault either, until the encryption for it is finalized.

## ADR-082 — The vault's encryption: AES-256-GCM + Argon2id + Windows DPAPI (2026-11-23)

**Context:** ADR-031 prepared the vault's shape, but hid the encryption behind `UnavailableSecrets`,
because the build environment had no crates.io access, and the offline registry contained not a single
cryptographic crate. The password import (M7), however, needs a target vault, which
cannot be built without encryption. The maintainer's decision: **passwords + TOTP + passkey**, encryption
**Windows DPAPI + optional master password** (the ADR-010 DEK/KEK plan), and **Chrome/Firefox CSV import**.

**Decision:**

1. **The crates are wired in** (`aes-gcm` 0.10, `argon2` 0.5, `zeroize` 1): the build environment now reaches
   crates.io, so the earlier blocker is gone.
2. **Record encryption is AES-256-GCM** (`AesGcmSecrets`, `secrets.rs`): a fresh 96-bit
   nonce per record (written before the ciphertext), the record's AAD is **origin + record id** (ADR-010) — the GCM
   tag checks the key, the body, and the AAD together, so the wrong key, the tampered record, and
   the blob replayed to another origin are a single error (`Corrupt`), never a plaintext match.
3. **The master password path is Argon2id** (`from_master_password` + `generate_salt`): the 256-bit key comes from the
   password and the stored salt; the same password+salt gives the same key, the wrong password does not open.
4. **The DPAPI path on Windows** (`secrets::dpapi`): `CryptProtectData`/`CryptUnprotectData` from
   `crypt32`, with `CRYPTPROTECT_UI_FORBIDDEN`; the module is the crate's only place using `unsafe`
   (in a `#[allow(unsafe_code)]` module, with SAFETY comments), because the crate has `#![deny(unsafe_code)]`.
5. **The key material lives in `zeroize::Zeroizing`**, the `Debug` impl does not print a secret.

**Consequence:** (+) the vault can now seal and open — the `can_store: false` is gone for the
master password and DPAPI paths; (+) the 10 new tests measure (round-trip, AAD binding, tamper rejection,
wrong password, salt uniqueness, DPAPI round-trip on Windows); (−) the DEK/KEK storage, the `credential` table's
writing/reading, TOTP, passkey, and CSV import are still separate slices; (−) the DPAPI blob is bound to machine+user,
the profile's portability is only given with a master password (ADR-010). `cargo test` is now **166** green.

## ADR-081 — The bookmark import consumes the Netscape-HTML format (M7) (2026-11-23)

**Context:** the M7 "password and bookmark import" was open. The password import has no target vault in the
product (a separate, larger slice), the bookmark import, however, fits the card model naturally.

**Decision:**

1. **The input is Netscape-HTML** (`<!DOCTYPE NETSCAPE-Bookmark-file-1>`): this is the only format that
   Chrome, Firefox, and Edge all export, so one parser covers all three.
2. **Tolerant regex parser** (`bookmarks.h` `parseBookmarksHtml`): the format is not valid XML (bare `<p>`,
   `<H3>` folders), so the `<A HREF>` anchors are extracted with a regex, not a stream parser; the `javascript:`
   links and the folders are skipped, the order is preserved.
3. **Every bookmark is a card:** title + URL, onto the default workspace, skipping already-existing URLs
   (re-import does not duplicate). The identifier is the C++ counterpart of the grid's own `newCardId`
   (`bookmarks.h` `newCardId`, 26 characters, Crockford base32).

**Consequence:** (+) the bookmark import is done, `--bookmarkimporttest` measures it (the parser's 3 cases, the id shape,
and the card upsert); (−) the password import is not there (no target vault — a separate slice); (−) the file picker dialog
cannot be clicked by the harness — the menu item is the product's entry point, `--bookmarkimporttest` measures the path underneath it.

## ADR-080 — The "Automatic" profile resolves in the core (power + pressure → built-in, M6) (2026-11-23)

**Context:** the open half of M6 "Five resource profiles, with automatic switching" was the automatic switching. The
ADR-020 already defined and tested the `ProfileKind::auto(power, pressure)` mapping, but it was not wired up.

**Decision:**

1. **`Auto` is the sixth value of `ProfileKind`** (serde `auto`), its `params()` is `None` — it has no fixed
   parameter, like `Manual` has. The core stores and returns `resource_profile="auto"`.
2. **The resolution happens in the core** (`resolve_profile` IPC: `{on_ac_power, pressure}` → `{kind, params}`), based on the
   `ProfileKind::auto` + `params()`, so that ADR-020's table stays in one place (in the Rust core).
3. **The shell measures the conditions and re-asks:** it reads the power source from the system (`GetSystemPowerStatus`,
   with the `ANTIKOLT_AC_POWER` override for the self-test), the pressure comes from the existing memory sampler; after every
   sample it calls `resolve_profile` again, if the profile is `auto`. The first resolution starts with `normal` pressure
   (the power source is the Balanced/Saver selector), `checkMemoryPressure` refines it afterwards.

**Consequence:** (+) the profile follows the machine — hard pressure falls to `Ultra` even on mains power, the
return steps up to `Balanced`; `--autoprofiltest` measures the resolution (6 cases) and the power source override;
(−) the bands take effect in the next sample cycle (the already-made classification still happened with the old bands);
(−) `Fast` never comes from `auto` — that is always a manual choice.

## ADR-079 — The grid's language also follows the shell (HU/EN), via the `data-language` attribute (2026-11-23)

**Context:** ADR-078 made the shell's **chrome** switchable; the grid (`home/app.js`) was still Hungarian. The
grid runs in a separate process (in the engine), so the language must be passed to it — the same way the theme (ADR-037)
already crosses over.

**Decision:**

1. **The language lives in the `data-language` attribute** on the grid's `<html>`, following the theme's `data-theme`.
   The shell sends it with `pushLanguageToGrid` (on load and on `setLanguage`), and via the `window.antikoltSetLanguage`
   hook requests the in-place re-render.
2. **The grid translates its own strings with a `t()`** (`home/app.js`, HU→EN `EN` map), based on the
   `data-language`; unknown key/non-`en` returns the Hungarian source. The `system.language` bridge
   is the fallback (if the push has not arrived yet).
3. **The language switch is reload-free on the grid too:** `setLanguage` rewrites the static labels (search,
   new card button) and re-renders the page (counter, empty state, container legend, cards).

**Consequence:** (+) the grid switches HU/EN, `--gridlangtest` measures it (the search placeholder "Keresés" → "Search",
the `window.antikoltLanguage` witness); (−) the card's **inline** editor labels ("Mentés", "＋címke" in
`antikolt-card.js`) and the grid's static aria labels are still Hungarian for now (next slice).

## ADR-078 — The shell's language is switchable at runtime (HU/EN), first the chrome (2026-11-23)

**Context:** the interface is entirely Hungarian (Hungarian is the authoritative language), the English translation was the open HU/EN
item (M7). The full i18n (shell + grid) is large; this slice lays the **foundation**: the language switch
setting, the translation mechanism, and the shell's **chrome** (toolbar, tray, settings dock, search bar).

**Decision:**

1. **The language lives in QSettings** (`ui/language`: `hu`/`en`, default `hu`), like the theme and the dark pages — it is
   a UI setting, not portable profile data.
2. **The translation happens at runtime, from a table** (`i18n.h` `uiText(hu, lang)`): the source is Hungarian, the
   `en` gives the English from a `QHash`, unknown key/non-`en` language returns the source (the missing
   translation is never an empty label). There is no `.ts`/`.qm` build step, because the number of chrome strings is small, and
   the test thus runs without the engine.
3. **The switch is restart-free:** `MainWindow::setLanguage` sets it, saves it, and relabels the chrome in place;
   the settings dock's `Nyelv:` dropdown calls it.

**Consequence:** (+) the chrome is switchable HU/EN, `--languagetest` measures it (the pure `uiText` table, the
QSettings persistence, and the in-place relabeling); (−) the **status messages and the grid** are still Hungarian
for now (next slices); (−) the `tr()`-based internal error messages (`core_client`/`engine_client`) this slice does not
touch.

## ADR-077 — The preview name is the content, not the card id (2026-11-23)

**Context:** the preview file name was the card id until now (`<id>.png`), and the `sha256`-based
content-addressed blob store stood as an open item, citing the lack of the Rust `sha2` crate ("no network for
`cargo vendor`"). In reality the name is produced in the **shell** (the capture happens there, the bytes are
there), so the core's Rust side does not even need `sha2`: Qt's `QCryptographicHash::Sha256` gives the same,
without a network.

**Decision:**

1. **The preview name is the content's hash** (`<sha256-hex>.<ext>`, `previewContentName` in `previews.h`,
   `QCryptographicHash::Sha256`). The card record's `preview_ref` thus carries the identity of the **image**,
   not the card's.
2. **Dedup follows from the naming:** two cards whose pages are photographed identically byte-for-byte share a single
   file — the second capture no longer copies a new file, only the record learns the same name.
   A content change gives a new hash, the old file becomes a victim of the orphan cleanup (`removeOrphanPreviews`).
3. **The freshness check runs with the existing name:** `previewNeedsRefresh` looks at the stored `preview_ref`
   (not the card id), so the 7-day rule stays valid after content addressing.

**Consequence:** (+) the content-addressed blob store is in place, the `sha2` crate dependency is gone (the hash is in the
shell, with Qt); (+) dedup collapses identical images into one file; (+) `--previewhashtest` (fired against public
test vectors), `--previewgentest`, and `--previewworkertest` (`preview_content_addressed=ok` /
`worker_content_addressed=ok`) measure it; (−) the card-id-based names are gone — the `<id>.png` files created in the old profile
disappear in the orphan cleanup.

## ADR-076 — `video_max_height` is a display cap, not a decoding cap (2026-11-23)

**Context:** ADR-020's resource profiles carry a `video_max_height` field (Fast 2160, Balanced 1080,
Saver 720, Ultra 480), and the profile text promised that "the governor reduces the resolution to save memory".
The shell only **reported** the field so far (`--memoryreport`), it did not apply it — this was F6's last
open control action. The research established: CEF/Chromium **does not provide** a per-page video decoding
resolution limit (there is no such CDP op), and the CSS injection only reaches the top-level `video` elements (the
cross-origin iframe is its own document — a security boundary).

**Decision:**

1. **The cap is a display cap:** the engine, with a named style element (`antikolt-video-cap`), injects the
   `video{max-height:Npx}` rule (`set_video_cap` op), driven by the profile's `video_max_height`. The
   rule is replaced on profile switch, the `manual` profile (no parameter) removes it — the same trick as the
   dark pages toggle (ADR-044/045).
2. **The decoding resolution is `not-measurable`:** the display cap limits the *rendered box*, not the
   *decoded stream* — CEF does not provide a per-page video decoding limit, so the memory saving is indirect
   (a smaller composited box), not a reduction of the decoding buffer. The test **states this**
   (`video_cap_resolution=not-measurable`), it does not hide it.
3. **The scope is top-level `video`:** the cross-origin iframes fall outside (their own document); the cap does not
   work around this.

**Consequence:** (+) the profile's `video_max_height` is now **applied** (not just reported), and the
`--videocaptest` measures it (Fast 2160px → Saver 720px with the rendered height → Manual removed); (−) the real
decoding resolution reduction is not implementable in CEF, so the memory promise is indirect in this area and
documentedly bounded.

## ADR-075 — The `<antikolt-card>` standalone package (2026-11-22)

**Context:** the card web component lived in the home page's own script (`app.js`) until now, grown together with the grid's internal globals
(`pageContainers`, `pageWorkspaces`, `pendingFocusId`) — exactly what the `04-UTEMTERV` open item
("today's card is the page's own component") wanted to eliminate. A user-authored home page thus could not load
the card.

**Decision:**

1. **The component goes into its own file** (`home/antikolt-card.js`), wrapped in an IIFE: `hashHue`, `STATUS`,
   `cardStyles`, the `AntikoltCard` class, and the `customElements.define` (guarded). The grid (`app.js`) and a
   user page load **the same** file — one source, not a copy.
2. **The management controls' data comes from the host**: the container/workspace `<select>` reads from
   `setOptions({ containers, workspaces })`; the grid calls it with the loaded tables, a user page
   leaves it empty (the card still renders, without the `<select>`s). The focus-restore `pendingFocusId`
   state is an internal `let` of the package, read out with `getPendingFocusId()`.
3. **The card still receives the data from outside** (`element.card = …`), it does not ask the core for it — the component
   is agnostic about where the record comes from, and the shell's push remains the grid's business.

**Consequence:** (+) one source for the card, a user-authored home page can load it; (+) the package is standalone, the
host's page cannot break the card; (−) the **full** editable home page (storing the user HTML,
serving it, and an editor surface) remains a separate, larger item — this slice provides the package, not the
home page editor.

## ADR-074 — The memory budget as a profile setting (F6) (2026-11-22)

**Context:** the `system.memory` bridge's `budgetBytes` was **always 0** so far ("we don't invent a budget"), and the grid
therefore only printed the used value. F6's budget control needs a real, user-adjustable number for this,
which the interface shows as a bar.

**Decision:**

1. **The budget is a row of `user_prefs`** (`memory_budget_mb`, schema **`0007`**): default `512` (MiB), validation
   `64..=4096`. The `ALTER TABLE … ADD COLUMN` runs only if the column is missing (following 0005), otherwise
   a half-finished migration would fail with "duplicate column". The field gets `serde(default)`, so that the pre-F6
   profile bundle import does not fail on the missing field.
2. **The shell caches it, does not ask per request**: `CoreClient::userPrefs` + `MainWindow::refreshUserPrefs()` after the core's
   `ready` and after the profile import; the `system.memory` response has `budgetBytes = memory_budget_mb × MiB`. The measurement
   (working set) remains the engine's, the budget is the profile's — the two are not mixed.
3. **The grid draws a bar**: the `#mem` field's `used / budget MB` text is complemented by a used/budget fill
   (`--mem-fill` inline variable), so the overrun is visible, not just a number.

**Consequence:** (+) the budget is real, persistent, and the profile migration carries it; (+) `--metricstest` now
also asserts `page_budget_bytes > 0` (instead of the old hard 0); (−) the settings surface (the `user_prefs_set` native
control) and the rest of F6 — per-card breakdown, resource profiles, raster home page — are still open.

## ADR-071 — The container isolation boundary: its own profile directory, proxy, and permission policy (2026-11-21)

**Context:** the container was a filter and a label until now. The engine created every surface with a `nullptr`
`CefRequestContext`, so **every card used a single cookie jar and cache** — the ADR-009
"one partition per card" promise was never fulfilled by the engine, the `partition` key was decoration. Proxy and
permission policy did not even exist.

**Decision:**

1. **A separate `CefRequestContext` per partition**, with a **single** directory under the profile root
   (`<user data>\persist_card_<hash>`). The directory is created by CEF; the ProfileManager only accepts a profile directly under the root
   (the `partitions\` subdirectory gave the "Cannot create profile at path" error, and the partition silently
   fell into memory — measured).
2. **The container is the policy's key**: the surface carries the container id in the `container` field, with the
   associated `proxy` / `proxy_bypass` / `permissions` fields. Two cards from one container get a shared proxy and permissions, but
   never a shared cookie jar. The fields may be missing in a given case (additive, `serde(default)`), so no
   protocol version bump is needed (proto/README.md's compatibility rule).
3. **The proxy is the container's setting in the core** (`proxy_mode`: `system`/`direct`/`fixed`, `proxy_host`, `proxy_port`,
   `proxy_bypass`), schema `0005`. The engine translates it into CEF's `proxy` preference, in the
   `CefRequestContextHandler::OnRequestContextInitialized` hook (runs on the first surface's creation, before the first request);
   it logs the feedback, it does not swallow it.
4. **Permissions are per container, denied by default**: `permissions` = name → `allow`/`deny` (geolocation,
   notifications, camera, microphone, clipboard, …). What the container does not name, the engine **denies**
   (`CefPermissionHandler`), because there is no prompt surface, and silent granting is the worst outcome. The `media` is a separate
   key, via `CefMediaAccessCallback`.
5. **The surface does not edit the policy yet**: the fields live in the core, the JSON export, and over the bridge, the container's
   rename sends back the full record (otherwise the policy would be lost) — the proxy/permission editor UI is the
   next slice.

**Consequence:** (+) the cards are truly isolated, and can be placed behind a container's proxy without switching the whole
browser; (+) the policy is a database row, so it survives rename, restart, and profile migration;
(−) the policy editor is missing, so today the user sets it only via the bridge/profile import;
(−) the partition profile directories remain in the profile, and deleting the card does not clean them (the deletion is the next,
cleanup slice's business).

## ADR-072 — User preferences (`UserPrefs`) in SQLite (schema 0006) (2026-11-22)

**Context:** ADR-070 made all four stores SQLite-first (schema 0004): cards, containers, workspaces, sessions. What is missing is the storage of user-level preferences: `max_live_cards` (the number of simultaneously live cards), `idle_hibernate_ms` (the time for hibernating an inactive card), and `cold_card_ms` (when a card becomes cold). They had no persistent state until now, and there was no IPC read/write for them.

**Decision:**

1. **New table: `user_prefs`**, with a single row (`id = 1`, `CHECK (id = 1)`), schema **`0006`** (`PRAGMA user_version = 6`). The migration is a `CREATE TABLE IF NOT EXISTS` over the existing v5 file; the `DEFAULT` values apply if it was never set, and the seed row is written with `ON CONFLICT (id) DO NOTHING`.
2. **`UserPrefs` struct** (`sqlite_store.rs`): `max_live_cards: u32 = 5`, `idle_hibernate_ms: u64 = 600_000` (10 minutes), `cold_card_ms: u64 = 86_400_000` (24 hours), `Default` + `defaults()` + `validate()`. The two timers are deliberately ordered: cold is the later state, never the earlier one.
3. **Validation:** `max_live_cards` ∈ [1, 12]; `idle_hibernate_ms` ∈ [60000, 3600000]; `cold_card_ms` ∈ [3600000, 2592000000]; and `cold_card_ms <= idle_hibernate_ms` is rejected (the cold window must be longer than the idle one, otherwise the card would cool before it hibernates). The errors surface as `SessionError::Rules(&'static str)`; `is_invalid_argument()` is true for `Rejected(_)` | `Rules(_)`.
4. **IPC ops:** `user_prefs` (read) and `user_prefs_set` (write, after validation). `profile_export` reads `sessions.user_prefs()` and puts it into the `Bundle`; `profile_import`, if `read.prefs` is `Some`, calls `sessions.set_user_prefs(prefs)`.
5. **`Bundle`** is extended with the `prefs: Option<UserPrefs>` field (`#[serde(default)]`), and `Bundle::new(...)` takes over the `prefs: UserPrefs` parameter.

**Consequence:** (+) the user preferences are persistent, the profile migration carries them; (+) the validation forbids meaningless combinations (e.g. `cold_card_ms <= idle_hibernate_ms`); (−) the schema goes 4→6, an older build cannot read a v6 file backwards.

## ADR-073 — Login popup: gesture → a real window in the same partition, 60 s (2026-11-22)

**Context:** "sign-in everywhere" goes through a popup (OAuth/SSO, `window.open`). The engine's
`CefLifeSpanHandler::OnBeforePopup` was not overridden until now, so by CEF's default *every* popup
opened, but landed on the same `EngineClient`: the popup's browser overwrote the card's `state_->browser`, and its
closing would have taken the card's surface down. The shell did not know about the popup, so it could hibernate in the
middle of the login.

**Decision:**

1. **The popup opens only on a user gesture** (`user_gesture == true` → allow, otherwise blocked). The
   gesture-less popup is like an unrequested permission: silent rejection. The `popup_requested` event carries the
   decision (`disposition`: `authenticated_window` | `blocked`) to the shell.
2. **The popup inherits the opener's partition** (CEF's default: the same `client`), so the cookies are shared — this is
   the essence of the SSO chain. The popup is **not a card**: no snapshot, no preview.
3. **The engine tracks the popup's browser separately** (`HostState::popup_browser`): its closing (`OnBeforeClose`) only
   ends the popup (`popup_closed` event), the card's surface lives on.
4. **The 60 s lifetime lives in the engine** (steady-clock, `kPopupTimeoutMs = 60000`): on expiry the engine
   closes the popup with `CloseBrowser` — the shell does not know the popup's surface id, so the engine is responsible. The
   shell, with the `popup_open` flag, only suspends hibernation until `popup_closed`.

**Consequence:** (+) the OAuth/SSO flow runs with the card's cookies, in its own partition, and does not break the
card; (+) the unrequested popups are blocked; (−) the gesture path is not measurable in the harness (the synthetic `window.open`
is not a gesture), so the `popup_requested`/`popup_closed` path is `not-measurable` — the decision is in the code and in `02` §8.5
recorded.

## ADR-070 — All four stores are SQLite-first (schema 0004) (2026-11-21)

**Context:** ADR-069 made the cards into a database, but the containers, workspaces, and sessions
remained JSON documents: their tables were only updated by `db_status(sync)`, and nothing read them. Thus the grid's
container list, the tab row, and the session panel still read a full list on every request, and after `container_delete`
the mirror tables could go stale. The swap was ripe: the tables and the schema were in place.

**Decision:**

1. **All four stores read from and write to `antikolt.sqlite3`**; the four `*.json` (cards, containers, workspaces,
   sessions) are **export**: the adoption, the shutdown, and `db_status(sync)` write them, atomically, and nothing reads
   them back except the profile before the switch.
2. **The adoption is marked per store in the `meta` table** (`adopted:cards`, `adopted:containers`,
   `adopted:workspaces`, `adopted:sessions`). The `user_version` only means the schema (`0004`); the adoption state does not
   depend on which store opened first (the four stores open four connections to the same file). The `0004` migration
   records `adopted:cards` in a v3 file, because there the cards were already adopted — without this the `cards.json`
   could be written back over the newer rows.
3. **The session table carries the ledger's full row** (state, policy, cookie/backing-store sizes, service
   workers, `last_seen`/`last_verified`, RAM, `keep_alive`), the policies in a one-row `session_rules` table; the
   enumerations with the `as_db`/`from_db` mapping, an unknown value is a **loud error**, not a guess.
4. **Adopting one store:** the document wins (it was the writer); missing document → seed (4 containers, 3 workspaces,
   empty ledger); empty document → stays empty; unreadable → the core **backs off** (`*Error::Legacy`, with the file name), it does not
   seed over the data.
5. **`db_status` checks all four exports row by row** (`checked`, `mismatches`, `verified`); an unreadable
   export is `-1` (`not measured`), never "matches".
6. **The orderings** are the surface's: containers and workspaces in insertion (legend/mirror) order (`ORDER BY rowid`),
   sessions by origin.

**Consequence:** (+) no more manual mirroring — `sync_reference_tables` is gone, because there is nothing to mirror; (+)
every list and panel request is an indexed read; (+) the adoption is restartable per store, and one corrupt document does not
take the others with it; (−) four connections open to the same file (fine with WAL; the schema migration runs once);
(−) an old build reads a v4 file with `user_version=4`, but would keep writing the JSON, so the backward jump is not
supported.

## ADR-069 — The card store SQLite-first: JSON will be the export (2026-11-21)

**Status:** points 1–5 are live; point 6 (the other three stores remain JSON for now) was replaced by ADR-070.

## ADR-069 — The card store SQLite-first: JSON will be the export (2026-11-21)

**Context:** ADR-064…066 built a mirror: the JSON file wrote, the database followed. This proved the migration
on real data, but the sync of the two writes had to be kept by hand — ADR-066 was born precisely because the
`container_delete` `move_to` branch left silent staleness in the mirror. The grid's list already came from the database, so
the meaningful half of the swap existed; the write was missing.

**Decision:**

1. **The single writer of the cards is `antikolt.sqlite3`** (`CardStore` via `Db`): `card_upsert`, `card_open`,
   `card_page_text`, `card_delete`, container moving and profile import all write there. The mirror (`Db::mirror`,
   `mirror_card`, `mirror_delete`) is gone.
2. **The `cards.json` is an export**, not a store: the core writes it on adoption, on shutdown and on `db_status(sync: true)` (by atomic
   replacement), and reads it **once**: when adopting a pre-switch profile. It does not rewrite it at startup: the export
   is derived, and serializing the whole list on every startup would bring the old O(n) write back onto the startup path.
3. **One-time adoption, with schema `0003`:** `Db::open` remembers which `user_version` it saw; if it is old (0–2),
   then `cards.json` was the truth, so it adopts it (with store normalization), and only **after the adoption**
   writes the `3` (`mark_switched`). If the JSON is unreadable, the core **backs off** (`StoreError::Legacy`, naming the file
   explicitly) — it does not seed over the user's cards. If the adoption **drops rows** (the mirror lagged behind, or the
   document emptied), it first saves the database with `VACUUM INTO` (`antikolt.pre-adoption.sqlite3`): the JSON wins,
   but the other copy does not disappear.
4. **An empty profile is not re-seeded:** the sample cards are created only if the database file is new **and** there is no
   JSON to adopt. Whoever deleted all their cards gets an empty grid on the next startup too.
5. **`db_status` measures DB ↔ export agreement, row by row** (`checked`, `mismatches`, `verified`). `sync` updates the
   derived copies: the export from the cards, the container/workspace reference tables from the
   JSON stores. An unreadable export is `mismatches = -1` (`not measured`), it never "matches".
6. **Containers, workspaces and sessions remain JSON documents** — their own swap is a separate slice;
   until then the `db_status` reference tables update on demand, and nothing reads them.

**Consequence:** (+) there is one writer, so there is no more manual mirroring and no "the count matches, yet a stale row"
error class (that was the cause of ADR-066); (+) the `container_delete` move is a single SQL `UPDATE`; (+) the JSON remains
for human eyes and as a backup, but it can no longer stray into the product path; (−) `cards.json` can go stale after the last
export/shutdown — `db_status` says so (`verified: no`) and the log records it; (−) an old
build reads a new file with `user_version=3`, but it would keep writing the mirror, so jumping backwards is not supported.

## ADR-068 — Deletion in two steps: first arm, then delete (2026-11-19)

**Context:** `cards.delete` (ADR-061) deleted with a single click. But the card is a **large click target**,
which the user clicks to open — an `×` inside it can carry away data with a single mistaken click.

**Decision:** there is an `×` in the card's meta row, and deletion is **two steps**:

1. the first click **arms** the card: the button text becomes `Biztos?`, with a red background, and the page-side state
   (`armedDelete = {id, at}`) shows this;
2. the second click **within five seconds** deletes (`cards.delete`, it draws after the core's response);
3. after five seconds the arming **expires on its own**, because a card left armed for a minute is a trap for the next mistaken
   click.

**Consequence:** (+) a mistaken click does not carry away a card; (+) the confirmation is **not a dialog**, so it does not collide
with the CEF `prompt` problem (ADR-053) and it is measurable; (−) the measurement lagged for a long time: first the CEF GPU process died
(`GPU state invalid …`, `exit_code=-2147483645`), then the engine's stdio lifecycle (`stdin closed` → the engine host exits).
Both resolved (log slices 90–91), and `--addcardtest` has been **measured, green** since:
`delete_armed=ok, delete_card=ok (remaining=10 expected=10), add_card_check=ok` (`exit=0`, 11,9 s).

## ADR-067 — The database returns cards in the store's order (2026-11-19)

**Context:** the JSON store sorted in Rust (descending by `last_opened_at_ms`, then by title). Once the grid's list
comes from the database (ADR-065), the unsorted `SELECT` returned **insertion order** — the grid would have silently
stopped reordering when a card was opened. This is exactly the kind of silent behavior change that a
store swap must not cause.

**Decision:** `ORDER BY last_opened_at_ms DESC, LOWER(title) ASC` in `all_cards()`, and a Rust test
(`the_database_returns_the_stores_order_not_the_insertion_order`) that writes two cards in **deliberately reversed insertion
order** and expects the store's order. If someone removes the `ORDER BY`, the test fails.

**Consequence:** (+) the grid's order after the switch is the same as before, and a test protects this; (−) the sort now
lives in two places (Rust store and SQL) — when the JSON store is retired, SQL will remain the only one.

## ADR-066 — The mirror follows writes row by row; the list now only reads (2026-11-19)

**Status:** obsolete — superseded by ADR-069: the cards have a single writer (the database), the mirror is gone,
so there is nothing left to mirror by hand.

**Context:** ADR-065 rewrote the whole mirror on each list request — correct, but an O(n) write per request.

**Decision:**

1. **Every card write updates its own row** (`mirror_card`), deletion removes it (`mirror_delete`); the list only
   reads.
2. **An op that writes several cards mirrors all of them** — otherwise the `container_delete` `move_to` branch causes silent
   staleness: the count matches, so no self-repair starts, and the grid shows the old container.
   `--containertest` caught this (`container_move_delete=FAILED … card_in=c-proba`).
3. **Self-repair only on a count mismatch**, and even then **loudly** (`[core] db_list_out_of_step … — rebuilding`).

**Consequence:** (+) the list request is a read, not a write; (+) the mirroring of multi-card operations is explicit and measured;
(−) the JSON store is still the writer, so the sync of the two writes must be kept by hand — the next slice removes this
(writing into SQLite, JSON as export).

## ADR-065 — The grid's list comes from the database (the second half of the swap) (2026-11-19)

**Status:** obsolete — superseded by ADR-069: the list still comes from the database, but it no longer needs to be mirrored
beforehand, because the write also goes there.

**Context:** ADR-064 created the mirror, but the grid still read from the JSON file. The point of the swap is the
read: as long as the list comes from the JSON, the schema, the indexes and the queries mean nothing.

**Decision:** the `cards_list` op **mirrors first, then reads** (write-through):

1. the JSON store is still the **writer** (`card_upsert`/`card_open`/`card_page_text`/`card_delete` write there),
2. on a list request the core writes the JSON list into the database (`Db::mirror`, a transactional swap), then reads the cards
   back **from the database**,
3. if the count does not match (so the mirror cannot be right), the core **does not serve uncertain data**: it returns the JSON list
   and signals it with a `[core] db_list_mismatch` line on stderr. Likewise, if the database does not open.

**Consequence:** (+) the grid now reads from the database, and the switch does not risk data loss, because the write
location did not change; (+) the error cases are **loud** (stderr), not a silent fallback; (−) the mirroring is an
O(n) write on every list request, which is waste at 5 000 cards — a deliberate intermediate state that the next slice removes
by making the **write** also go into the database and the JSON just an export.

## ADR-064 — The database arrives: SQLite schema `0002`, with mirroring (2026-11-19)

**Status:** obsolete — superseded by ADR-069: the schema is `0003`, and card writes also go into the database, not
into the mirror.

**Context:** the four stores are JSON documents with atomic writes — readable and enough for 10 cards, but **they answer no
queries**. Every "which cards are in this container", "what is expiring", "how many are in this
workspace" question walks the whole list. At 5 000 cards this is the wrong answer, and that is why the store swap
is on the schedule.

**Decision:**

1. **`rusqlite` with the `bundled` feature**: SQLite compiles into the binary, there is no system DLL dependency.
2. **The schema in `PRAGMA user_version`** (not in a table): `2` is today's version; the `cards` table carries every field of the JSON record
   (`page_text`, `workspace`, `tags` as JSON text — no join table for a 12-element list).
3. **The migration is idempotent**, and the JSON stores **remain the truth** for now: the `db_status` op, with `sync: true`,
   mirrors the four stores' contents into the file, then **reads the first card back** and compares the
   fields (`verified`). The swap happens once this mirror provably matches on real data.
4. **WAL logging**: a reader should not wait on a writer, and a crash should not take the last transaction.

**Consequence:** (+) there is a database, migration, card round-trip and two real queries (workspace counting),
all measured in the Rust tests; (+) the JSON stores keep working untouched, so the product did not become riskier;
(−) **the swap has not happened yet**: the shell still reads the JSON files, the `db_status` op is for now
a sidetrack; the mirroring (the `sync`) is only useful if someone calls it — the shell's current path does not call it.

## ADR-063 — The web address is also editable on the card (2026-11-19)

**Context:** the title (ADR-062) and the note were editable, the **web address** was not — yet the card's *purpose* is that
field where a typo silently persists: the card keeps opening the old page, and the user believes they
rewrote it.

**Decision:** the domain in the meta row is the handle — **double click → input field → `Enter` saves, `Esc` cancels**, and
the save uses the whole record's `cards.upsert`. The entered address is normalized (if there is no scheme, it becomes `https://`), and the
displayed domain is recomputed from the core's response (`pageCardFromCore`), so the UI never lies about
where the card points. The save **writes first, then restores the DOM** — the order the title editor learned
because of the `blur` re-entry (ADR-062).

**Consequence:** (+) every human field of the card (title, web address, note, tag, container, workspace)
is editable in the UI; (+) the domain comes from the core's response, not from what the user *typed*; (−) the
web address change does not reopen the card (the next opening uses the fresh address), and the old page's
captured text (ADR-049) remains the old content until the card is re-frozen — the log states this.

## ADR-062 — Renaming the card in place (2026-11-19)

**Context:** after ADR-061 cards could be added and removed, but their **title could not** be rewritten in the UI —
`cards.upsert` knew how, the page did not call it.

**Decision:**

1. **Double click on the title** → an input field in the title's place (`Enter` saves, `Esc` cancels), and the save goes through the same
   `cards.upsert` path that carries the whole record (so the container, the tags, the note and the captured
   page text are not lost).
2. **It is also reachable by name on the page path** (`window.antikoltRenameCard(id, title)`), because a future menu item
   does not have to simulate a double click.
3. **The editor calls the save directly on the page path** (not through an event): an event launched from the card's shadow content
   did not reach the grid, and a change typed by the user must not depend on an event that does not arrive.
   The event is only a fallback in an environment without a page.

**Consequence:** (+) rewriting the title works in the UI — the double click opens the field, `Enter` saves, and the
core's response is drawn (`--addcardtest`: `wrote_by=gesture`); (+) the whole record goes across, so a container,
tag, note or page text cannot be lost; (−) the editor **writes first, then restores the DOM**: the first
version began by restoring the title, whereupon the browser sent `blur`, whose `finish("", false)` branch
re-entered and flew away, so the save was missed — the measurement (`wrote_by=path`, empty `editor`) showed this, and that is why
the order flipped; (−) editing the web address (URL) still does not exist.

## ADR-061 — The home page is editable: adding and deleting cards on the page (2026-11-19)

**Context:** the user could not create a card in the UI — the grid showed the core's cards, but a new
card could only get in through the core's `cards.upsert`. A browser you cannot put cards into is not
a browser. Deletion did not exist either: the core had no `card_delete`.

**Decision:**

1. **A `＋` button next to the tab row**, an inline form on the grid (`webcím` + optional `cím`), `Enter` saves, `Esc` cancels.
   The dialog is absent for the same reason as elsewhere (ADR-053): under CEF, `prompt` would silently return `null`.
2. **A card has two things one form cannot ask about**: the workspace and the container. So the new
   card goes to the **active tab** and the **first container** — these two are "here and now" for the user.
3. **The id is ULID-shaped** (`01M7T7…`, 26 characters), because this way the store can express creation order lexicographically
   too; the title, if empty, becomes the web address's domain.
4. **`card_delete` in the core** (`CardStore::remove`): deletion is the counterpart of adding, and the UI needs it,
   so that a mistyped web address does not have to be fixed by editing the file.
5. **Deletion on the card** goes through the page path (`antikoltDeleteCard`), drawing after the core's response — like every other write.

**Consequence:** (+) the home page is "editable" in the most important sense: cards can be added and removed; (+)
adding goes through exactly the same `cards.upsert` path as pinning/note/tag/container, so the grid
never shows anything the core did not accept; (−) **editing** the card (rewriting the title/web address/note in the
UI) does not exist yet, only creation and deletion; (−) deletion is immediate, without confirmation — the
`confirmed` step is a later decision, because the confirmation dialog is the same CEF problem.

## ADR-060 — The bundle's path across the bridge: the caller supplies the path, the bridge opens no dialog (2026-11-19)

**Context:** ADR-059 made the move ready in the core, but the page could not reach it: neither the bridge nor the page could
export. The "Save to file" button, however, wants a native `QFileDialog`, which the harness cannot
click — and a dialog opened inside a bridge request would also stall the page's response.

**Decision:**

1. **The bridge gets `profile.export` / `profile.import` methods, with a `path` argument.** The caller supplies the path;
   the bridge never opens a native UI in the middle of a request.
2. **The page-side entry point:** `window.antikoltProfileWrite(path)` / `window.antikoltProfileRead(path)` — this is the
   point the button (and the test) calls. The dialog will later go *in front of these*, the bridge stays unchanged.
3. **The test measures on the real path:** after the two headless core pairs, `--profiletest` opens a **window**, exports through
   the page into the bundle, then reads it back — it reads the answer from the page's own variable (`window.__profWrite`)
   because `evaluate` does not await the promise.

**Consequence:** (+) the move reaches the UI, and it stays measurable (there is no dialog in the measurement's path); (+) the
bridge's contract contains no UI decision; (−) the *button* and the file picker do not exist yet, so today the user
cannot click yet — `19-FOLYTATAS.md` §12 and `21-ALLAPOT.md` state this.

## ADR-059 — The profile travels in a portable JSON bundle (`profile_export` / `profile_import`) (2026-11-19)

**Context:** the user's data lives in four separate files in the profile (`cards.json`, `containers.json`,
`workspaces.json`, `sessions.json`). The documentation promises "moving", but there was no way: moving from one machine to another
would have required carrying the profile folder — by hand, knowing the file names. This is not a user feature.

**Decision:**

1. **One bundle: `schema`, `created_at_ms`, `cards`, `containers`, `workspaces`, `sessions`, `rules`.**
   JSON, not zip: the four stores are already JSON, the bundle is small, and a backup readable with a text editor is
   what a person can trust.
2. **`profile_export {path}`** writes from the stores (atomically: temp + rename, like every other write), and
   reports what it moved (cards/containers/workspaces/sessions).
3. **`profile_import {path}` validates first, then writes.** An unknown `schema` is **rejection** (`version_mismatch`),
   not guessing — and reading the file is the *first* step, because a half-succeeded import is worse than one never
   started. The stores are swapped with `replace_all`, the cards go through the same normalization as on a
   write (so the tag rule cannot be bypassed with an import either).
4. **`sessions` is in the bundle**, because the logins and the cleanup rule are also part of the profile.

**Consequence:** (+) the profile can be moved and saved with one file, and `--profiletest` measures that after importing into the fresh
profile, the *file* contains what the export carried out; (+) the rejection of a bad/newer bundle
states what this core speaks; (−) the bundle **also contains secrets** (session rows, cookie
metadata), so protecting the file is the user's job — encryption is a later decision, and
`19-FOLYTATAS.md` states this.

## ADR-058 — The card's workspace is switchable on the card, with a native `<select>` (2026-11-19)

**Context:** after ADR-056/057 the workspace table and the tab row's lifecycle lived in the core, but a card's
workspace could not be changed in the UI — only through the programmatic path (`antikoltMoveToWorkspace`). A
setting the user cannot reach is not a setting.

**Decision:** in the card's meta row, next to the container selector, there is a **second, narrower native `<select>`**
(`.workspace-select`, `aria-label="Munkaterület"`). The switch is the same path as the container's: `change` event →
`setWorkspace` → `cards.upsert` → the core's response draws. **Why not drag-and-drop:** drag is fragile in the grid's virtualized
DOM (the card is an `antikolt-card` shadow host that rebuilds on scroll), and it is
not usable with the keyboard; the `<select>` gives both for free. Drag can be added *beside it* later.

**Consequence:** (+) the workspace switch is also available in the UI, with the same gesture as the container switch; (+)
the test uses the same selector as the user (`--workspacetest`); (−) two dropdowns in the meta row
is more crowded, so the workspace selector appears with smaller type and narrower.

**Confirmed (slice 123):** the "can be added *beside it* later" branch has arrived — the card is `draggable`, can be dropped onto the tab
(`application/x-antikolt-card` MIME type, the tab's `dragover` accepts only this), the `drop` calls the same
`setWorkspace` as the `<select>`; the `<select>` remains the keyboard path. The wiring is proven by `--workspacedragtest`
(`workspace_drag_wired`/`_gate`/`_move`/`_restored` all `ok`).

## ADR-057 — The workspace lifecycle on the tab row, with the same gestures as the containers' (2026-11-19)

**Context:** ADR-056 put the workspace table into the core, but in the UI one could not create,
rename or delete a workspace — the bridge knew, the UI did not call it. The same gap as with the containers before
ADR-053.

**Decision:** the tab row gets the same three gestures, in the same form: `＋` (inline input field, `Enter`
saves, `Esc` cancels), double click on the tab (rename), `×` (delete). **No dialog** — `prompt` under CEF
would silently return `null` (ADR-053). Deleting a used workspace **displays** the core's `in_use` response
on the tab row ("1 card is still in this workspace"), it does not swallow it. The new id is a slug derived from the name
(`w-` + the accent-free form, with a sequence number on collision).

**Consequence:** (+) the workspace lifecycle is complete in the UI, and every operation goes through the core, whose
response the tab row draws; (+) the two tables (container, workspace) follow the same pattern, so a
user has to learn one gesture; (−) *moving* a card's workspace still only goes through the programmatic
path (`setWorkspace`), without drag-and-drop/menu.

## ADR-056 — The workspaces also live in the core, and the card's workspace is a real field (2026-11-19)

**Context:** the page's `pageCardFromCore` guessed the workspace from the card's *position*
(`index > 12 ? "w-read" : "w-home"`). This was not an approximation but an error: every thirteenth card silently landed on the
"Reading" tab, whether the user wanted it or not — and the tab row also lived in the page, not in the core.

**Decision:**

1. **`workspaces.json` in the core** (`workspaces_list` / `workspace_upsert` / `workspace_delete`), with the three
   shipped workspaces on first startup (`w-home` Home, `w-work` Work, `w-read` Reading).
2. **`Card.workspace: String`** (`#[serde(default)]`): the card's workspace is **stored data**. The empty field means
   the first workspace — this is what cards before the table mean, and a missing field must not
   move a card onto a tab the user did not choose.
3. **The table goes by push** to the page together with the card list and the container table (`antikoltAcceptWorkspaces`),
   for the same reason as the containers (ADR-052): the bridge script installs at the end of load, which can be later than the page's
   own script.
4. **Deletion follows the same rule**: as long as a card is in it, `in_use` — silent moving is forbidden. Moving goes through
   the card (`cards.upsert`, `workspace` field), the page path (`setWorkspace`) is ready; the drag-and-drop
   UI does not exist yet.

**Consequence:** (+) the thirteenth card no longer lands in Reading on its own; (+) the tabs' names and row belong to the
core, so a rename survives a reload; (+) the card's workspace is written on the same path as the
container, the tag, the note and the pin; (−) *UI* movement of the card's workspace (drag/menu) does not exist
yet, only the programmatic path — `19-FOLYTATAS.md` §12 states this.

## ADR-055 — Tags on the card, normalized in the core (2026-11-19)

**Context:** the search field's placeholder has promised "tag" since time immemorial (`Keresés — cím, webcím, jegyzet, címke,
oldalszöveg`), yet the card record had no tag field: the promise was a lie. The page's `matchesTerm`
nevertheless examined a `card.tags` array that never existed.

**Decision:**

1. **`Card.tags: Vec<String>`** in the core (`#[serde(default)]`, so the old `cards.json` loads without trouble).
2. **Normalization belongs to the core**: trim, case-insensitive deduplication (the first spelling stays), at most 12
   tags, each at most 32 characters. The rule lives in the store, because every writer meets there (the page, a
   later importer, the tests) — an importer would not follow a rule living in the page. Truncating to length is **not an error**:
   a long tag shortened is still a tag, but losing the save is data loss.
3. **Editing on the card, in place**: next to the chips, a `＋címke` button opens an input field; `Enter` saves,
   `Esc` cancels (the same pattern as the containers — no JS dialog, ADR-053). Deletion is the chip's `×`.
4. **Search sees it without accents too** (`kavezo` → `kávézó`), because the tags land in the same haystack as
   the title, the web address, the note and the page text.

**Consequence:** (+) the search's promise became true, and the dead branch of `matchesTerm` is alive; (+) the tags are written on the same
`cards.upsert` path as the pin and the note, so the grid shows the core's response; (−) the tags do not yet
appear in the `.antikolt` export as a separate list (they are part of the card record), and there is no tag autocomplete.

## ADR-054 — Deleting a used container with a move (2026-11-19)

**Context:** since ADR-052 the core refuses to delete a container that still has cards in it (`in_use`).
This was right — no silent moving — but a **dead end**: the user could not delete their old
grouping.

**Decision:**

1. **`container_delete` gets an optional `move_to` field.** If the container is used and `move_to` is present, the core
   **moves the cards first** (in one save, `CardStore::move_container`), **then deletes**, and the response
   says how many cards migrated (`moved`). If there is no `move_to`, the `in_use` error remains; if the `move_to` does not
   exist or is the deleted container itself, `invalid_argument`/`not_found`.
2. **The order is not accidental:** the move is written out first, then the deletion. If the deletion fails, the cards
   are still somewhere — they do not disappear.
3. **The UI turns the dead end into a resolution:** after the `×`, if the core says `in_use`, the legend opens a row
   ("1 card is still in this container"), with a selector for the **other** containers and an
   "Move and delete" button. One click performs both.

**Consequence:** (+) the old grouping can always be deleted, but never silently; (+) the core decides, the page only
offers; (−) the UI offers only *one* target at a time (no "all here, except this one"); and moving the cards
does not ask, which is why the `in_use` error message still exists on the bridge.

## ADR-053 — Editing the container in place, without a dialog (2026-11-19)

**Context:** ADR-052 put the table into the core, but in the UI one could not create, rename
or delete a new container — the bridge knew, the UI did not call it.

**Decision:**

1. **An inline, in-place editor** at the end of the legend: the `＋ konténer` button or a double click on a chip writes out an
   input field, `Enter` saves (the form's `submit`), `Esc` cancels. **No `prompt`/`confirm`**: for the CEF-side
   JS dialog the host would have to provide `CefJSDialogHandler`, which does not exist — the `prompt` call would silently
   return `null`, and the UI "does nothing".
2. **The new id derives from the name** (`c-` + accent-free slug, sequence number on collision), so that a person can say it
   out loud; the color is generated by the core (ADR-052).
3. **Deletion with `×`**, and the core's `in_use` response **appears** in the legend ("1 card is still in this
   container") — a silently swallowed error is exactly the kind of lie this project does not allow.

**Consequence:** (+) the container lifecycle is complete in the UI: create, rename, delete, filter; (+) every
operation goes through the core, and the legend draws the core's response; (−) the input field appears in the legend's row
(the grid slides down one row), and there is no "cancel" button, only `Esc`/`Enter` — documented in the hint.


## ADR-052 — The container table lives in the core, and arrives at the page by push (2026-11-19)

**Context:** the containers' names and colors were so far in the page (`PAGE_CONTAINERS`, a constant), the core only stored
a `container` id on the cards. A rename thus did not survive a reload, and "the same" container
could mean two things in two layers.

**Decision:**

1. **The table belongs to the core** (`containers.json`, `containers_list` / `container_upsert` / `container_delete`). On
   first startup the core seeds the four shipped containers, so an old profile loses nothing. The color
   is `#RRGGBB`; if it is missing or bad, the core generates a deterministic color (same id → same color).
2. **Deletion does not move silently.** As long as a card references a container, `container_delete` refuses
   with an `in_use` error: a silent move is the "my cards disappeared" error class.
3. **The page draws the core's, and the table comes by push** together with the card list (`antikoltAcceptContainers`). The
   page does not ask: the bridge script installs at the *end* of load, which can be later than the page's own script — a measured
   error was that the legend drew the fallback table while the fifth container was already in the core.
4. **The page's constant is only a fallback**: in a browser, without the core, the four shipped containers appear.

**Consequence:** (+) the rename and the new container survive a reload, because they are in the core; (+) the color and
name source is in one place, so the dot and the legend cannot diverge; (+) deletion does not reorder cards
unnoticed; (−) the UI button for *creating/renaming* a container does not exist yet (the bridge knows, the UI
does not call it), and the page's fallback table stays in `app.js`.


## ADR-051 — The container legend is also the filter (2026-11-19)

**Context:** the colored dot on the cards showed which container a card belongs to, but nothing said what the colors
meant, and filtering by container was impossible.

**Decision:**

1. **A row of chips above the grid**, one per container: colored dot + name + **count**. The number refers to the current
   search, not to the whole store — a legend that writes "3 cards" while the search hides
   two lies.
2. **The chip itself is the filter**: on click the grid shows only that container's cards, another click removes it.
   One container can be active at a time: the question it answers is "which container is this?", not "which
   intersection of these three containers?".
3. **The filter is view state, not a stored setting.** A new startup shows everything: a forgotten filter is a trap
   ("where are my cards?"). The chips signal the state with `aria-pressed`, and the numbers update together with the
   filter.

**Consequence:** (+) the meaning of the colors is visible, and filtering by container is one click; (+) the numbers move
together with the search, so they cannot contradict the grid; (−) the filter does not persist across sessions
(deliberate), and the container *table* still lives in the page, not in the core — creating a new container and
moving the table into the core are open.

## ADR-050 — Switching the container from the card, with a native `<select>` (2026-11-19)

**Context:** the containers so far only *appeared* (a colored dot on the card, from the page's own table), but the
card–container binding could not be changed in the UI — the roadmap's "card–container binding, container
switch" item was open.

**Decision:**

1. **The selector is on the card**, in the meta row: a native `<select>` whose options are the containers. Not
   a custom dropdown: the native element brings keyboard handling, the screen reader and the platform menu
   for free, and the colored dot stays beside it (the dot's color = the container's color).
2. **The write goes on the usual path**: `antikolt-container` event → the grid's `setContainer` →
   `cards.upsert` → the core. The grid only shows what the core accepted (the same rule as with
   pinning and the note), and the error shows in the `window.antikoltContainerState()` witness.
3. **The model and the record stay separate**: the view holds a container *object* (color + name), the record only an id,
   so the page translates the response's id back on the same table it read the cards with.

**Consequence:** (+) the binding on the card can be switched with one gesture, and the decision is the core's; (+) the color and the name
come from one place (the page's table), so the dot and the menu cannot diverge; (−) the container *table* still lives
in the page, not in the core — creating a new container, renaming, and filtering by container remain open
(the `19-FOLYTATAS.md` §12).

## ADR-049 — The page text is also searchable, but only for cards saved to disk (2026-11-19)

**Context:** ADR-047 stated that the search does not promise page text, because nothing indexes the articles'
text. This was honest, but incomplete: the user often remembers a **sentence**, not the title.

**Decision:**

1. **The text is produced at freeze time.** When the card goes to disk, the shell asks the page for its
   `document.body.innerText` (max. 8000 characters), and writes it into the card's record with the core's `card_page_text` op.
   Not a separate index: a field of the card record, so there is no new service to maintain.
2. **A separate op, not `card_upsert`.** The shell does not own the whole record (container, note, title);
   sending back a half-finished card would overwrite what it never read. So `card_page_text` writes only the text,
   by id.
3. **Attachment and a cap.** The core collapses whitespace and truncates to `MAX_PAGE_TEXT = 16 000` characters. The
   card list is a single JSON document: 500 cards × 16 kB is the upper bound, not 200 MB. The page's side also truncates
   (8000), so the network path is bounded too.
4. **The pin and the note do not delete it.** A `card_upsert` from the page never carries text; if it comes empty, the core
   keeps the earlier capture. Only a new freeze overwrites it.
5. **Search sees it in all three places**: the core's `cards_list` emits the `page_text` field, the shell's
   `cards.search` puts it into the folded haystack, and the page's `matchesTerm` also checks the `pageText` field.

**Consequence:** (+) a word seen in an article finds the card, even without accents; (+) no new index, no
new service; (−) it only works for **frozen** cards (a live card has no text yet), and the text is valid
at the moment of freezing — it does not follow later page changes; (−) the card record grows, hence the cap.

## ADR-048 — Cleanup also does not silence the playing card (2026-11-19)

**Context:** hibernation has long made an exception for the card playing media (docs/HU/02 §4.2), but **session
cleanup does not**: if the origin due under the `wipe`/`sweep` policy was just the card on the screen's,
the shell immediately deleted its storage, and a video playing in the foreground fell silent. `19-FOLYTATAS.md` §2 kept this
on record as an open item.

**Decision:** cleanup asks the page the same thing as hibernation — with a single, both-place-used
script (`LiveMediaPlayingScript`: `video`/`audio`, `!paused && !ended`) — and if the answer is `playing`, then the
deletion of the **live** origin is skipped, and the status bar says why ("the card is playing media — the live
session is kept"). The plan for non-live origins still waits for the next opening.

**Why the page is the decision maker, and not the shell's guess:** the playback state is a property of the document
(`paused`/`ended`), not of the shell's bookkeeping; inferring from the title or the URL is exactly the kind of guessing
the project avoids elsewhere too. A page that does not respond (error) gets no privilege: in that case
cleanup runs the old way.

**Measurement (on this machine):** `--mediacleantest` raises a due `wipe` row through the core, opens the
origin's card, and requests cleanup. The machine **has no audio device**, so Chromium does not even start the player
(measured: `paused=true`, `readyState=0`, and the `/tone.wav` request **did not even start**) — the test states this too
(`media_skip=not-measurable`), it does not pretend to have measured it. What it did measure: cleanup runs on the new, media-aware
path, and without a `playing` answer (idle) the deletion happens (`media_wipe_when_idle=ok`), the card's
storage becomes empty. On a machine with an audio device the same test also measures the `playing` branch (the code is there, the
`media_skip` line awaits it).

**Consequence:** (+) background cleanup cannot silence a page playing in the foreground; (+) the two paths
(hibernation, cleanup) use the same script, so they cannot diverge; (−) the card's state is decided one
DevTools round later, so cleanup is a little slower, and for an unresponsive page the old
behavior remains.

## ADR-047 — Search is accent-independent, and does not promise page text (2026-11-19)

**Context:** the grid's search so far compared with `toLowerCase()`, so `kave` did **not** find the
`Kávézó` card — exactly the word a Hungarian user types. The placeholder text, moreover,
promised "page text", while nothing indexes the articles' text (the core and the shell search only title, web address,
note and tag).

**Decision:**

1. **Accent-independent matching on both sides.** The page's `foldSearch` and the shell's `FoldForSearch` do the same
   thing: lowercases, decomposes to Unicode NFD and removes the combining marks (`ő` → `o`, `ű` → `u`), then maps the
   decomposition-stable letters (`ł`, `ø`, `đ`, `ß`) by hand. On both sides, because the question must not
   answer differently depending on whether the page or the shell filtered.
2. **An empty result points at the search, not at nothing.** The grid shows "No match for: »…«" with the
   searched term, and the card count is empty — the earlier silent emptiness suggested the page had broken.
3. **The placeholder does not promise page text.** "title, web address, note, tag (also without accents)" — page-text
   search would require indexing the hibernated page's text, so it remains an open item of `19-FOLYTATAS.md` §12,
   and until it exists, the UI does not claim that it does.

**Consequence:** (+) search adapts to the user's typing, not to the stored form; (+) the empty state
says what can be done (fewer words, deletion); (−) the fold is approximate: the `ß` → `ss` and the Polish `ł` → `l`
are a hand-maintained list, so exceptions may remain in rare languages; (−) the page text is still not
searchable — this is a deliberate promise retraction, not a hidden shortcoming.

## ADR-046 — The shell's log file is the console that does not exist (2026-11-19)

**Context:** slice 55 throws away the console window that Windows gives to shortcut launches — and with it
the stderr too, so the engine's messages (GPU errors, page-side error messages) went into the void. On a
machine where nobody looks, an error thus disappears without a trace.

**Decision:** through the **existing** message handler (`ForwardQtMessages`) the shell also writes every line to
`%APPDATA%\Antikolt Project\Antikolt\logs\shell.log`, with a timestamp, and at 1 MB it keeps **one generation**
(`shell.log.1`). The log file path is set **only for plain launches** (empty command line): a program run from the terminal
already has stderr, and the self-tests must not write into the user's log. The limit
is adjustable with an environment switch (`ANTIKOLT_SHELL_LOG_LIMIT`), so `--logtest` rotates with a few kilobytes.

**Why not a new logging layer:** most of the errors come from the engine, which directs its own stderr to the shell's `qInfo`;
a new, parallel logger would only ensure that the text differs in the two places. We format in one place,
and the same string goes to stderr and to the file.

**Consequence:** (+) a shortcut launch is also diagnosable, and because of the rotation the disk does not fill up;
(+) the log is the same as what we would see in a terminal, so it can be copied into a bug report; (−) the log may contain
page titles and error messages (but **no password, no TOTP** — I5 still holds: the secret does not get into the main world or
the log); (−) the file write also runs on the error-handling path, so it is deliberately dumb (open/append/close), and
even a full disk cannot turn logging into an error.

## ADR-045 — Page darkening has several levels, and the theme switcher is an icon (2026-11-18)

**Context:** after ADR-044 the user asked that **there be several kinds of dark mode for the pages too**, not
only for the browser, and that the theme switcher buttons appear as **icon + tooltip** (the "light grey dark
amoled" label was four words in the widest, least informative part of the control bar).

**Decision:**

1. **Four page-darkening levels** (the `set_forced_dark` `mode` field, the earlier `enabled` boolean field
   stays backward compatible): `off`, `invert` (near black), `dim` (dark gray, `contrast(0.72)`),
   `warm` (warm dark gray, `contrast(0.78) sepia(0.35)`). Each is the same trick at a different strength: inverting the page
   and re-inverting the images, with a **named** style element that the mode change *replaces*,
   and switching off *removes* — without a reload.
2. **The theme switcher icons**: the four UI themes each get a generated icon (the theme's background + text color, 20 px),
   the word in the **tooltip** and the screen-reader name; page darkening is a crescent-moon icon, selectable from the menu, and its
   **icon shows the currently active level**.
3. **The setting persists** (`QSettings`, `ui/page_dark_mode`), and the old `ui/forced_dark` boolean key is converted
   to `invert` on first startup, then deleted.

**Why not Chromium's own auto-dark:** see ADR-044 (process-level, version-dependent). `contrast()` and
`sepia()` use the same page-level mechanism, so there is no new mechanism, only a different level.

**Consequence:** (+) the user can choose between full dark and levels gentler for long reading;
(+) the control bar got narrower, and the words are in the tooltip, where people look for them; (−) the inversion
is approximate: photos may differ even when re-inverted, and shadows and gradients behave
differently per level — that is why "Off" remains the default state.

## ADR-044 — Dark pages are the user's switch, and the UI does not lie about the theme (2026-11-18)

**Context:** the user reported that "the dark modes do not work", in an amoled-themed window
(which their picture proves: the control bar and the status bar are `#0A0A0A`), because the **Wikipedia article stayed white**. The
two are not the same thing: ADR-037 deliberately does not repaint web pages, it only *tells* them
(`prefers-color-scheme`), and the article has no dark theme of its own. So the UI applied its own themes correctly,
but it did not darken the **page** — and the user saw this as a "non-working dark mode".

**Decision:** three independent things, each measured:

1. **The UI themes** (`light`/`grey`/`dark`/`amoled`) paint the **product's** UI: the window, the
   control bar and the **grid** too. The grid's own `data-theme` attribute was so far a separate switch (in fact it
   even lagged one click behind), from now on the control bar's decision drives it, on every theme change and every
   grid load. `--themetest` measures this **pixel by pixel**: the control bar's average brightness is 241 / 60 / 42 /
   15 in the four themes, the grid's image 239 → 68 (light → amoled).
2. **The "Page theme: auto/dark/light"** keeps its ADR-037 meaning: the browser tells the page
   what the system wants; the page decides. This does not darken.
3. **New: a "🌙 Dark pages" switch** (**off** by default, remembered): the engine injects a **named**
   style element into the page (`set_forced_dark`) that inverts the document and re-inverts the images — and
   restores it by removing the same element, without a reload. Measured: a white test page's brightness
   251 → 13.

**Why not Chromium's own "auto dark" feature:** it is a process-level switch (`WebContentsForceDark`),
which affects every page, and its behavior changes with the Chromium version. The style element is page-level, exactly
revocable, and the user's decision — this fits the spirit of ADR-037 (the page stays the page).

**Consequence:** (+) the user gets dark pages with one click, and the next startup knows it too;
(+) the grid and the control bar never contradict each other; (−) the inversion is not perfect: the color world of images/videos
stays approximate, but gradient graphics and shadows can differ from the original design —
that is why it is **off by default**, and not part of the "Page theme" button but a separate switch; (−) the setting is in the user
profile (`QSettings`, `ui/forced_dark`), so it is not yet on at first startup.

## ADR-043 — The tray icon is the launcher and the running indicator: one user, one Antikolt (2026-11-18)

**Context:** the program is launched by the portable folder and the shortcut, but the close button **exited**, and
there was no indication whether the browser was running; opening the shortcut a second time launched a *second*
Antikolt — with a second engine, a second profile, a second ~150 MB on the same card store. The
user request was literally this: "the launcher icon to the tray, which is also the running icon".

**Decision:** the tray icon is the **same** image as the shortcut icon, and it lives as long as the process lives:

1. **One user, one Antikolt.** Before starting, the shell claims a named local socket
   (`antikolt-shell-<user>`) (`QLocalServer`). If someone already answers it, this launch
   asks the running window to come forward, and **exits without a window, with 0** — it does not start an engine, profile,
   process. The socket name differs per user, so two accounts on the machine can run two Antikolts.
2. **The icon in the tray** (`QSystemTrayIcon`) with the program icon and the "Antikolt — running" tip: this is the
   running indicator. On click (or double) it toggles the window; its menu is "Open" and "Exit".
3. **The close button is not an exit**: it puts the window into the tray, and explains once where it is (balloon).
   The only real exit is the tray menu's "Exit" item (or stopping the process).
4. **Without a tray** (a trayless session, or `ANTIKOLT_NO_TRAY`) everything is as before: closing exits.
   Every self-test runs in this mode, only `--traytest` turns the tray on.

**Why a lingering background process does not start the browser:** that would be contrary to the promise about memory
(the "launcher" is a second, always-running program), and the Windows tray has no event that would address a
non-running program. The shortcut + guard combination gives the same user experience, without a
process.

**Consequence:** (+) closing does not take the browser away, the user returns to the same place; (+) the
shortcut does not accidentally start a second browser, so two processes do not write the card store; (+) the icon
and the shortcut are the same image, so the tray shows that it is running; (−) a window put into the tray **does not give back
memory**: the engine and the grid live (~150 MB, measured value) until the user presses "Exit" — but an open
card hibernates on its own under the usual 10-minute rule, so the browser waiting in the tray also releases
the card's memory; (−) a single socket name guards within one machine, at one time (a race of two launches
in an ms window could theoretically both be primary — in practice the Windows pipe reservation catches
this).

## ADR-042 — The engine host holds two surfaces, and every command names its own (2026-11-17)

**Context:** ADR-041's worker surface collided with the engine host's previous assumption: `RunEngineHostServer`
held a single `HostState` and executed every command on it — the `surface` field was checked by the wiring,
but the engine ignored it. A second surface therefore did not fit, and a command addressed to a wrong
(non-existent) surface silently served the grid.

**Decision:** the engine host holds **at most two** surfaces (grid + worker), with separate state per surface,
and every surface-related command acts on the **named** surface:

1. `create_surface` supplies the surface's identifier; the states live keyed by the surface identifier, the
   `bridge_reply` looks up the request in the *owner* surface's state.
2. A command addressed to an unknown surface is `surface_not_found` ("no live surface") — it does not fall back to the grid.
3. The engine refuses a third surface with `engine_busy` ("at most two surfaces at a time"), so the
   limit is also enforced in the engine, not just in the shell's intent.
4. The `hidden` flag is per-surface (`create_surface { …, hidden }`), with the `--hidden` process switch
   and without it too; with this the worker does not appear before the user.
5. `WM_CLOSE` per window closes only its own surface (window → surface registration).

**Measurement (memory, instead of the "4 MB"):** over the worker's lifetime the engine-host process tree
was **9 processes / 167 MB** compared to the **8 processes / 143 MB** baseline in the same run
(`--previewworkertest`, 150 ms sampling) — i.e. the second surface is **a separate renderer process,
~24–25 MB**. So ADR-041's point 5 promise of 4 MB was wrong; the real bound is the short lifetime
(≤ 8 s), one worker at a time, and standing on the grid.

**Consequence:** (+) the worker's surface runs on the same engine, so there is no second engine/Cache; (+)
every test that so far built on a command "silently falling onto the grid" now **fails honestly** — during the
slice ten such test errors surfaced (dangling `id` references in deferred lambdas,
stale surface identifiers after a hibernated grid), and each was fixed; (−) the engine's state management
is one level more complex (surface registration, per-surface teardown), and the shell must guarantee
so that when a card opens, the worker *first* stops before the new surface's command goes out.

## ADR-041 — The background preview gets a short-lived second surface (2026-11-17)

**Context:** since slice 28, the card preview is only produced when the user opens the
card. The grid therefore shows empty frames until every card has been opened once. The
`nextPreviewCard` already decides *which* card should get an image, but the **execution** has so far hit the
**I1** invariant: "one live surface at a time" — the grid is also a surface.

**Decision:** the background preview gets a **temporary worker surface**, with strict limits:

1. **Starts only** when there is no live card (the user is on the grid), no other worker, and the daily budget
   allows it (`previewBudgetAllows`).
2. **One worker at a time**, with its own partition (`persist:preview`), so it does not mix with a card's
   cookies.
3. **8-second ceiling** for loading and capture; after that the surface stops and the card is skipped.
4. **Stops immediately** when the user opens a card (the worker may not compete with the user).
5. **The extra memory is bounded by the short lifetime**, not a number: measurements show the worker surface is a
   **separate renderer process** (~25 MB on the engine-host process tree; see ADR-042). The original "at most
   4 MB" promise was, by measurement, **wrong**, and instead the limit is: one worker at a time, only while
   on the grid, for at most 8 seconds.

**Why reusing the grid surface is not an alternative:** it would destroy the grid (the user
would return to an empty screen), and would violate the meaning of "home".

**Consequence:** (+) the grid previews fill themselves in; (+) the user's card never slows down
because of it (point 5: immediate interruption); (−) I1 **strictly** holds, but with one documented, short, and one-at-a-time
exception — this must also be enforced in code (state machine: `idle → loading → capturing → idle`), and
proven with a self-test.

**Status:** **implemented** (2026-11-17). The worker lives in `MainWindow` (`previews.h` guards it), the engine host
keeps two surfaces (ADR-042), and `--previewworkertest` measures: interruption at the ceiling, successful capture,
interruption on card open, the surface disappearing, and the frame. The memory promise is corrected in ADR-042.

## ADR-040 — Signing: Store + SignPath, two channels (2026-11-17)

**Context:** the release today is a portable folder + desktop shortcut (`scripts/dist.ps1`), unsigned.
SmartScreen warns about the unsigned exe, which is unacceptable to the end user for a browser.
The free paths in 2026: **Microsoft Store MSIX** (Microsoft re-signs the package, the developer account
is free for individuals) and **SignPath Foundation** (free OV-level signing for OSS projects, the key in their
HSM). Azure Artifact Signing (formerly Trusted Signing, ~10 USD/month) is available to individual developers only
in the USA/Canada, and an EV certificate has not bypassed SmartScreen since 2024 — the 400+ USD/year
premium is therefore not justified.

**Decision:** **two channels, not a choice.**

1. **Own download (this is the product today):** the exes in the `dist\Antikolt` folder are signed by **SignPath** — a free OSS
   program, the condition being a public repo and CI build. Application is due once there is a reproducible
   release; until then a self-signed certificate is only for testing valid on one's own machine (per Microsoft's
   comparison, this **blocks** installation on other machines).
2. **Installed from the Store:** **MSIX + Microsoft Store**, where signing is done by Microsoft (free, no
   certificate). This package is installed from the Store; it does not sign our portable folder.

**Why one is not enough:** the Store does not sign the exe downloaded from our own site (the portable folder and the
shortcut), and SignPath does nothing to the MSIX installed from the Store. The two cover **different
artifacts**, so both are needed for full coverage.

**Consequence:** (+) our own release will also be signed, with no certificate purchase; (+) the Store path is free and
warning-free for Store users; (+) the certificate never reaches the developer's machine (no
leaking key); (−) SignPath signing goes through their process (CI integration + review), and the Store path
brings MSIX packaging and Store rules; (−) Linux DEB signing in the free tier is **not guaranteed**
(RPM yes, DEB only with the "Semantic Code Signing" designation) — this must be confirmed with SignPath.

**Details and steps:** `20-ALASAS.md`. The tool in the repo: `scripts/sign.ps1` (PFX / thumbprint /
self-signed mode, and it **rejects without a certificate**).

## ADR-039 — The session rules are set by the user, checked by the core (2026-11-17)

**Context.** The 24-hour policy was until now only *displayable*: the panel showed the rules, but
they could not be changed (the ledger sent `rules` in the `session_list` response, with no write
for it). And a security promise that "cannot be set" is not a promise.

**Decision.** New wire op: `session_rules { rules }` → `{ rules }`, with the expiration given in hours on the panel
(1–720 hours), a "delete on app close" flag and a **Save** button. The **validation is in the core**
(`SessionStore::set_rules`): the TTL must be a real retention window (1 hour – 30 days), because a
zero TTL would immediately sweep every session, and a value beyond 30 days is a typo, not a policy. The
rejection has its own error variant (`SessionError::Rules`), so the response says what is wrong — it does not borrow a
row-level error.

**Consequence.** (+) the user can really set the policy, and the core runs the same `plan_cleanup`
as before; (+) the rules stay in `sessions.json`, so they survive a restart;
(−) the TTL is given in hours by the UI, so non-whole-hour values are not available (a deliberate
simplification); (−) per-panel (per-card) override still does not exist, only the container-level one.
## ADR-038 — Scroll and zoom: on the document, and by reflow (2026-11-17)

**Context.** The wire contract has long advertised the `scroll` and `set_zoom` ops, while the engine answered `not_supported`.
Each has at least two implementations, and the wrong choice apparently works but actually
does something else.

**Decision.** `scroll` is **`window.scrollBy`**: deterministic, needs no pointer position, and is exactly what
is needed to restore the scroll position. It does not scroll an inner overflow container — that would be gesture-based
scrolling (`Input.synthesizeScrollGesture`), which is a *different*, later feature. `set_zoom` is
**`Emulation.setDeviceMetricsOverride`**: the CSS viewport shrinks, the text reflows (like in a
browser), the image is not magnified. The scale **multiplies** by the screen's own DPR (which we first
measure with DevTools), so 150 % on a 125 % display gives 1.875 DPR, not 1.5 — an absolute factor
would silently shrink on HiDPI. 100 % clears the override.

**Consequence.** (+) scroll and zoom are measurable on the page's own `scrollY`/`devicePixelRatio` values; (+)
HiDPI is correct; (−) scroll moves the document, not the focused inner container; (−) zoom
overrides the page's size, so the page receives the `resize` events (this is correct, but the shell must not
write it back with `set_bounds`).
## ADR-037 — The page theme is decided by the page, the browser only signals it (`set_media`) (2026-11-17)

**Context.** The dark page theme (F7) can be imagined two ways: the browser **forces** the colors
on every page (inversion, `forced-colors`), or **signals** to the page that the user wants dark, and the
page's own CSS decides. The first is violent: it ruins images, brand colors, and contrast, and the
user cannot undo it per page. The second is what the platform also offers
(`prefers-color-scheme`).

**Decision.** The shell, through the engine's `set_media` op, sets via `Emulation.setEmulatedMedia` the
`prefers-color-scheme` (`auto` = no override, `dark`, `light`), and **remembers** the choice: on every
surface attach and load it re-applies it, because the emulation belongs to the page that
is currently visible. A page that does not support the query **stays unchanged** — this is intentional: the
forced colors would be a separate, riskier feature (and not this op).

**Consequence.** (+) the user gets a dark page with one click where the page supports it, and
can switch back; (+) the page's own brand and images stay intact; (+) the setting is surface- and
navigation-independent (we re-apply it); (−) pages that do not know the query do not
change — the UI does not promise this; (−) the per-page exception ("this page stays light") does not yet exist.

## ADR-036 — The memory figure is measured by the engine, the page gets it on the bridge request channel (2026-11-17)

**Context.** The product promises a "memory-saving" browser, but the UI showed nothing about
consumption: the page's `system.memory` call got zero from the mock. Three wrong paths: the shell guesses
(not a measurement), the page computes (it cannot see the engine's process, and the page's data is unreliable), or the
status bar shows nothing (which was the situation).

**Decision.** The **engine** measures what is measurable: a `metrics` op that uses `Runtime.evaluate` to fetch the page's own
node count (`document.getElementsByTagName('*').length`) and the `performance.memory` JS heap, and
uses `GetProcessMemoryInfo` for its **own working set**, and answers `{dom_nodes, js_heap_bytes,
engine_working_set_bytes}`. The shell forwards this on the page's `system.memory` **request**
as `{usedBytes, budgetBytes}`; `budgetBytes` is 0 until the profiles carry a memory budget (F6),
and in that case the page only prints the used value — it does not invent a budget.

**Consequence.** (+) the UI shows a real, measured figure, and the page gets it over the bidirectional bridge
(the same channel as the card queries); (+) the measurement lives in the engine, because only it sees the document
and its own process; (−) the JS heap value is **quantized**: Chromium rounds it for privacy reasons
(exactly `10 000 000`), so it is an indicator, not accounting data — the log states this, and the UI
shows it rounded, in MB; (−) the budget and the per-card breakdown (F6) remain open.

## ADR-035 — The card preview comes from the engine, on the `antikolt://blob/` scheme, not from the network (2026-11-17)

**Context.** The grid needs a preview, and the page already references it (`antikolt://blob/<ref>`). Two wrong
paths: the page loads it from the network (the site may be slow, the image goes stale, the card's data may leak to the
remote server), or the image travels in the database (the card list size and the protocol cost
explode). The right answer: the preview is a **local file** served from the profile by the *engine*, and the page
references it with a stable name.

**Decision.** The engine registers the `antikolt` scheme (`STANDARD | SECURE | CORS_ENABLED |
FETCH_ENABLED`), and answers the `blob/<név>` request from the file `<user data>/previews/<név>`
(`CefStreamResourceHandler`, MIME from the extension). It **resolves the name within the directory**: empty name,
`..` or a separator → rejection; nonexistent file → 404. The file is **produced by the shell** (the `capture` op's
output), so the preview is not a download but a production.

**Consequence.** (+) the grid preview is local, fast, and referenceable toward the page; (+) a card cannot read out
an arbitrary file through the browser (name resolution stays in the directory); (+) the
card list stays small (a name, not an image); (−) the **production** of the preview is not done yet: a card
must be opened in the background, an image made with `capture` and written to the profile (the same I1 constraint as in
cleanup), and the card record will need an additive `preview_ref` field.

## ADR-034 — The page can ask: the bridge is bidirectional, and this is protocol v2 (2026-11-17)

**Context.** ADR-032 fixed a one-way bridge because the page could not ask: the shell pushed, the
page could only signal (`open_card`, `freeze_card`). This is enough to fill the grid, but not enough for
the page to get an **answer** from the core's data — search, pin, note —, and the "page keeps its own copy"
state means, long term, two truths.

**Decision.** The page's `window.antikoltNative.request(method, args)` call is a **persistent `cefQuery`**: the
engine keeps the callback in hand, forwards the request as a `bridge_call` event with a
**request number**, the shell answers with `bridge_reply` (`json` or `error`), and with that the page's promise
settles. The one-way calls (`open_card`, `freeze_card`) are unchanged — the engine answers `{}` immediately.
This is **protocol v2**: a new `Message::BridgeReply` variant, `PROTOCOL_VERSION` 1 → 2, and the
`bridge_reply` op in `proto/README.md`. The version is **checked by both sides**, so a v1 shell and
a v2 engine do not "half-work" but reject each other.

**Consequence.** (+) the page gets an answer from the core's data, not from its own copy; (+) every request
gets exactly one answer — an unknown method also gets a rejection, not silence (a dangling promise is the most expensive
bug on a page); (+) the bridge still lives only on the surfaces the shell allowed, and
only in the main world; (−) the lifetime of the persistent queries must be managed by the engine (when the page disappears
the callbacks are dropped), and because of the version step the shell and engine must ship together; (−) the contract of the
methods the page can ask is now `proto/README.md`, not `home/app.js`.

## ADR-033 — The session policy is evaluated by the core, the shell only executes (2026-11-17)

**Context.** The 24-hour policy (`keep`/`sweep`/`wipe`) decides when an origin's storage is
deleted, and `wipe` **logs out**. If the shell computed this, then (a) the decision would be tied to a window's
lifetime, (b) the same rule would live in two places, and (c) the panel and the cleanup
could diverge. Moreover, `plan_cleanup` builds on the disk-stored `last_seen_ms` and the
container rules, which are the core's data.

**Decision.** The policy's **evaluation happens in the core**: `antikolt-core::session` holds the rule
(`SessionRules`, `plan_cleanup`, `CleanupPlan`), the core service's `session_due` op returns the
**rows to execute together with their plan**. The shell only executes (`clear_origin_data` from the engine)
and displays. The ledger lives in `sessions.json`, in a file separate from the card store —
so a faulty session row cannot end up in the card list.

**Consequence.** (+) the rule lives in one place, the panel and the cleanup see the same evaluation;
(+) the plan and the decision travel together, so the wrong storage set cannot be deleted; (−) the shell
talks back and forth with the core in every cleanup round (this is intentional: the decision is not born in the shell);
(−) the `clear_origin_data` engine op is still `not_supported`, so executing the plan is the next slice.

## ADR-032 — The card list is pushed from the shell to the page while the bridge is one-way (2026-11-17)

**Context.** The grid must show the core's cards. But the page today **cannot ask**: the bridge
(`window.antikoltNative`) is one-way, the `cefQuery` answer path is not wired. For a real request-response
round, a persistent `CefMessageRouterBrowserSide::Callback` is needed on the engine side (the engine must hold
the call until the core answers), and on the wire protocol a new message (`bridge_result`) — which
per the contract **raises the protocol version** (v1 → v2), and must be carried through the engine, the shell, and the
core.

**Decision.** The card list is, for now, a **shell-initiated push**: after the home page loads, the shell
asks the core for `cards_list`, and passes it to the page **as data**
(`window.antikoltAcceptCards(JSON.parse("…"))`, the JSON is an escaped string literal in the shell, not
injected code). The page converts the core's records into its own model with the `pageCardFromCore` adapter, the
search runs on the page over the loaded list, and `window.antikoltGridState()` is the page's own witness of what it
shows. If there is no core, the page shows its sample cards, and the status bar states why.

**Consequence.** (+) the grid now shows real data, with zero protocol change; (+) because of the page-side
witness (`antikoltGridState`), the measurement does not rely on the shell's bookkeeping or a screenshot;
(−) the page cannot refresh/ask (a new card, reordering only shows on reload); the real
bidirectional bridge comes with protocol v2, and then this push is demoted to an event ("the list
changed"). Details: `16-FEJLESZTESI-NAPLO.md`, sixteenth slice.

## ADR-031 — The vault's boundary behaves closed: no encryption, no secret (2026-11-17)

**Context.** Per the ordering (ADR-027), the vault is the next slice. The encryption per ADR-010
(per record AES-256-GCM, AAD = origin + record id, DEK in the system key store or with Argon2id)
requires crates: `aes-gcm`, `argon2`, `zeroize`. In this build environment there is **no crates.io access**
(`curl: (35) schannel: AcquireCredentialsHandle failed: SEC_E_NO_CREDENTIALS`), the offline registry
contains **14 crates**, none cryptographic; the `windows`/`winapi` crate is also unavailable, and the crate
uses `#![deny(unsafe_code)]`, so the platform key store (DPAPI) cannot be called via FFI either.

**Decision.** The vault's **shape** is built now (lock/unlock, 5-minute idle auto-lock, `require_open` gate,
`SecretKind`, `VaultStatus`), and encryption goes behind an **explicit seam** (`SecretBox` trait),
which has a single implementation in the tree: `UnavailableSecrets`, which **refuses every seal and
unseal** (`vault_locked` / `not_supported`). `VaultStatus` states that in this build
`can_store: false`, and gives the reason. **There is no silent plaintext fallback** — this is ADR-010's one
unbreakable rule — and there is no hand-written AES.

**Consequence.** (+) the shell, the session ledger, and the passkey flow can now build on this shape;
(+) real cryptography will be a swap of one implementation, the API does not change; (+) the 10 new tests pin the
behavior (4:59 open, 5:00 locked, a clock stepping backward does not extend, the seam never returns
plaintext); (−) the vault **cannot store a secret** until the crates are vendored —
the UI must state this, not hide it. Details: `16-FEJLESZTESI-NAPLO.md`, fifteenth
slice.

## ADR-029 — Every DevTools call goes through one type (2026-11-17)

**Context.** CEF's DevTools transport is asymmetric: `ExecuteDevToolsMethod` only **starts** a
call, the answer arrives later in `OnDevToolsMethodResult`, and the caller only knows from the message id
which answer is its own. The first two consumers (MHTML snapshot, JS evaluation) therefore got two separate,
almost identical observer classes. `capture` and passkey (ADR-027) need the same three:
unique id, parameter dictionary, response processor.

**Decision.** A single `DevToolsCall` type (`apps/engine-host/src/devtools.{h,cpp}`): method name +
parameters + response callback. **Sequences are chained by the response callbacks** (`Page.enable` →
`Page.captureSnapshot` → `Runtime.evaluate`), not a state machine. The answer is parsed **once**, in one place
(`DevToolsReply`), and this type **owns the parsed JSON document**: CEF's internal
dictionaries live in the document, and if the document is freed, the dictionary comes back **empty** — it does not
error, but lies (empty `value`, `state_bytes=0`). This bug surfaced twice (once in the direct
observer, once when this type took the document's lifetime away from the caller).

**Consequence.** (+) the answer parse, id assignment, and observer lifetime are decided in one place;
(+) `capture` and the WebAuthn calls get the same for free; (+) the `eval` and snapshot
`state.json` works again. (−) the answer's JSON stays in memory until the processor runs (a few KB;
for the snapshot, after base64 decoding the caller no longer references it). Details:
`16-FEJLESZTESI-NAPLO.md`, fourteenth slice.

## ADR-030 — Paint is measured with the engine's own image, not from the screen (2026-11-17)

**Context.** The earlier paint check made a copy from the screen (`QScreen::grabWindow`), and
the same binary on the same code measured sometimes 25 colors, sometimes 1 color (thirteenth slice). The screenshot measures the
**desktop**, not the engine: a covered, minimized, or otherwise-composited window
gives a single-color image, while the page rendered flawlessly.

**Decision.** The measure of paint is the engine's own image: CDP `Page.captureScreenshot` through the `capture` protocol op,
in PNG, with the real image size written alongside the file. The screenshot remains only **diagnostics** (it does not
decide a transition), and `capture` is the same path that also provides the grid **previews** — not a test helper.

**Consequence.** (+) deterministic measurement (the compositor's pixels, the page's own viewport as the independent
witness); (+) the preview pipeline builds on the same primitive; (−) the image of a covered/standalone engine window may be empty,
so preview production must give it a visible surface (measured observation: a standalone, covered
window gives 1 color, an embedded card gives 25). The measurement lives in the `--capturetest` and `--embedtest` self-
check.

## ADR-027 — The passkey is part of v1.0, not v1.1 (2026-11-17)

**Context.** The user has registered passkeys (Ügyfélkapu also builds on this), and
stated: the passkey matters. The earlier ordering (v1.1 software passkey) would have led to a passkey-less v1.0,
which for the user is not a "good enough browser", but one they cannot
log into their own accounts with.

**Decision.** The **software passkey is v1.0 scope**, per path "B" of `12-PASSKEY.md`:
CEF + CDP `WebAuthn.enable` / `WebAuthn.addVirtualAuthenticator` (CTAP2, platform authenticator,
`automaticPresenceSimulation`), the credentials stored in the **vault**
(`WebAuthn.getCredentials` / `setCredential`), bound to the vault unlock. Hardware keys (FIDO2 USB)
remain later: CEF has no built-in WebAuthn, it must be provided over the CDP path.

**Consequence.** The `capture`/DevTools layer gets a new consumer, and the vault (the ADR about the encrypted
store) **cannot be pushed** after the passkey — the two must be in one slice. The order is therefore:
DevTools infrastructure → vault → passkey → autofill/TOTP.

## ADR-028 — The disk quota is adjustable, default 50 GB (2026-11-17)

**Context.** The earlier answer: "rather 50 GB, but overridable". The user has now clarified:
**let it be adjustable**.

**Decision.** The quota is a **user setting** (per device), default 50 GB, upper bound the free
disk space minus a safety margin; the setting lives in the core (not the shell), because the core decides about
hibernation. Exceeding the quota is not an error but an **eviction rule**: the least-recently-opened
cards are deleted first (LRU), the pinned ones last.

**Consequence.** The `03-MEMORIA.md` budget and the `session`/`snapshot` records get a
`quota_bytes` setting; the shell only displays it.

| # | Decision | Status |
|---|---|---|
| ADR-001 | A single live web view | Accepted |
| ADR-002 | Native Rust core + Qt 6 shell + CEF engine (not Electron) | Accepted |
| ADR-003 | Disk hibernation MHTML + meta (not screenshot+scrolling) | Accepted |
| ADR-004 | A separate engine-host process (T1/T2 modes) | Accepted |
| ADR-005 | Engine adapter with 7 primitives + MessagePack IPC | Accepted |
| ADR-006 | Windows: native window; Linux: OSR | Accepted |
| ADR-007 | SQLite + WAL + two FTS5 indexes | Accepted |
| ADR-008 | Content-addressed blob store with quota | Accepted |
| ADR-009 | 10 minutes / 24 hours, with origin-level deletion, overridable | Accepted |
| ADR-010 | Vault: DEK/KEK + AES-256-GCM + optional Argon2id | Accepted |
| ADR-011 | HTML home page with a narrow bridge API | Accepted |
| ADR-012 | Theme ladder + luminance check + grey mode | Accepted |
| ADR-013 | Preview refresh with the same engine, scheduled | Accepted |
| ADR-014 | License MPL-2.0, Qt with dynamic linking | Accepted |
| ADR-015 | Zero telemetry | Accepted |
| ADR-016 | One window, no extension system (v1) | Accepted |
| ADR-017 | Code and commits in English, documentation HU + EN | Accepted |
| ADR-018 | Desktop browser basics as required parts of v1 | Accepted |
| ADR-019 | Session ledger: measurable keep-alive of logins | Accepted |
| ADR-020 | Resource profiles (Fast/Balanced/Saver/Ultra/Manual) | Accepted |
| ADR-021 | Development is done by an AI agent — with quality-assurance rules | Accepted |
| ADR-022 | Attachment mode: Windows native, Linux OSR; OSR spike in F0 | Accepted |
| ADR-023 | Passkey strategy: v1.0 without it, v1.1 software authenticator in the vault | Accepted |
| ADR-024 | Code signing: SignPath Foundation + Ed25519 update signing | Accepted |
| ADR-025 | Disk quota: 50 GB base, overridable per machine | Accepted |
| ADR-026 | Contribution and security governance (DCO, protected areas, fuzz, SECURITY.md) | Accepted |
| ADR-027 | Passkey in v1.0: CDP virtual authenticator, the credentials in the vault | Accepted |
| ADR-028 | Disk quota: adjustable, default 50 GB, with LRU eviction | Accepted |
| ADR-029 | Every DevTools call on one `DevToolsCall` type; the answer owns the JSON document | Accepted |
| ADR-030 | Paint measurement with the engine's own image (`capture`); the screenshot is only diagnostics | Accepted |
| ADR-031 | Vault: the encryption seam behaves closed (no crate → no secret, no plaintext) | Accepted |
| ADR-032 | The card list reaches the page by shell push while the bridge is one-way (bidirectional bridge = protocol v2) | Accepted |
| ADR-033 | The session policy is evaluated by the core (`session_due`), the shell only executes and displays | Accepted |
| ADR-034 | The page can ask: persistent `cefQuery` + `bridge_reply` = protocol v2 (ADR-032 still applies to one-way calls) | Accepted |
| ADR-035 | The card preview is served by the engine from the profile (`antikolt://blob/`), resolving the name within the directory | Accepted |
| ADR-036 | Memory is measured by the engine (`metrics`), the page gets it on the request channel; the budget is 0 until the profiles carry it | Accepted |
| ADR-037 | The page theme is decided by the page: `set_media` sets `prefers-color-scheme`, does not force color | Accepted |
| ADR-038 | `scroll` = `window.scrollBy`, `set_zoom` = reflowing device-metrics override, multiplied by the DPR | Accepted |
| ADR-039 | The session rules are set by the user (`session_rules`), checked by the core (TTL 1 hour – 30 days) | Accepted |
| ADR-040 | Signing: Store + SignPath, two channels (Store = MSIX from MS, SignPath = our own portable release) | Accepted |
| ADR-041 | The background preview gets a short-lived second surface (8 s, one at a time, immediate interruption) | Implemented |
| ADR-042 | The engine host keeps two surfaces, and every command names its own (surface id, `hidden`, `engine_busy` for the third) | Accepted |
| ADR-043 | The tray icon is the launcher and run indicator: one user, one Antikolt (local socket guard, close to tray, `--traytest`) | Accepted |
| ADR-044 | Dark pages are the user's switch (`set_forced_dark`), and the control bar theme also drives the grid (`--themetest`) | Accepted |
| ADR-045 | Page darkening has four levels (`off`/`invert`/`dim`/`warm`), the theme-switch icon + tooltip | Accepted |
| ADR-046 | The shell log file (`logs/shell.log`, rotated at 1 MB) is the stderr of a console-less start | Accepted |
| ADR-047 | Search is diacritic-independent (page and shell with the same fold), an empty hit names the term, does not promise page text | Accepted |
| ADR-048 | Cleanup does not mute a playing card either: the same media query as at hibernation | Accepted |
| ADR-049 | The page text goes into the card record on freeze (`card_page_text`, max. 16 000 characters), and search sees it in all three layers | Accepted |
| ADR-050 | Container switch on the card, with a native `<select>`; the write goes through the core, the page only shows what the core accepted | Accepted |
| ADR-051 | The container legend is also the filter: chips with a colored dot and the search-based count, with `aria-pressed` | Accepted |
| ADR-052 | The container table lives in the core (`containers.json`, today an SQLite table per ADR-070), arrives on the page by push, and cannot be deleted while a card uses it | Accepted |
| ADR-053 | Container editing in place, without a dialog (`＋`, double click, `×`), the core's answer is shown | Accepted |
| ADR-054 | Deleting a used container via `move_to`: first a move, then deletion; the UI offers it, does not force it | Accepted |
| ADR-055 | The tags live in the card record (`tags`), the core normalizes them (trim, dedup, 12×32), search sees them without diacritics too | Accepted |
| ADR-056 | The workspace table lives in the core (`workspaces.json`, today an SQLite table per ADR-070); the card's `workspace` field is stored data, not the page's index | Accepted |
| ADR-057 | The workspace lifecycle is on the tab strip, with the same gesture as the containers' (`＋`, double click, `×`), without a dialog | Accepted |
| ADR-058 | The card's workspace is switchable on the card, with a native `<select>` (not drag: fragile in the virtualized grid and unusable by keyboard) | Accepted |
| ADR-059 | The profile travels in a portable JSON bundle (`profile_export`/`profile_import`), with a `schema` number; an unknown schema is rejected | Accepted |
| ADR-060 | The bundle arrives over the bridge, the path is given by the caller (`profile.export`/`profile.import`); the bridge opens no dialog | Accepted |
| ADR-061 | The home page is editable: `＋` + inline form, `card_delete` in the core, the new card goes to the active tab and the first container | Accepted |
| ADR-062 | Renaming the card in place (double click, `Enter`/`Esc`), with the full record's `cards.upsert`; the save goes over the page path (`antikoltRenameCard`) | Accepted |
| ADR-063 | The URL is editable on the domain bar (double click), the domain is recomputed from the core's answer | Accepted |
| ADR-064 | SQLite store (`rusqlite` bundled, schema `user_version=2`), the JSON stores stay the truth; `db_status` mirrors and re-verifies | Deprecated (ADR-069) |
| ADR-065 | The grid's list comes from SQLite (write-through: mirroring before the list); JSON stays the writer, the faulty case is loud | Deprecated (ADR-069) |
| ADR-066 | The mirror follows writes row by row (`mirror_card`/`mirror_delete`), the list is now read-only; the multi-card op mirrors separately | Deprecated (ADR-069) |
| ADR-067 | The database returns the cards in the store's order (`ORDER BY last_opened_at_ms DESC, LOWER(title)`), a test protects it | Accepted |
| ADR-068 | Two-step deletion on the card (`×` → `Sure?` → delete within 5 s); measured and green (`delete_armed=ok delete_card=ok add_card_check=ok`) | Accepted |
| ADR-069 | The card store is SQLite-first (`user_version=3`): one writer in `antikolt.sqlite3`, `cards.json` export; one-time import from the pre-switch profile; `db_status` measures the DB ↔ export match row by row | Partially deprecated (point 6: ADR-070) |
| ADR-070 | All four stores are SQLite-first (`user_version=4`): the four JSON exports; per-store import in the `meta` table; the ledger's full row + one-row `session_rules`; `db_status` checks all four exports row by row | Accepted |
| ADR-071 | The container isolation boundary (`user_version=5`): per partition its own `CefRequestContext` and profile directory, per container a proxy (`system`/`direct`/`fixed`) and permission policy (denied by default); the house rules are a database row | Accepted |
| ADR-072 | User preferences (`UserPrefs`) in SQLite (`user_version=6`): a `user_prefs` table with a single row (`CHECK (id = 1)`); `max_live_cards`, `idle_hibernate_ms`, `cold_card_ms` with defaults and validation; IPC `user_prefs`/`user_prefs_set`; wiring of `profile_export`/`profile_import` | Accepted |
| ADR-073 | Login popup: gesture → a real window in the opener's partition (shared cookies), 60 s, tracked separately (`popup_browser`); blocked without a gesture; `popup_requested`/`popup_closed` events; the shell suspends hibernation with the `popup_open` flag | Accepted |
| ADR-074 | Memory budget (F6): `memory_budget_mb` in `user_prefs` (`user_version=7`, default 512, 64..=4096, `serde(default)`); the shell caches it with `refreshUserPrefs`, `system.memory` returns it as `budgetBytes`; the grid draws a used/budget bar; `--metricstest` `page_budget_bytes > 0` | Accepted |
| ADR-075 | `<antikolt-card>` as a standalone package: `home/antikolt-card.js` IIFE, the grid and the user page load the same; the container/workspace `<select>` from `setOptions`, the focus state with `getPendingFocusId()` | Accepted |
| ADR-076 | The `video_max_height` display cap: the engine's `set_video_cap` op with a named style element (`video{max-height:Npx}`), the profile drives it, `manual` removes it; the decode resolution is `not-measurable` (CEF has no per-page limit), the scope is top-level `video` | Accepted |
| ADR-077 | The preview name is the content hash (`previewContentName`, `QCryptographicHash::Sha256` in the shell — the `sha2` crate dependency is gone); dedup from the naming; freshness check looks at the stored `preview_ref` | Accepted |
| ADR-078 | The shell language is switchable at runtime (`ui/language` QSettings, default `hu`); `i18n.h` `uiText` from an HU→EN table; `setLanguage` re-labels the chrome in place; status messages and the grid are the next slices | Accepted |
| ADR-079 | The grid's language also follows the shell (`data-language` attribute + `pushLanguageToGrid` + `window.antikoltSetLanguage`); the grid's own `t()` (`home/app.js` EN map) re-renders without reload; the card's inline labels and the static aria-labels are the next slice | Accepted |
| ADR-080 | The `Auto` profile resolves in the core (`ProfileKind::Auto`, `params()`=None; `resolve_profile` IPC based on `ProfileKind::auto` + `params()`); the shell measures the power source (`GetSystemPowerStatus` + `ANTIKOLT_AC_POWER`) and pressure, and re-asks after every sample; `Fast` does not come from auto | Accepted |
| ADR-081 | Bookmark import eats Netscape HTML (the only export common to Chrome/Firefox/Edge); a tolerant regex parser (`bookmarks.h`), every bookmark is a card, re-import does not duplicate; password import is absent (there is no target vault) | Accepted |

---

## ADR-001 — A single live web view

**Context:** the goal is memory under 1 GB; the traditional tab model keeps a renderer per tab.
**Decision:** in the application at most one web renderer runs at a time (page or home page).
**Consequence:** (+) the largest memory gain; (+) a simple lifecycle. (−) there is no page running
in the background (music, notifications, upload); (−) card switching is always hibernate+restore — this is
compensated by writing to disk and the immediate preview. **A single, documented exception:** the login popup
started by the user (OAuth/SSO) temporarily opens a second renderer, for at most 60 s
(`02-ARCHITEKTURA.md` 8.5.). **A counterexample we consciously accept:** background music playback does not work;
if this is needed later, only a single "pinned media" view may get an exception (new ADR).

## ADR-002 — Native Rust core + Qt 6 shell + CEF engine (not Electron)

**Context:** Electron's base cost (~250–350 MB empty, Node + its own renderer) does not fit the
goal; and the multi-engine requirement cannot be met under Electron.
**Decision:** the domain logic in Rust (`antikolt-core`), the shell C++20 + Qt 6 (QML), the engine CEF (Chromium),
with `cxx` FFI between the two. The engine in a separate process, over IPC.
**Consequence:** (+) a base smaller by ~150–250 MB, full engine control, CDP, multi-engine path open;
(−) much more integration work, C++ and CEF knowledge needed; (−) no npm ecosystem.

## ADR-003 — Disk hibernation MHTML + meta (not screenshot + scrolling)

**Context:** the "snapshot + scrolling" restore loses forms, SPA state, and the
loaded content; the user's decision: "keep everything, but not in RAM".
**Decision:** on freeze `Page.captureSnapshot` (MHTML) + meta (scroll, anchor, viewport, media,
sessionStorage) + preview + extracted text goes to disk, compressed, content-addressed.
**Consequence:** (+) the best restore fidelity possible without memory; (+) offline
reading and full-text search for free; (−) disk load (1–40 MB/card) — handled with a quota;
(−) the JS runtime state still cannot be saved (a documented limit).

## ADR-004 — A separate engine-host process (T1/T2)

**Context:** Chromium's base cost exists even with no page; multi-engine attachment also requires a separate
process.
**Decision:** the engine is a runnable `antikolt-engine-host`, over IPC; T1 = kept warm, T2 = fully
stopped (ultra-low mode).
**Consequence:** (+) a 150 MB idle state is achievable; (+) engine swap/crash isolation for free;
(−) cold start 0.6–1.2 s (mitigated with hover-prewarm); (−) more complex IPC and lifecycle.

## ADR-005 — Engine adapter with 7 primitives + MessagePack IPC

**Context:** engine swap-ability is only possible with a narrow, well-defined interface.
**Decision:** `create / navigate / eval / scroll / capture / snapshot / destroy` (+ 3 auxiliary), local
socket, length-prefixed MessagePack, `proto/v1` versioning.
**Consequence:** (+) any engine can be implemented; (+) the protocol is testable without the engine;
(−) engine-specific capabilities must be signaled with flags; (−) the richest features (e.g. extensions)
do not fit the contract — a conscious limit.

## ADR-006 — Windows: native window; Linux: OSR

**Context:** Wayland does not allow reliably embedding a foreign process's window; on Windows
and X11, however, it is routine.
**Decision:** on Windows native window embedding (`SetParent`), on Linux offscreen rendering + own
compositing.
**Consequence:** (+) a good path on both platforms; (−) maintaining two attachment paths; (−) OSR is CPU-heavy,
so on Linux the media-quality limit kicks in sooner.

## ADR-007 — SQLite + WAL + two FTS5 indexes

**Context:** structured metadata + full-text search + local, network-free operation.
**Decision:** one SQLite database (WAL, `synchronous=NORMAL`), `unicode61 remove_diacritics 2` for word search,
`trigram` for substring search.
**Consequence:** (+) one file, transactions, mature tooling; (+) Hungarian diacritics handled; (−) two indexes
mean double space and write cost; (−) very large text corpora must be limited (256 KB/card).

## ADR-008 — Content-addressed blob store with quota

**Context:** snapshots are large and can repeat; the decision: possibly tens of GB of disk.
**Decision:** blobs named by `sha256`, with `zstd` compression; global quota 50 GB, 100 MB per card,
LRU + archive-based garbage collection; atomic write.
**Consequence:** (+) deduplication, integrity check, robust backup; (−) the quota and GC
require code (and a user interface).

## ADR-009 — Session policy: `keep` / `sweep` / `wipe`

**Context:** the user's decision was originally "full deletion after 24 hours = logout", but the
requirement's clarification is that **important logins must be kept alive** (Google, Facebook, own
site), preferably with low memory use, at a measurable cost. The login (cookie, token) is in the engine's **disk**
partition, so keeping it alive costs **0 RAM** while the card is hibernated.
**Decision:** the 24 hours do not delete the login but the by-products; a three-level policy
(per card/per container):
- **`keep`** — deletes nothing; the login stays (important services);
- **`sweep`** *(container-based)* — deletes cache, `sessionStorage`, Service Worker, code cache;
- **`wipe`** — full origin deletion = logout (bank, healthcare, work).
The deletion is origin-level, the snapshot is never deleted, and executing it needs the engine (CDP
`Storage.clearDataForOrigin`), so it runs with a `cleanup_pending` flag, at the engine's next available moment.
**Consequence:** (+) logins stay, the RAM cost is measurable and ~0; (+) `wipe` remains
available for sensitive accounts; (−) the "my login disappeared" experience will not be the default, but the
user must understand the lock indicator; (−) because of Service Worker hibernation, background push
does not work (a documented loss).

## ADR-010 — Vault: DEK/KEK + AES-256-GCM + optional Argon2id

**Context:** DPAPI alone is bound to the user profile, not portable, no key rotation.
**Decision:** a random DEK encrypts the records (AES-256-GCM, per-record nonce, AAD = origin+id); the DEK
has two wrappings: system key store (DPAPI / Secret Service) and/or master password (Argon2id).
**Consequence:** (+) portable, rotatable, strong; (−) losing the master-password mode = data loss
(hence an explicit warning and recovery code); (−) own cryptographic code (built on system libraries).

## ADR-011 — HTML home page with a narrow bridge API

**Context:** the "can look any way" requirement can only be met with HTML/CSS/JS; but the home page must not
get native access.
**Decision:** a local HTML home page (`antikolt://home`), cards as `<antikolt-card>` components, strict CSP,
allowlist bridge (`cards.*`, `search`, `workspaces.*`, `events.on`), sanitization, versioned editor.
**Consequence:** (+) endless customizability; (+) the user can write their own page; (−) the home page occupies a
renderer (hence virtualized, light); (−) the user's own code can be faulty → fallback to a built-in
template.

## ADR-012 — Theme ladder + luminance check + grey mode

**Context:** automating dark mode is unreliable; the user's decision: predefined
techniques, and grey mode should be the base.
**Decision:** a six-step technique ladder (T0–T6), after each step a check based on the screenshot's average brightness,
the result stored per page (`site_recipe`); four UI themes (light/grey/dark/amoled).
**Consequence:** (+) measurable, self-checking, learns per page; (−) an image and extra work for every step;
(−) the generated stylesheet engine needs maintenance (adapted from an MIT-licensed base).

## ADR-013 — Preview refresh with the same engine, scheduled

**Context:** a "current preview" requires a live load, which contradicts the "no background engine" principle.
**Decision:** the preview refresh uses the single engine host, in idle time, one card at a time,
with an 8 s time limit, a daily/weekly limit, switchable off.
**Consequence:** (+) no second renderer; (+) a fresh grid; (−) sometimes the engine "wakes up" idly
(hence only on the home page, idly, with an indicator).

## ADR-014 — License MPL-2.0, Qt with dynamic linking

**Context:** an open-source browser; CEF is BSD, Qt is LGPL-3.
**Decision:** MPL-2.0 (file-level copyleft); Qt is dynamically linked, replaceable in the installer.
**Obligation:** Qt's modified files must be published; the user must be given the possibility to recompile and replace Qt
(on Linux this is natural, on Windows it must be documented).
**Consequence:** (+) shareable, but not infectious; (−) the legal/installation requirements of Qt replacement must be
met. **Details, obligation list, and handling of AI authorship:** `13-LICENC.md`.

## ADR-015 — Zero telemetry

**Context:** a data-conscious identity, GDPR simplicity, user trust.
**Decision:** no automatic network reporting; on crash a local log that the user
can send manually.
**Consequence:** (+) trust, simple compliance; (−) harder to discover bugs — hence good local
diagnostics and an easy "bug package export" are needed.

## ADR-016 — One window, no extension system (v1)

**Context:** every further renderer/window/extension works against the 1 GB goal.
**Decision:** in v1 one window and one live view; no extension API; instead user scripts,
rules, and page recipes.
**Consequence:** (+) the memory and security model is simple; (−) some users will miss
their extensions — this is a conscious trade.

## ADR-017 — Code and commits in English, documentation HU + EN

**Decision:** source code, identifiers, commit messages, API documentation in English; the plan and user
documentation in Hungarian (authoritative) and English (translation).
**Consequence:** (+) international contribution is possible; (−) the documentation must be maintained in two languages
(the translation always lags one step — this is accepted).

## ADR-018 — Desktop browser basics as required parts of v1

**Context:** the Gemini plan did not include these; without them the browser cannot be used as a daily tool.
**Decision:** download manager, find-in-page, zoom, PDF, PiP, printing, popup handling, crash
recovery, keyboard navigation and accessibility, import/export, search-engine choice are part of
v1 (F7).
**Consequence:** (+) a genuinely usable product; (−) F7 will be longer, and the beta only comes after.

## ADR-019 — Session ledger: measurable keep-alive of logins

**Context:** the user refined the requirement: not "automatic login everywhere" is needed,
but **keeping important logins alive with low memory use**, and **let what we lose be measurable**.
**Decision:** a record for every `container × origin` pair (`session` table): state, cookie count/size,
storage size, number of Service Workers, last verified login, measured RAM (if running), policy, and the
"if I delete it, you lose this" list. The UI's **Sessions** panel shows this; the cost stated plainly:
a dormant login ≈ 0 RAM.
**Consequence:** (+) "what we lose" is data, not marketing; (+) the user sees why so much
goes to RAM (the running pages, not the logins); (−) a new table and panel, plus the `state` heuristic
(an `unknown` state also exists). Details: `10-MUNKAMENET.md`.

## ADR-020 — Resource profiles

**Context:** the user's request: "let there be profiles, the live test decides anyway", and the machine is configurable.
**Decision:** five profiles (Fast / Balanced / Saver / Ultra / Manual) package the engine lifetime,
hibernation time, preview refresh, media quality, GPU choice, and budget bands; automatic switching
(outlet ↔ battery, memory pressure) with a pinnable override; a remembered per-machine configuration.
**Consequence:** (+) the same product is usable on a weaker machine; (+) measurement decides, not belief;
(−) more configuration UI and more test cases. Details: `09-CELHARDVER.md`.

## ADR-021 — Development is done by an AI agent, with quality-assurance rules

**Context:** the project owner does not develop; the code is written by an AI agent. This is fast, but means heightened
need for checking, and raises authorship/legal questions.
**Decision:** development proceeds with an AI agent, but:
1. at the end of every phase **measurement** (memory, time) and a **test** decide, not the "it's done" claim;
2. the critical layers (vault, IPC, database migration, snapshot integrity) get a **manual review**;
3. the repo **declares** the AI assistance (`NOTICE`, `AUTHORS`, SPDX), and does not claim exclusive human
   authorship;
4. every claim goes into the log with evidence (command output, test, measurement).
**Consequence:** (+) fast progress, documented decisions; (−) human testing (dogfooding) and the
review of critical code cannot be saved; (−) handling legality/provenance is mandatory. Details: `13-LICENC.md`.

## ADR-022 — Finalizing the attachment mode

**Context:** native window embedding is unreliable under Wayland; OSR is portable, but more expensive.
**Decision:** Windows 11 = **native window**; Linux/Wayland = **OSR**; the two behind the same `EngineSurface`
abstraction. A **minimal prototype of the OSR branch is the F0 spike**, so the risk surfaces early.
**Consequence:** (+) a working path on both platforms; (+) the engine's process can be stopped anytime (the image stays);
(−) maintaining two paths; (−) with OSR an accessibility bridge must be built. Details: `11-OSR.md`.

## ADR-023 — Passkey strategy

**Context:** CEF has **no built-in WebAuthn support** (the embedder's `WebAuthenticationDelegate`
is missing), the hardware path is a CEF patch or a separate helper process, which is not realistic for a solo project.
**Decision:** v1.0 = no passkey (password + TOTP), the vault's data model is ready; v1.1 = **software
passkey in the vault**, with a CDP virtual authenticator (key export → vault → reload); the user
presence check is replaced by **vault locking**; CEF patch and a separate helper process are excluded (except,
if it becomes unavoidable because of passkey-only accounts).
**Consequence:** (+) a platform-independent, portable passkey; (−) no hardware protection; (−) the CDP internal
API is version-dependent, so a spike precedes it, and in case of failure path "A" (without passkey) is the fallback.
**Confirmed (2026-11):** the project owner accepted the software compromise; the check of passkey-only accounts
happens per point 2.5. of `12-PASSKEY.md`. Details: `12-PASSKEY.md`.

## ADR-024 — Code signing

**Context:** on Windows, SmartScreen warns about the unsigned installer; since 2024 the EV
certificate does **not** bypass SmartScreen; for an individual, EU developer Azure Artifact Signing is not
available (only to organizations in the EU).
**Decision:** v0.x = unsigned, but with an **Ed25519/minisign-signed update manifest** (fail-closed);
v1.0 = **SignPath Foundation** free signing (OSI-license condition → MPL-2.0 complies) + still
Ed25519 for updates; EV omitted; OV/Azure only if we become an organization.
**Consequence:** (+) zero cost; (+) the updates' integrity is cryptographically verified;
(−) in the first period a SmartScreen warning, and the publisher name will be "SignPath Foundation" — this
must be communicated. Details: `14-KODALAS.md`.
**Clarification:** **there is no paid certificate in the plan** (the project owner's decision). Updates are fully
automated: an Ed25519-signed manifest (fail-closed, two keys, rollback) + Sigstore provenance;
Authenticode with SignPath's free signing, at most one click per release.

## ADR-025 — Disk quota

**Context:** because of disk hibernation and time travel, snapshots can reach tens of GB; the
project owner asked for 50 GB as the default, overridable.
**Decision:** global base quota **50 GB**, 100 MB per card, LRU + archive-based collection; per-machine
overridable (`config/devices/<hostname>.toml`), for example 100 GB on VECTOR.
**Consequence:** (+) predictable space usage; (+) expandable on the target machine; (−) the quota GC needs code and
a UI (the user sees what takes space).

## ADR-026 — Contribution and security governance

**Context:** the project owner's expectation: "anyone may contribute freely, because a browser can have
security risks". A license is not enough for this: security is a matter of process.
**Decision:** **MPL-2.0 + DCO** (no CLA, no permission request); **protected code areas** (vault, IPC,
snapshot parser, migrations, platform/sandbox) with maintainer approval; mandatory automated gates
(test, **fuzz** for untrusted inputs, memory regression, static analysis, secret scanning,
SPDX check); `SECURITY.md` with a private reporting channel and a 30-day fix target; signed
releases + SLSA/Sigstore provenance.
**Consequence:** (+) free contribution and a serious security process at once; (+) the AI assistance
is transparent (NOTICE/AUTHORS/PR template); (−) the protected areas slow progress — this is conscious.
Details: `15-HOZZAJARULAS.md`.
