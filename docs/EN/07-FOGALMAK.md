# 07 — Glossary

The uniform meaning of the terms used in the plan. If a document differs from this, this one is authoritative.
(In parentheses the English equivalent, because the code and commit messages are written in English — ADR-017.)

| Term | Meaning | In English |
|---|---|---|
| **Card** | A saved web page, the unit that appears on the home page. No tab, no page running in the background. | card |
| **Home page** | The browser's starting surface: a local, editable HTML page containing the cards. Its default template is the card grid. | home / start page |
| **Card grid** | The home page's built-in base layout: a virtualized, sectioned card grid. | card grid |
| **Home canvas** | The home page as a freely editable web page (the user can rearrange it any way). | home canvas |
| **Live view** | The single web renderer that is currently running (page or home page). At most one at a time. | live view |
| **Engine** | The browser engine that renders web content (first CEF/Chromium). | engine |
| **Engine host** | The separate process tree running the engine and its executable (`antikolt-engine-host`). | engine host |
| **Engine adapter** | The contract of 7 primitives (`EnginePort`) through which the core reaches the engine. | engine adapter |
| **Attach mode** | How the engine's image gets into our own window: native window embedding or offscreen (OSR). | attach mode |
| **OSR** | Offscreen rendering: the engine renders into frames, the shell composites them. A Wayland-safe path. | offscreen rendering |
| **T1 / T2** | Engine lifecycle: T1 = kept-warm engine host, T2 = fully stopped engine host (ultra-low mode). | warm / killed engine host |
| **Freeze** | Saving the card's state to disk, then terminating the engine. | freeze / hibernate |
| **Disk hibernation** | The result of freezing: all saveable state on disk, zero RAM. | disk hibernation |
| **Snapshot** | A complete save of one freeze: MHTML + meta + preview + extracted text, versioned. | snapshot |
| **MHTML** | The full DOM snapshot with embedded resources; can be opened without a network. | MHTML |
| **Anchor** | The topmost visible element + offset saved alongside the scroll position, needed for exact restoration because of lazy loading. | scroll anchor |
| **Melt-back** | The card's "homecoming": the live view shrinks into the home-page card (animation + engine termination). | melt-back |
| **Blob store** | A content-addressed (`sha256`), compressed (`zstd`) file store for snapshots. | content-addressed blob store |
| **Container** | Identity container: an isolated engine partition for one account (cookies, storage, cache). | container / identity container |
| **Partition** | The engine's own data store per container (`persist:c_<id>`). | profile partition |
| **Origin-wipe** | Execution of the 24-hour rule: deleting the given origin's session data within the partition. | origin-level wipe |
| **Pin** | Marking a card so that it never hibernates automatically (e.g. music, long form). | pin |
| **Session** | The login state of a `container × origin` pair: cookies, storage, Service Workers. | session |
| **Session ledger** | A measurable record of sessions: state, disk, RAM, policy, "what we lose". | session ledger |
| **`keep` / `sweep` / `wipe`** | The three levels of the 24-hour policy: delete nothing / byproducts / everything. | keep / sweep / wipe |
| **Resource profile** | A configuration package (engine lifetime, hibernation, preview, video, GPU): Fast / Balanced / Frugal / Ultra / Manual. | resource profile |
| **OSR compositing** | The engine renders into memory, the shell outputs it to the screen (a Wayland-safe path). | OSR compositing |
| **Software passkey** | The passkey key pair lives in the browser's vault (not in the TPM), using a CDP virtual authenticator. | software passkey |
| **Passkey-only account** | An account where the password was deleted and one can log in **exclusively** with a passkey — it cannot be opened at all with a passkey-less browser. | passkey-only account |
| **DCO (Signed-off-by)** | A one-line commit statement: the contributor is entitled to submit the code; no CLA, no permission request. | Developer Certificate of Origin |
| **Sigstore / provenance** | Free, automated signing + proof that the binary was built from the public source in the public CI. | Sigstore provenance |
| **SignPath Foundation** | Free code signing for OSI-licensed open-source projects (one approval per release). | SignPath Foundation |
| **Recipe** | Settings learned/stored per page: darkening technique, zoom, permissions, form hints, login runbook. | site recipe |
| **Login runbook** | A declarative sequence of steps for automatically logging into a page, with security limits. | login runbook |
| **Vault** | The built-in password / TOTP / passkey store. | vault |
| **DEK / KEK** | Data key (encrypts records) / key-encrypting key (system keychain or master password). | data / key encryption key |
| **Technique ladder** | The six degrees of darkening (T0–T6), with luminance checking, learned per page. | darkening ladder |
| **Luminance check** | Measuring the darkening result from the screenshot's average brightness; if it is not dark enough, the next technique comes. | luminance check |
| **Grey mode** | One of the three base schemes (light / grey / dark), on pages desaturation + slight muting. | grey mode |
| **Ultra-low mode** | T2 + raster home: the browser at ~120–180 MB, the grid appears as an image, becomes live on first interaction. | ultra-low mode |
| **Raster home** | An image of the home page's last state + an interaction map, without an engine. | raster home |
| **Budget controller** | A component that watches the process tree's memory and intervenes according to bands. | memory governor |
| **M0/M1/M2/M0-light** | The states of the memory model: grid with engine / one average page / one heavy page / grid without engine. | memory modes |
| **Bridge API** | The narrow, allowlisted message API between the home page and the core. | bridge API |
| **Preview scheduler** | The preview updater running in idle time with the same engine. | preview scheduler |
| **Phase 0 (F0)** | The timed week: prototype + real memory numbers + decision gate. | phase 0 |
| **Dogfooding** | The project owner uses the product as their own daily browser (from M3). | dogfooding |

## Naming rules in the code

- Crate/file-name prefix: `antikolt-` (e.g. `antikolt-core`, `antikolt-engine-host`).
- Internal scheme name: `antikolt://home`.
- Database tables: singular, `snake_case` (`card`, `snapshot`, `site_recipe`).
- The live view is a **singleton** in the code (`LiveView`), because the I1 invariant requires this.
## The page's bridge: the calls and the test hooks

Between the grid (the `home/` page) and the shell, the CEF `cefQuery` channel carries everything (ADR-004, ADR-034). The page
side has **only** these entry points — there is no other way to reach the engine host:

| Name | Direction | What it does |
|---|---|---|
| `window.antikoltNative.request(method, args)` | page → shell | request; the shell's answer (`{ok, …}` or `{error}`) comes back as a Promise |
| `window.antikoltNative.open(url, title, pinned)` | page → shell | open a card |
| `window.antikoltAcceptCards(list)` | shell → page | passing the core's card list (only the grid receives it) |
| `window.antikoltGridState()` / `window.antikoltNoteState()` | shell → page | **test witnesses**: the state of the page's model, so that a self-test does not guess from the DOM |

**Test hooks (development only):**

- `ANTIKOLT_BRIDGE_TEST=1` — the shell starts the bridge's self-test (`--bridgetest` does the same).
- the `test.hang` method — intentionally **does not answer**, so that the shell's response watchdog can be measured
  (`--timeouttest`: `answer_ms=6540`, `timeout_message=a shell nem válaszolt időben`). On a live page it is not
  called; the shell keeps the call with a request number, and answers once after the watchdog.
- `ANTIKOLT_HIBERNATE_MS` — shortening the hibernation time (`--lifecycletest`).

These hooks run in the **main world** (I5): they never contain a secret, and the page only sees what the
shell intentionally passes over.
