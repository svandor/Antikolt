# 02 — Architecture plan

> This is the authoritative plan. `00-JAVASLATOK.md` is the critique, `03-MEMORIA.md` the measurement plan, the
> `04-UTEMTERV.md` the schedule, `05-DONTESEK.md` the decision log.

## 1. Goals, non-goals, invariants

**Goals**

1. A browser that stays under **1 GB RAM** in daily use, and **~150 MB in card grid view**.
2. **One user, all accounts at once** — containers, vault, TOTP, autofill.
3. **Disk hibernation**: when a card is left, all state is written to disk and the engine ceases to exist.
4. **An editable, webpage-format home page** into which cards can be inserted.
5. **Swappable engines**: the first is Chromium (CEF), the structure is ready for the next ones.

**Non-goals (v1)**

- Browser extension API (Chrome extensions), cloud sync, mobile, multi-window operation,
  built-in cloud AI, Bittorrent/torrent client, e-mail client, VPN service.

**Invariants (these are the fundamental laws of the design — if a feature violates them, it does not get in)**

| # | Invariant | Check |
|---|---|---|
| I1 | At most **one** card renderer runs at a time (a page **or** the home page). Two documented exceptions: the user-initiated **login popup** (OAuth/SSO), which lives for at most 60 s and closes automatically; and the **background preview worker** (ADR-041/ADR-042): a short-lived, hidden surface on the `persist:preview` partition, for at most 8 s, only while on the grid, and ending immediately when a card opens. | measurement bench: card renderer ≤ 1 (+0–1 transient popup, +0–1 worker on the grid); `--previewworkertest` |
| I2 | Every card's state is **on disk** when it is not live. | after hibernation a snapshot belonging to the card exists |
| I3 | A card never consumes RAM when it is not open. | measurement bench: 50 cards, 0 open pages → budget |
| I4 | One window, one live view (v1). **One Antikolt runs** per user: opening the shortcut a second time brings up the running window, it does not start a second one (ADR-043). | code: `LiveView` singleton + `InstanceGuard`; `--traytest` |
| I5 | A secret (password, TOTP) never enters the page's main world, logs, or error reports. | test: page JS cannot see it |
| I6 | No network telemetry. | the network log in the test is empty |
| I7 | The user's data (cookies, snapshots) stays on their machine, encrypted where there is a secret. | audit |

## 2. System topology

```
                    ┌──────────────────────────────┐
                    │      antikolt-shell (Qt 6)    │  ~60-100 MB
                    │  QML UI: card grid,           │
                    │  control bar, vault window,   │
                    │  settings, notifications      │
                    └───────┬──────────────┬────────┘
             FFI (cxx)      │              │  IPC (Unix socket / named pipe)
                    ┌───────▼───────┐   ┌──▼─────────────────────────┐
                    │ antikolt-core │   │  antikolt-engine-host      │ ~200-500 MB
                    │ (Rust lib)    │   │  (CEF / Chromium, separate │  (only when running)
                    │ · SQLite      │   │   process tree)            │
                    │ · vault       │   │  · CefBrowser + CDP        │
                    │ · lifecycle   │   │  · native window or OSR    │
                    │ · snapshots   │   │  · network, cookies, GPU   │
                    │ · search      │   └────────────────────────────┘
                    │ · preview     │
                    └───────┬───────┘
                            │
                    ┌───────▼───────────────────────────────┐
                    │  Disk: SQLite, blob store, partitions │
                    │  (unlimited, default quota 50 GB)     │
                    └───────────────────────────────────────┘
```

**Why not a single process?** Because the memory promise requires that the *engine can be fully
stopped*. A separate process tree (T2 mode) stops after 2 minutes of idleness, and the browser shrinks to **120–180 MB**.
Besides, this is also the only sensible path for multi-engine support: every engine is its own
executable, behind the same IPC contract.

**Why Rust for the core?** The lifecycle machine, the vault, the snapshot store and search require memory safety and
deterministic resource management; Rust provides this, and it is easily testable. CEF and Qt are C++,
therefore the **shell is C++, the core is Rust**, with `cxx` FFI. (If the shell later becomes Rust too, the FFI layer stays.)

## 3. The engine host and the adapter

### 3.1. The contract (7 primitives + 3 supplements)

Every engine implements this interface; the core only sees this. The primitives are deliberately poor,
so that another engine (WebKitGTK, WebView2) can also fulfill them.

```
EnginePort
  create(cfg) -> SurfaceId           // partition, viewport, DPR, permissions, proxy
  navigate(id, url, opts) -> ()      // opts: referrer, hard reload, timeout
  eval(id, script, world) -> Json    // world: isolated | main
  scroll(id, x, y, animate) -> ()
  capture(id, fmt, quality) -> Bytes // for preview (WebP)
  snapshot(id) -> MhtmlBytes         // full DOM, with embedded resources
  destroy(id) -> ()
  --- supplements ---
  set_media(id, prefers_color_scheme, forced_colors)
  set_zoom(id, factor) / metrics(id) -> {fps, dom_nodes, js_heap, mem}
  download / popup / permission events -> event channel
```

**Deliberately NOT in** the interface: extension management, calling functions on the engine's internal C++ objects,
injecting arbitrary native code. This is what preserves the engine's swappability.

### 3.2. Protocol

- Channel: local socket (Windows: named pipe, Linux: `AF_UNIX` in the user's runtime directory),
  with access control (same user only, `0600`).
- Encoding: length-prefixed MessagePack (`rmp-serde`), versioned (`proto/v1`).
- Every message: `{id, op, payload}` / `{reply_to, ok, error}`; 5-second base timeout, the
  `snapshot`/`capture` 15 seconds.
- Frame channel (OSR mode only): double-buffered shared memory + damage rectangles.
- The engine host **never receives a filesystem path** from the core, only a partition name; the partition directory is
  created by the core, the engine host only knows its own `--user-data-dir`.

### 3.3. Integration modes

| Mode | Platform | Advantage | Disadvantage |
|---|---|---|---|
| **Native window** (`SetParent` / `XReparentWindow`) | Windows, X11 | hardware acceleration, simple, fast | not reliable under Wayland; manual input focus handling |
| **OSR** (offscreen, frame sharing) | everywhere | platform-independent, Wayland-safe, uniform | CPU-heavy, complex (vsync, input forwarding) |

**Plan:** v1 Windows = native window; Linux (v0.3) = OSR. The IPC and the adapter are built so that the integration
mode is transparent to it (`EngineSurface` abstraction). Detailed explanation (why, at what cost,
with what risks): `11-OSR.md`.

### 3.4. Engine life modes and resource profiles

The engine is not "on or off", but part of a **profile**. A profile is a bundle: engine lifetime,
hibernation time, preview refresh, media quality, GPU selection, budget bands.

| Profile | Engine | Hibernation | Preview | Video | When |
|---|---|---|---|---|---|
| **Fast** | warm (T1) | 15 min | yes | full | desktop machine |
| **Balanced** *(default)* | warm (T1) | 10 min | yes, when idle | 1080p | everyday use |
| **Efficient** | cold (T2) after 2 min | 5 min | no | 720p | laptop on battery |
| **Ultra** | cold (T2) immediately | 2 min | no | 480–720p | memory pressure |
| **Manual** | every value separate | — | — | — | advanced user |

- T1 = warm engine host (card open ~200–400 ms), T2 = stopped engine host (~600–1200 ms). In T2, the mouse
  hovering over a card starts **pre-warming**, so the click already finds a warm engine.
- The profile can **switch automatically** (wall power ↔ battery, memory pressure), but can be pinned.
- Configuration remembered per machine (`config/devices/<hostname>.toml`).
- Details and the rationale for the profiles: `09-CELHARDVER.md` chapter 3.

## 4. Card lifecycle

**There is no "sleeping tab" state.** This is one of the most important differences of this plan from Chrome's "Memory Saver"
behavior: with us there is no half-dead, de-prioritized, disconnected page that still occupies memory.
A card **either lives** (and then it is the single live view), **or is on disk** (and then it
occupies zero RAM). Between the two there is only a short transition of a few hundred milliseconds.

```
             ┌──────────────────────────── SAVED (dormant) ────────────────────────────┐
             │  no engine, has snapshot + preview + text                               │
             └───────▲──────────────────────────────┬──────────────────────────────────┘
     24h expiry       │                              │ card opened
     (purge)          │                              ▼
             ┌────────┴────────┐            ┌────────────────┐
             │ PURGING         │            │ WARMING        │  engine + navigation + snapshot
             │ (data deletion) │            │ (skeleton img) │  restore attempt
             └────────┬────────┘            └───────┬────────┘
                      │                             ▼
                      │                     ┌───────────────┐
                      │                     │ LIVE          │  the single live view
                      │                     └───┬───────┬───┘
                      │        card left        │       │ 10 min idleness
                      │                         ▼       ▼
                      │                  ┌─────────────────┐
                      └──────────────────│ FREEZING        │  capture + MHTML + scroll + storage
                                         │ (saving)        │  then engine destroy
                                         └─────────────────┘
```

### 4.1. States and transitions

| State | Meaning | Memory |
|---|---|---|
| `saved` | card on the home page, snapshot exists | 0 |
| `warming` | engine starting, page loading, preview/skeleton shown instead | grows |
| `live` | the single live view | full |
| `freezing` | saving in progress (max. 2.5 s), then `destroy` | full |
| `purging` | 24-hour data deletion running | small |
| `archived` | archived by the user: no preview refresh, but the snapshot remains | 0 |

### 4.2. The 10-minute rule (the engine ceases)

- **The idleness timer** starts when the card opens, and **resets on every user input**
  (keyboard, mouse, scroll, click, focus).
- Default **10 minutes**, per card 1–120 minutes or `soha`.
- **Suspending exceptions** (in these cases the engine does not cease):
  - the card is **pinned** (`pinned`),
  - the page is **playing audio** or **video** (`navigator.mediaSession.playbackState`,
    the state of `document.querySelector('video,audio')`, or the "audible" event signaled by the engine),
  - a **file download** is in progress,
  - an **unsaved, long form** is in focus? — *not* an exception: saving preserves it, so it is not needed,
    but the UI shows a discreet bar 5 seconds before ("we are about to save to disk").
- If the user is actively working on a card, but the engine's memory budget grows dangerously (see
  `03-MEMORIA.md` chapter 4), the **budget governor** can request immediate freezing — but only by signaling.

### 4.3. The 24-hour rule (three-level session policy)

The goal is not to log people out, but that **logins remain** — the login is
on disk, not in RAM (in detail: `10-MUNKAMENET.md`). Therefore the 24 hours apply **not to the cookies**,
but to the session's **byproducts**:

| Policy | Deleted after 24 hours | Remains | Recommended |
|---|---|---|---|
| **`keep`** | nothing (only cache LRU) | cookies, storage, login | important services (Google, Facebook, own site) |
| **`sweep`** *(container default)* | cache, `sessionStorage`, Service Worker, code cache | cookies, localStorage, IndexedDB → **stays logged in** | most sites |
| **`wipe`** | everything for the origin (logout) | only the vault record | bank, healthcare, work |

- Expiry counts from the card's **last opening**, and is evaluated lazily (on open and during idle time).
- Deletion is **origin-level** within the given partition: cards of the same domain + same container
  use the same session, so the rule is predictable.
- The **snapshot is never deleted** because of the policy (offline reading and search remain).
- Override: per-card `session_policy`, per-container default, and `wipe_on_lock` for sensitive
  containers.
- Execution **requires the engine** (CDP `Storage.clearDataForOrigin`), because Chromium does not store
  per origin in separate files. Therefore expiry first creates a **mark** (`cleanup_pending`), and deletion runs at
  the engine's next available moment: at the next card opening in that container, or in an
  idle maintenance window (max. 2 s). **We never touch the running engine's files by hand**, and the
  partition directory is only touched when the container is fully deleted.
- The UI shows the policy and the lock state on every card (`🔒` logged in, `⏳` about to be
  deleted, `🔓` logged out).

### 4.4. The "melting back" animation (the card's homecoming)

This concept comes from your phrasing ("they drop down, close up, melt back into the webpage"), and
the UI's language conveys this:

1. When the card is left, the engine **first** creates the snapshot (max. 2.5 s), meanwhile the UI shows a
   thin, moving bar under the control bar ("saving").
2. When saving is done, the live view **shrinks in 250 ms** to the place of the card on the home page
   (FLIP animation: the image transforms from the starting rectangle into the card's rectangle), and the card's
   preview is refreshed.
3. The engine `destroy`s, memory is freed immediately — the user does not perceive this process even if
   the grid is already visible.

The same in reverse: card click → the card's image "opens" into the live view (0 ms, because the image is already
in our hands), while the page loads. Thus there is never an empty, flickering, white transition.

## 5. Disk hibernation — specification

This is the product's most important own capability, therefore it is precisely defined.

### 5.1. What we save (and what we do not)

| Layer | Do we save it? | How | Note |
|---|---|---|---|
| DOM (full, current state) | ✅ | CDP `Page.captureSnapshot` → MHTML | embedded CSS/images too |
| Filled form values | ✅ | part of the DOM (MHTML) | password fields **excluded**, a reference instead |
| Scroll (x, y) + anchor | ✅ | `scrollX/Y` + selector of the topmost visible element + offset | anchor because of lazy loading |
| Viewport, DPR, zoom | ✅ | meta | |
| localStorage, IndexedDB, cache, cookies | ✅ *(does not even need saving)* | the partition stays on disk | hibernation does not delete storage |
| sessionStorage | ✅ | extraction + re-injection | it dies together with the renderer |
| Media player position | ✅ | `currentTime`, `paused`, `volume`, `muted` | |
| Dark mode technique + result | ✅ | meta | |
| Extracted text (for search) | ✅ | from the MHTML | max. 256 KB/card |
| Preview image | ✅ | WebP 260×150 | |
| **JS runtime state** | ❌ | — | impossible; documented limitation |
| **Open WebSocket/WebRTC/SSE** | ❌ | — | reconnection is the page's job |
| **Running animations, timers** | ❌ | — | they restart |
| **Live passkey authentication session** | ❌ | — | must re-authenticate (v1.1) |

### 5.2. MHTML snapshot — how

1. `Page.enable`, then `Page.captureSnapshot {format: "mhtml"}` on the engine (via CDP through CEF or
   via the engine's own API).
2. The resulting MHTML is a snapshot of the DOM, with embedded resources — **it can be opened without a network**.
3. Compression: `zstd` (level 3). Typical size: 1–10 MB per page (20–40 MB for a heavy page).
4. Content addressing: the blob's name is its `sha256`, so snapshots with identical content are stored
   only once.

**Risk and fallback (this must be tried in Phase 0):** *producing* the MHTML is certain (CDP), but
**loading** the MHTML may depend on the engine and build — if CEF cannot open MHTML, restoration
does not work. Two fallbacks:

- **A) Resource map + own request interceptor (recommended fallback):** from the snapshot we extract the
  resources embedded into it, and our own `GetResourceHandler` serves them at the original URL,
  offline. In this case the page's scripts also **run** (from disk), so fidelity is better than MHTML.
- **B) Standalone HTML:** generating a single HTML file filled with `data:` URIs. Simple, but the
  scripts do not run, and for large pages the file size grows.

So MHTML is the **archive/base** format, and the restoration path depends on the Phase 0 measurement.

### 5.3. Disk layout

```
<userdata>/
  antikolt.db                      # SQLite (WAL)
  blobs/ab/cd/<sha256>.zst         # content-addressed blobs (mhtml, text, webp, storage-json)
  containers/<container_id>/       # the engine's partition (user-data-dir) — managed by the engine
  home/                            # the home page's current version + version history
  logs/                            # local log, rotated, with no secrets
  cache/                           # disposable (preview decoding, index)
```

**Quota and garbage collection:** default global quota **50 GB**, per card **100 MB** (the
last 2 snapshots remain). The collector runs in idle time: LRU order, more aggressive for `archived` cards.
The user sees the occupied space per card and in total, and can delete manually.

### 5.4. Durability and fault tolerance

- Write: blob → `fsync` to the temporary file → **atomic rename** → SQLite record.
- A process crash must not leave a half card: the record is written only after the complete blob, the manifest
  contains an integrity hash.
- If saving does not finish within 2.5 seconds (very heavy page), a **partial save** is
  made: preview + scroll + URL; the UI indicates it ("accelerated save"). The user is not blocked.
- During `freezing` the card cannot be reopened (the UI disables it while saving runs).

## 6. Restoration

On three levels, so that opening a card is immediate:

1. **R0 — immediate (0 ms):** the preview image appears enlarged, with a "loading" indicator.
2. **R1 — offline (100–400 ms):** loading the MHTML from disk. The page is **readable statically, without a network**,
   with scrolling at the saved position; header bar: "Offline snapshot · Live loading".
   This is also an **offline reading mode** — it works on a plane, on a slow network too.
3. **R2 — live (300–1200 ms):** navigating to the real URL, then:
   - writing back `sessionStorage` with `Page.addScriptToEvaluateOnNewDocument` (it runs before the page's own
     scripts);
   - `history.scrollRestoration = 'manual'`;
   - scroll restoration **with an anchor**: we repeat (`requestAnimationFrame`, max. 5 s) until the
     `scrollHeight` becomes stable, then we go to the anchor element — so even on infinite-scroll pages we
     reach the correct position;
   - writing back non-secret form values from the isolated world (the secret ones are offered by the vault);
   - media: setting `currentTime` + `pause`.

**Which level activates?** Setting: `mindig élő` (default, on a fast network), `először offline,
kattintásra élő`, `csak offline` (the card becomes a "snapshot reader"). A card opened from the home page
defaults to R0 → R2; the "open snapshot" button is always R1.

## 7. Data model (SQLite)

`WAL`, `synchronous=NORMAL`, `foreign_keys=ON`, `busy_timeout=5000`. All times are `INTEGER` (Unix ms, UTC).
Identifiers: ULID-like texts (time-sortable). Schema version: `PRAGMA user_version`, migrations
`migrations/0001_init.sql` etc.

```sql
-- Workspaces (the home page's tabs/sections)
CREATE TABLE workspace (
  id TEXT PRIMARY KEY, name TEXT NOT NULL, icon TEXT,
  position INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL, deleted_at INTEGER
);

-- Identity containers
CREATE TABLE container (
  id TEXT PRIMARY KEY, name TEXT NOT NULL,
  color TEXT NOT NULL,                    -- #RRGGBB
  icon TEXT,
  partition_key TEXT NOT NULL UNIQUE,     -- 'persist:c_<id>'
  proxy_json TEXT,                        -- {mode:'none'|'http'|'socks5', host, port, user}
  session_ttl_seconds INTEGER NOT NULL DEFAULT 86400,
  session_policy TEXT NOT NULL DEFAULT 'sweep',  -- keep|sweep|wipe
  wipe_on_app_lock INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL, deleted_at INTEGER
);

-- Cards (saved pages)
CREATE TABLE card (
  id TEXT PRIMARY KEY,
  workspace_id TEXT REFERENCES workspace(id) ON DELETE CASCADE,
  container_id TEXT REFERENCES container(id) ON DELETE SET NULL,
  url TEXT NOT NULL,
  title TEXT NOT NULL DEFAULT '',
  favicon_ref TEXT,                        -- blob sha256
  notes TEXT,
  position INTEGER NOT NULL DEFAULT 0,
  layout_json TEXT,                        -- {x,y,w,h,size:'s'|'m'|'l'} on the home page
  theme_mode TEXT NOT NULL DEFAULT 'auto', -- auto|light|grey|dark|amoled
  theme_technique TEXT,                    -- manual override: T0..T6
  zoom REAL NOT NULL DEFAULT 1.0,
  state TEXT NOT NULL DEFAULT 'saved',     -- saved|warming|live|freezing|purging|archived
  pinned INTEGER NOT NULL DEFAULT 0,       -- never hibernate
  session_policy TEXT,                     -- NULL = the container's policy; keep|sweep|wipe
  login_required INTEGER NOT NULL DEFAULT 0,
  last_opened_at INTEGER, open_count INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL, deleted_at INTEGER
);
CREATE INDEX card_ws ON card(workspace_id, position);
CREATE INDEX card_recent ON card(last_opened_at DESC);

-- Snapshots (versioned, pointing to content-addressed blobs)
CREATE TABLE snapshot (
  id TEXT PRIMARY KEY,
  card_id TEXT NOT NULL REFERENCES card(id) ON DELETE CASCADE,
  created_at INTEGER NOT NULL,
  reason TEXT NOT NULL,                    -- switch|idle|close|crash|manual
  url TEXT NOT NULL, final_url TEXT NOT NULL, title TEXT,
  scroll_x INTEGER NOT NULL DEFAULT 0, scroll_y INTEGER NOT NULL DEFAULT 0,
  anchor_json TEXT,                        -- {selector, dx, dy}
  viewport_json TEXT NOT NULL,             -- {w,h,dpr,zoom}
  manifest_ref TEXT NOT NULL,              -- blob
  preview_ref TEXT,                        -- blob (webp)
  text_ref TEXT,                           -- blob (extracted text)
  luminance REAL,                          -- 0..1 (for dark mode checking)
  bytes INTEGER NOT NULL DEFAULT 0,
  engine TEXT NOT NULL,                    -- 'cef'
  integrity TEXT NOT NULL                  -- sha256 of the manifest
);
CREATE INDEX snapshot_card ON snapshot(card_id, created_at DESC);

-- Credentials (the secrets' *_ct columns are encrypted with AES-256-GCM)
CREATE TABLE credential (
  id TEXT PRIMARY KEY,
  container_id TEXT REFERENCES container(id) ON DELETE SET NULL,
  origin TEXT NOT NULL,
  kind TEXT NOT NULL DEFAULT 'password',   -- password|totp|passkey
  label TEXT,
  username_ct BLOB, password_ct BLOB,
  totp_secret_ct BLOB, totp_params_json TEXT,  -- {digits,period,algorithm}
  passkey_ct BLOB,                          -- v1.1
  last_used_at INTEGER, created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL
);
CREATE INDEX credential_origin ON credential(origin, container_id);

-- Session ledger: origin-level record, so that it is measurable,
-- what we keep alive and what it costs (see 10-MUNKAMENET.md)
CREATE TABLE session (
  id TEXT PRIMARY KEY,
  container_id TEXT NOT NULL REFERENCES container(id) ON DELETE CASCADE,
  origin TEXT NOT NULL,
  state TEXT NOT NULL DEFAULT 'unknown',   -- logged_in|logged_out|expired|unknown
  policy TEXT NOT NULL DEFAULT 'sweep',    -- keep|sweep|wipe (inherited from the container)
  cookie_count INTEGER NOT NULL DEFAULT 0,
  cookie_bytes INTEGER NOT NULL DEFAULT 0,
  storage_bytes INTEGER NOT NULL DEFAULT 0, -- localStorage + IDB + Cache Storage
  sw_count INTEGER NOT NULL DEFAULT 0,
  last_seen_at INTEGER,
  last_verified_at INTEGER,               -- last verified login
  last_sweep_at INTEGER,
  live_ram_bytes INTEGER,                  -- measured value, if currently running; NULL if dormant
  keep_alive INTEGER NOT NULL DEFAULT 0,   -- optional keep-alive (1 request / 15-60 min)
  updated_at INTEGER NOT NULL
);
CREATE UNIQUE INDEX session_origin ON session(container_id, origin);

-- Per-site recipe (learned settings)
CREATE TABLE site_recipe (
  origin TEXT PRIMARY KEY,
  theme_mode TEXT, theme_technique TEXT, zoom REAL,
  permissions_json TEXT,                   -- {camera:'deny', notifications:'ask', ...}
  form_hints_json TEXT,                    -- {selector: 'username'|'password'|'otp'}
  login_runbook_json TEXT,                 -- step sequence (see 9.5)
  blocked_runbooks INTEGER NOT NULL DEFAULT 0,
  updated_at INTEGER NOT NULL
);

CREATE TABLE tag (id TEXT PRIMARY KEY, name TEXT NOT NULL UNIQUE, color TEXT);
CREATE TABLE card_tag (
  card_id TEXT NOT NULL REFERENCES card(id) ON DELETE CASCADE,
  tag_id  TEXT NOT NULL REFERENCES tag(id)  ON DELETE CASCADE,
  PRIMARY KEY (card_id, tag_id)
);

-- Local log (no secrets), rotated. Deliberately no FK: the log remains after the card is deleted.
CREATE TABLE event (
  id INTEGER PRIMARY KEY AUTOINCREMENT, at INTEGER NOT NULL,
  kind TEXT NOT NULL, card_id TEXT, detail_json TEXT
);

-- Full-text search: word-based AND substring-based (handling Hungarian accents)
CREATE VIRTUAL TABLE card_fts USING fts5(
  card_id UNINDEXED, title, url, notes, content,
  tokenize = "unicode61 remove_diacritics 2"
);
CREATE VIRTUAL TABLE card_fts_trigram USING fts5(
  card_id UNINDEXED, content, tokenize = "trigram"
);
```

**Why two FTS indexes?** The `unicode61 remove_diacritics 2` handles Hungarian accents (á, é, ő, ű) too
for word search; the `trigram` is needed for substring, type-as-you-search. Both are local, no
network.

**Search syntax:** `alma kenyér` (and), `"pontos kifejezés"`, `tag:munka`, `container:munka`,
`workspace:otthon`, `site:youtube.com`, `before:2026-01-01`, `is:pinned`.

## 8. Containers

- Every container = one engine partition (`persist:c_<id>`), with its own cookie, storage and cache directory.
- **Isolation:** no crossing between two containers; the same domain in two containers is two separate accounts.
- **Deletion/origin-wipe:** the 24-hour rule and the "delete session" button are origin-level.
- **Proxy:** configurable per container (the engine's startup parameter, or CDP
  `Network.setProxyOverride` — must be verified per engine). This is one of the strongest differentiators:
  "work container = company VPN, private container = home network".
- **Container color:** the control bar always shows which identity you are in (color code + name), and the
  card borders on the home page indicate it. This protects against "posting to the wrong account".
- **Container deletion:** deleting the entire partition directory, with confirmation, together with the credentials
  belonging to it (optionally preserved).

### 8.5. Popups, OAuth/SSO and login windows

The "automatic login everywhere" mostly happens through a **popup** ("Sign in with
Google account"), therefore this is not a detail but a core service:

- The engine's popup request (CEF `OnBeforePopup`) is opened by the core as a **real, temporary window**,
  **in the same partition** — otherwise the process does not inherit the cookies, and the login gets stuck.
- The popup is **not a card**: it ceases on close (no snapshot, no preview). The user can
  "save it as a card" if they want to keep it after all.
- `window.opener` and the `redirect_uri` redirection work; the whole SSO chain (`accounts.google.com`
  → back to the service) stays within a single container.
- The popup follows the live view's lifecycle: if the engine host stops, the popup ceases too. Therefore if
  authentication is in progress, the browser **does not stop** the engine, or warns.
- This is the **only documented exception to the I1 invariant**: for a short time 2 renderers run (card + popup),
  for at most 60 s; the measurement bench handles this separately.

## 9. Vault, autofill, TOTP

### 9.1. Key hierarchy

```
DEK (32 bytes, random)  ──AES-256-GCM──▶  all secrets (credential, TOTP, passkey)
   ▲
   ├─ wrap #1: system keyring (Windows DPAPI / Linux Secret Service), with application-specific entropy
   └─ wrap #2: optional master password → Argon2id (m=64 MiB, t=3, p=4) → AES-256-GCM key
```

- Two wraps make possible: fast opening with the system key, **portability** with a master password (export/import,
  another machine), and key rotation.
- The DEK lives only in the Rust core's memory, after unlocking; on lock it is zeroed. Auto-lock:
  5 minutes of idleness or the screen locking (Windows `WM_WTSSESSION_CHANGE`, Linux logind signal).
- If the system key is unavailable (e.g. no Secret Service on Linux), the UI **warns** and
  asks for the master password — it never silently falls back to plaintext.

### 9.2. Filling (autofill)

- The script runs in an **isolated world** (CDP `Page.createIsolatedWorld`), so the page's JS does not see its
  variables, only the DOM changes.
- Filling is done with the **native setter** (`Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,
  'value').set.call(el, v)`) + `input`/`change`/`blur` events — otherwise React/Vue-based pages do not
  notice it.
- The picker UI appears in a **closed Shadow DOM** (the page cannot restyle it, and the closed
  shadow's content cannot be read by the page's JS).
- **Origin binding:** filling only on a matching origin (after IDN/punycode normalization), and only into top-level
  or same-origin iframes; never into cross-origin iframes. In suspicious cases (homograph domain) it warns.
- **Saving:** on form submission / navigation it offers to save; the password is only stored encrypted;
  when it detects "password changed" it asks to update.

### 9.3. TOTP

Per RFC 6238 (SHA-1/6 digits/30 s is the default; SHA-256/8 digits/60 s is also supported). The code is computed by the core,
the UI shows it with a countdown ring, and **clears the clipboard 30 s after copying**
(if the clipboard still contains the same thing). Clock offset handling: the option to align to server time.

### 9.4. What the vault never does

- It sends nothing to the network (no cloud sync in v1).
- It does not log secrets, does not put secrets on the home page's bridge, does not give secrets to the main world.
- It does not fill a password on an `http://` page (only `https://` and `localhost`).
- It does not fill without a user gesture (click/key).

### 9.5. Login scenario (login runbook)

A declarative step sequence per card (the `site_recipe.login_runbook_json`):

```json
{
  "version": 1,
  "steps": [
    {"op": "goto", "url": "https://example.com/login"},
    {"op": "wait_for", "selector": "input[type=email]", "timeout_ms": 8000},
    {"op": "fill", "selector": "input[type=email]", "from": "credential.username"},
    {"op": "click", "selector": "button[type=submit]"},
    {"op": "wait_for", "selector": "input[type=password]", "timeout_ms": 8000},
    {"op": "fill", "selector": "input[type=password]", "from": "credential.password"},
    {"op": "click", "selector": "button[type=submit]"},
    {"op": "wait_for", "selector": "input[autocomplete=one-time-code]|iframe[title*=captcha]", "timeout_ms": 15000},
    {"op": "fill_totp", "selector": "input[autocomplete=one-time-code]", "from": "credential.totp"}
  ],
  "on_failure": "hand_over_to_user"
}
```

The executor runs for **max. 20 seconds**, does not click anything that is payment/deletion/posting (a blocked
operation list), and can be interrupted at any time. If it sees a CAPTCHA, it hands over to the user. This is the
compromise that is **safely** achievable from the "log in everywhere" promise.

### 9.6. Sessions and the cost of login

Alongside the vault there is a separate subsystem, the **session ledger**: it records, for every `container × origin` pair,
whether it is logged in, how large its disk footprint is, whether it has a Service Worker, which policy it follows, and
how much RAM it uses if it is currently running. The UI's **Sessions** panel shows this, and states that
keeping a login alive costs **0 RAM** as long as the card is hibernated — the real cost is the
warm engine host, the connection kept alive in the background, and the Service Workers.

The full analysis, the "what we lose" table and the keep-alive rules: `10-MUNKAMENET.md`.

## 10. Theme engine: light / grey / dark / AMOLED + darkening technique ladder

### 10.1. The interface (our own UI)

Four themes, with design tokens (CSS variables + QML palette): `light`, `grey` (neutral, medium brightness —
this is the default, because it is good both day and evening), `dark`, `amoled` (pure black, for OLED).
Overridable per card; switching light/grey/dark from the control bar is one click.

### 10.2. Darkening pages — the ladder

| Level | Technique | When it wins |
|---|---|---|
| **T0** | emulating `prefers-color-scheme: dark` | the page natively supports dark (the best result) |
| **T1** | Chromium's built-in automatic darkening | fast, medium quality on pages |
| **T2** | Generated stylesheet (Dark Reader-like engine, adapted from an MIT-licensed base) | most news sites, blogs, documentation |
| **T3** | Inversion + re-inversion of images/video | old, static pages |
| **T4** | Forced contrast (`forced-colors`) | accessible/simple pages |
| **T5** | Dimming layer (brightness, contrast) | anytime, as a last resort |
| **T6** | Reading mode (own, themed article view) | articles, ideal for grey mode |

**Grey mode** on pages: desaturation + slight dimming (`grayscale(0.9) brightness(0.92)`),
optionally combined with T6 — this is "distraction-free" reading.

### 10.3. Self-check (the essence that is missing from the Gemini plan)

After every darkening the engine provides a **screenshot**, from which we compute the **average relative luminance**
(downscaled to 32×32). If after the desired mode (dark) the luminance > 0.45, the browser **steps up to the next
technique**. The result is stored in `site_recipe`, so on the next opening the winning technique
starts. This is measurable, testable and learns per site — not a matter of faith.

Additionally we record the "broken" cases (e.g. the content disappeared after the technique: the number of DOM elements drastically
decreased) → automatic fallback to the previous technique + a signal to the UI ("this page does not like
dark mode").

## 11. Home page: the card grid as an editable webpage

### 11.1. Model

- The home page is a **local HTML document** (`antikolt://home`), which the browser itself serves
  from the filesystem. By default the built-in **card grid template**.
- **"Editability" applies to the home page (the library), not to foreign webpages' content.** A
  card's content is the webpage's own (or its saved snapshot); the user edits the *library*, the
  arrangement, appearance and notes of the cards — not the foreign page's DOM. (Exception:
  user scripts per card, in a later phase.)
- Cards appear as **web components**: `<antikolt-card card-id="01J...">`, which the user
  can insert anywhere (into a grid, a section, two columns, their own design).
- The user can modify the home page with the **internal editor** (HTML/CSS/JS, with preview, undo,
  version history), or write it **externally** and pull in their own file.
- Home page versions are versioned (`home/versions/`), restorable, exportable into a single
  `.antikolt-home` file (HTML + layout JSON + without previews).

### 11.2. Bridge API (between the home page and the core)

The home page **gets no** filesystem or network access; only this narrow API:

```
antikolt.cards.list({workspace, filter, sort}) -> CardMeta[]
antikolt.cards.get(cardId) -> CardMeta
antikolt.cards.open(cardId, {mode})           // live | offline snapshot
antikolt.cards.setLayout(cardId, layout)
antikolt.cards.create({url, title, ...})
antikolt.search(query) -> CardMeta[]
antikolt.workspaces.list() / .switch(id)
antikolt.containers.list()
antikolt.events.on('open'|'close'|'preview-updated', cb)
```

This goes over `postMessage`, with an **allowlist** and schema validation on the core side. The home page cannot
request arbitrary native operations.

### 11.3. Security

- CSP: `default-src 'none'; img-src data: antikolt:; style-src 'unsafe-inline'` — **no remote script can be
  loaded** onto the home page (except if the user explicitly allows it for a given origin).
- Sanitization of inserted HTML (content from an untrusted source).
- The home page runs in the engine, but in a **different partition** (dedicated `persist:c_home`), so that cookies do not
  mix; by default offline toward the network.
- A home page crash cannot take the browser with it; if the home page is broken, the browser falls back to the built-in
  template and reports the error.

## 12. Preview pipeline

- **When it is made:** on card freeze, on card open (refresh), and according to the scheduler.
- **How:** `capture` → 260×150 → WebP (q=80) → blob + `luminance` + `dhash` (filtering identical images).
- **Scheduler (keeping the "no background engine" principle):** only if (a) the user is on the home page,
  (b) no typing/mouse movement for 30 s, (c) no other engine task is running, (d) the card's preview is > 7 days old,
  (e) optionally only on charger. **One** card at a time, 8 s time limit, then immediate freezing.
- **Limit:** max. 100 refreshes per day, and the user can turn it off.
- **Home page-side requirement:** virtualized grid, only the visible cards in the DOM; decoded
  bitmap ceiling (e.g. 24 images); a card without a preview gets a placeholder.

## 13. Search

- Indexed fields: title, URL, notes, tags, extracted page text (max. 256 KB/card).
- Two FTS5 indexes (word + trigram), see chapter 7.
- Goal: under **50 ms** response even above 10,000 cards, without an in-memory index (SQLite page cache).
- Search is accessible through the home page bridge, and the `Ctrl+K` quick search also uses this.

## 14. Security model

**Threats:** a malicious webpage; a malicious downloaded file; foreign HTML inserted onto the home page;
another local process that would read the browser's data; a network attacker; a stolen laptop.

**Countermeasures:**

| Area | Measure |
|---|---|
| Pages | the engine's own sandbox + site isolation; no native access; permissions by origin+container |
| Autofill | isolated world, closed Shadow DOM, origin binding, user gesture |
| Secrets | AES-256-GCM, DEK/KEK, Argon2id option, auto-lock, clipboard clearing |
| Home page | strict CSP, sanitization, allowlist bridge, separate partition |
| Downloads | separate folder, "run" forbidden, optional Windows Defender scan, confirmation |
| Certificates | error = hard stop, override only per card, logged |
| IPC | local socket, `0600`, message schema validation, handling untrusted input |
| Privacy | no telemetry, no cloud, local log rotated, with no secrets |
| Cryptography | system library (OS CSPRNG, DPAPI, Secret Service), not an own implementation |

## 15. Platform layer

| Area | Windows 11 (v1) | Nobara/Fedora + KDE (v0.3) |
|---|---|---|
| Window | Qt 6 + native CEF window (`SetParent`) | Qt 6 + OSR (Wayland) / native (X11) |
| Scaling | DPI-aware, 125–200% tested | Qt high-DPI + fractional scaling |
| Keyring | DPAPI | Secret Service (KWallet/gnome-keyring) |
| Notifications | Windows toast | `org.freedesktop.Notifications` |
| Packaging | MSI/NSIS installer | RPM, later Flatpak |
| Update | own, signed updater | RPM repo / Flatpak |

**Wayland risk:** native window embedding depends on the `xdg-foreign` protocol, which varies per compositor;
therefore on Linux OSR is the primary path, native embedding only under X11.

## 16. Engine adapter and the future engines

- `EnginePort` (3.1.) + `EngineSurface` (integration) + event channel. The core only knows this.
- **Engine #1: CEF** (Chromium). Advantage: full control, CDP, stable, BSD license.
- **Engine #2 candidate: WebKitGTK** (Linux) — smaller memory footprint, a good "light mode"; disadvantage: different
  compatibility, OSR-centric integration.
- **Engine #3 candidate: WebView2** (Windows) — ships with the system, small installer; disadvantage: little control
  (DevTools, popup, profile), therefore only for a "compatibility engine" role.
- **Excluded:** Gecko (no embedding API), Servo and Ladybird (in 2026 not production-ready for embedding).
- Choosing the engine is possible per card/container (e.g. "open this page with WebKitGTK"),
  but the default is CEF. Switching engines creates a **new snapshot** (the two engines' DOMs differ).

## 17. Non-functional requirements

| Requirement | Target | Measurement |
|---|---|---|
| Startup to the home page | ≤ 1.5 s cold, ≤ 0.6 s warm | measurement bench |
| Perceived card open time | ≤ 600 ms (R0 immediate) | measurement bench |
| Freezing (saving) | ≤ 2.5 s, even on a BIG page | measurement bench |
| Search | ≤ 50 ms / 10,000 cards | benchmark |
| Memory | see `03-MEMORIA.md` | CI gate |
| Disk (base quota) | 50 GB, 100 MB per card | admin interface |
| Database size | < 200 MB at 10,000 cards (without the blobs) | benchmark |
| Accessibility | full keyboard navigation, screen reader support | manual test |
| Languages | HU + EN (v1) | i18n test |

### 17.1. Open dependencies (what the plan assumes, but is not yet proven)

These points are **risks, not decisions** — Phase 0 must prove or disprove them:

1. **MHTML loadability** in the engine (producing it is certain) — see 5.2. fallbacks.
2. **Popup anatomy with CEF**: `OnBeforePopup` + a temporary window of the same partition, with `window.opener`
   working, with cross-site login chains.
3. Accessing an **isolated world** on the engine (CDP `Page.createIsolatedWorld` or the engine's own API) — the filler's
   security stands on this.
4. **Per-origin data deletion** in the partition (whether CDP `Storage.clearDataForOrigin` deletes exactly what
   we think, and what the running page sees of it).
5. The **login scenario's** DOM dependencies: selectors break on page changes — this is why the
   "manual handover" and the `blocked_runbooks` counter are needed.
6. The real cost of **luminance measurement** (image → 32×32 → average): negligible per the plan, but must be measured.
7. The **weight of the home page (HTML)**: how many MB the user's own home page occupies in the renderer.
8. Keeping **the I1 invariant** between the home page and the card (the home page's renderer must be stopped when a card opens,
   and restored on close — this is cheap because of the "raster home page").
9. The stability of **native window embedding** (focus, IME, DPI change, alt-tab) on Windows 11.
10. **Argon2id**'s chosen parameters (64 MiB / 3 / 4) on hardware that keeps getting faster — to be reviewed every 2 years.

## 18. Repo structure and technology
```
antikolt/
  apps/desktop-shell/          # C++20 + Qt 6 (QML), CEF integration, platform layer
  crates/antikolt-core/        # Rust: db, vault, lifecycle, snapshot, search, preview
  crates/antikolt-ipc/         # message schema (serde + rmp), versioning, tests
  crates/antikolt-engine-host/ # the engine host executable (CEF), later other engines
  home/                        # built-in home page (HTML/CSS/JS) + templates
  proto/                       # IPC schema description + generated code
  migrations/                  # SQL migrations
  perf/                        # measurement bench, baselines, reports
  tests/e2e/                   # Playwright/Qt-test scenarios
  docs/{HU,EN,forras}
```

| Layer | Choice | Reason |
|---|---|---|
| Core | Rust (stable), `rusqlite` or `sqlx` (SQLite), `serde`, `zstd`, `image`, `argon2`, `aes-gcm` | security, determinism |
| Shell | C++20, Qt 6 (QML), CMake | fits CEF's C++; QML fast UI |
| Engine | CEF (Chromium) + CDP | control, snapshot, theme emulation |
| FFI | `cxx` | type-safe Rust↔C++ |
| Test | Rust unit + C++ unit (Catch2) + e2e (Playwright) + measurement bench | per layer |
| Packaging | CMake + CPack/NSIS (Win), RPM (Linux) | platform-native |

## 19. Sources (the documentation the plan is based on)

- CEF — https://github.com/chromiumembedded/cef
- Chrome DevTools Protocol (Page.captureSnapshot, Emulation.setEmulatedMedia, Storage.clearDataForOrigin) —
  https://chromedevtools.github.io/devtools-protocol/
- SQLite FTS5 — https://www.sqlite.org/fts5.html
- DPAPI — https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptprotectdata
- Secret Service API — https://specifications.freedesktop.org/secret-service/latest/
- Argon2 (RFC 9106) — https://www.rfc-editor.org/rfc/rfc9106.html
- TOTP (RFC 6238) — https://www.rfc-editor.org/rfc/rfc6238
- Qt 6 LGPL obligations — https://doc.qt.io/qt-6/lgpl.html
- xdg-foreign (Wayland embedding) — https://wayland.app/protocols/xdg-foreign-unstable-v2
- MPL-2.0 — https://www.mozilla.org/en-US/MPL/2.0/
