-- SPDX-License-Identifier: MPL-2.0
--
-- Antikolt schema, migration 1.
--
-- The authoritative description is docs/HU/02-ARCHITEKTURA.md §7; this file is the implementation
-- of that section. Rules:
--   * a migration is append-only: never edit a shipped migration, add a new one;
--   * timestamps are INTEGER milliseconds since the Unix epoch, UTC;
--   * identifiers are 26-character ULIDs stored as TEXT (see crates/antikolt-core/src/ids.rs);
--   * secrets live only in the *_ct columns and are AES-256-GCM ciphertext (ADR-010).
--
-- The core applies this inside a transaction and records the version in PRAGMA user_version.

-- Application-level key/value state (device profile, home page version, quota overrides).
CREATE TABLE app_meta (
  key        TEXT PRIMARY KEY,
  value      TEXT NOT NULL,
  updated_at INTEGER NOT NULL
);

-- Workspaces group cards on the home page.
CREATE TABLE workspace (
  id         TEXT PRIMARY KEY,
  name       TEXT NOT NULL,
  icon       TEXT,
  position   INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL,
  deleted_at INTEGER
);

-- Identity containers: one isolated engine partition each.
CREATE TABLE container (
  id                   TEXT PRIMARY KEY,
  name                 TEXT NOT NULL,
  color                TEXT NOT NULL,                    -- '#RRGGBB', one of the eight identity colours
  icon                 TEXT,
  partition_key        TEXT NOT NULL UNIQUE,             -- 'persist:c_<id>'
  proxy_json           TEXT,                             -- {mode, host, port, user}
  session_ttl_seconds  INTEGER NOT NULL DEFAULT 86400,   -- ADR-009, per-container default
  session_policy       TEXT NOT NULL DEFAULT 'sweep',    -- keep | sweep | wipe
  wipe_on_app_lock     INTEGER NOT NULL DEFAULT 0,
  created_at           INTEGER NOT NULL,
  deleted_at           INTEGER,
  CHECK (session_policy IN ('keep', 'sweep', 'wipe'))
);

-- Saved pages.
CREATE TABLE card (
  id              TEXT PRIMARY KEY,
  workspace_id    TEXT REFERENCES workspace(id) ON DELETE CASCADE,
  container_id    TEXT REFERENCES container(id) ON DELETE SET NULL,
  url             TEXT NOT NULL,
  title           TEXT NOT NULL DEFAULT '',
  favicon_ref     TEXT,                                  -- blob digest (hex)
  notes           TEXT,
  position        INTEGER NOT NULL DEFAULT 0,
  layout_json     TEXT,                                  -- {x, y, w, h, size}
  theme_mode      TEXT NOT NULL DEFAULT 'auto',          -- auto | light | grey | dark | amoled
  theme_technique TEXT,                                  -- T0..T6 override
  zoom            REAL NOT NULL DEFAULT 1.0,
  state           TEXT NOT NULL DEFAULT 'saved',         -- saved | archived
  pinned          INTEGER NOT NULL DEFAULT 0,            -- never auto-hibernate
  session_policy  TEXT,                                  -- NULL = inherit the container's policy
  login_required  INTEGER NOT NULL DEFAULT 0,
  last_opened_at  INTEGER,
  open_count      INTEGER NOT NULL DEFAULT 0,
  created_at      INTEGER NOT NULL,
  updated_at      INTEGER NOT NULL,
  deleted_at      INTEGER,
  CHECK (state IN ('saved', 'archived')),
  CHECK (zoom > 0.0),
  CHECK (session_policy IS NULL OR session_policy IN ('keep', 'sweep', 'wipe')),
  CHECK (theme_mode IN ('auto', 'light', 'grey', 'dark', 'amoled'))
);

CREATE INDEX card_workspace ON card(workspace_id, position);
CREATE INDEX card_recent ON card(last_opened_at DESC);
CREATE INDEX card_container ON card(container_id);

-- Hibernation snapshots: one row per freeze; blobs hang off the manifest.
CREATE TABLE snapshot (
  id                 TEXT PRIMARY KEY,
  card_id            TEXT NOT NULL REFERENCES card(id) ON DELETE CASCADE,
  created_at         INTEGER NOT NULL,
  reason             TEXT NOT NULL,                      -- switch | idle | close | crash | manual
  url                TEXT NOT NULL,
  final_url          TEXT NOT NULL,
  title              TEXT,
  scroll_x           INTEGER NOT NULL DEFAULT 0,
  scroll_y           INTEGER NOT NULL DEFAULT 0,
  anchor_json        TEXT,                               -- {selector, dx, dy}
  viewport_json      TEXT NOT NULL,                      -- {width, height, device_scale_percent}
  manifest_ref       TEXT NOT NULL,
  preview_ref        TEXT,
  text_ref           TEXT,
  luminance_permille INTEGER,
  bytes              INTEGER NOT NULL DEFAULT 0,
  engine             TEXT NOT NULL,
  integrity          TEXT NOT NULL,
  CHECK (reason IN ('switch', 'idle', 'close', 'crash', 'manual')),
  CHECK (scroll_x >= 0 AND scroll_y >= 0)
);

CREATE INDEX snapshot_card ON snapshot(card_id, created_at DESC);

-- Deferred maintenance: session cleanup that needs the engine to be alive to execute.
CREATE TABLE pending_maintenance (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  kind         TEXT NOT NULL,                            -- session_cleanup | quota_gc | blob_reverify
  container_id TEXT,
  origin       TEXT,
  payload_json TEXT,
  created_at   INTEGER NOT NULL,
  attempts     INTEGER NOT NULL DEFAULT 0,
  last_error   TEXT
);

CREATE INDEX pending_maintenance_kind ON pending_maintenance(kind, created_at);

-- Vault records; the *_ct columns are AES-256-GCM ciphertext (ADR-010).
CREATE TABLE credential (
  id              TEXT PRIMARY KEY,
  container_id    TEXT REFERENCES container(id) ON DELETE SET NULL,
  origin          TEXT NOT NULL,
  kind            TEXT NOT NULL DEFAULT 'password',      -- password | totp | passkey
  label           TEXT,
  username_ct     BLOB,
  password_ct     BLOB,
  totp_secret_ct  BLOB,
  totp_params_json TEXT,                                 -- {digits, period, algorithm}
  passkey_ct      BLOB,
  last_used_at    INTEGER,
  created_at      INTEGER NOT NULL,
  updated_at      INTEGER NOT NULL,
  CHECK (kind IN ('password', 'totp', 'passkey'))
);

CREATE INDEX credential_origin ON credential(origin, container_id);

-- Session ledger (ADR-019): what is alive, what it costs, and what a wipe would lose.
CREATE TABLE session (
  id               TEXT PRIMARY KEY,
  container_id     TEXT NOT NULL REFERENCES container(id) ON DELETE CASCADE,
  origin           TEXT NOT NULL,
  state            TEXT NOT NULL DEFAULT 'unknown',      -- logged_in | logged_out | expired | unknown
  policy           TEXT NOT NULL DEFAULT 'sweep',        -- keep | sweep | wipe
  cookie_count     INTEGER NOT NULL DEFAULT 0,
  cookie_bytes     INTEGER NOT NULL DEFAULT 0,
  storage_bytes    INTEGER NOT NULL DEFAULT 0,
  service_workers  INTEGER NOT NULL DEFAULT 0,
  last_seen_at     INTEGER,
  last_verified_at INTEGER,
  last_sweep_at    INTEGER,
  live_ram_bytes   INTEGER,
  keep_alive       INTEGER NOT NULL DEFAULT 0,
  updated_at       INTEGER NOT NULL,
  CHECK (state IN ('logged_in', 'logged_out', 'expired', 'unknown')),
  CHECK (policy IN ('keep', 'sweep', 'wipe')),
  UNIQUE (container_id, origin)
);

CREATE INDEX session_state ON session(state, last_seen_at);

-- Per-site learned settings.
CREATE TABLE site_recipe (
  origin            TEXT PRIMARY KEY,
  theme_mode        TEXT,
  theme_technique   TEXT,
  zoom              REAL,
  permissions_json  TEXT,
  form_hints_json   TEXT,
  login_runbook_json TEXT,
  blocked_runbooks  INTEGER NOT NULL DEFAULT 0,
  updated_at        INTEGER NOT NULL
);

CREATE TABLE tag (
  id    TEXT PRIMARY KEY,
  name  TEXT NOT NULL UNIQUE,
  color TEXT
);

CREATE TABLE card_tag (
  card_id TEXT NOT NULL REFERENCES card(id) ON DELETE CASCADE,
  tag_id  TEXT NOT NULL REFERENCES tag(id) ON DELETE CASCADE,
  PRIMARY KEY (card_id, tag_id)
);

-- Local audit log: never contains page content or secrets.
CREATE TABLE event (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  at          INTEGER NOT NULL,
  kind        TEXT NOT NULL,
  card_id     TEXT,
  detail_json TEXT
);

CREATE INDEX event_recent ON event(at DESC);

-- Full-text search: word-based with Hungarian diacritics, plus substring search while typing.
-- These are ordinary (not external-content) FTS5 tables; the core keeps them in sync.
CREATE VIRTUAL TABLE card_fts USING fts5(
  card_id UNINDEXED,
  title,
  url,
  notes,
  content,
  tokenize = 'unicode61 remove_diacritics 2'
);

CREATE VIRTUAL TABLE card_fts_trigram USING fts5(
  card_id UNINDEXED,
  content,
  tokenize = 'trigram'
);
