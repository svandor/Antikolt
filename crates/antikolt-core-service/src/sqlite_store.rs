// SPDX-License-Identifier: MPL-2.0

//! The store's home: one SQLite file instead of four JSON documents (ADR-064 → ADR-069 → ADR-070).
//!
//! Why: the JSON files are atomic and readable, but nothing *queries* them. Every "which cards are in this
//! container", "what does this session hold", "what is due" question walks the whole list in the core. That is fine
//! for 10 cards and wrong for 5 000, and it is the reason the roadmap listed the store swap as the next big step.
//!
//! Since ADR-069 the **cards are SQLite-first**; since ADR-070 the **containers, workspaces and sessions are too**.
//! The four JSON documents are derived exports now: written from the rows on adoption, on shutdown, and when
//! `db_status` asks for a sync, and read exactly once — to migrate a profile written before the switch. Each store
//! owns its own `user_version` threshold, so a store never adopts its own export back (a stale `cards.json` must not
//! roll the database back once the cards have moved).
//!
//! The schema version is a number in `PRAGMA user_version` — not a table — because it is the SQLite idiom and it
//! survives any table rename: `0002` adds `page_text`, `tags` and `workspace`; `0003` records the card switch; `0004`
//! reshapes `sessions` to the ledger row and adds `session_rules` for the other three stores' switch.

use std::path::{Path, PathBuf};

use rusqlite::{params, Connection, OptionalExtension};

use antikolt_core::profile::ProfileKind;
use antikolt_core::session::{LoginState, SessionLedgerEntry, SessionPolicy, SessionRules};
use serde::{Deserialize, Serialize};

use crate::containers::Container;
use crate::store::Card;
use crate::workspaces::Workspace;

/// The name of the database file inside the profile directory. One file, next to the JSON documents it replaces.
pub const DB_FILE_NAME: &str = "antikolt.sqlite3";

/// The schema this core writes. Bumped only with a migration; `0001` was the JSON files, `0002` added `page_text`,
/// `tags` and `workspace`, `0003` is the card-store switch (ADR-069), `0004` the other three stores' switch (ADR-070),
/// `0005` the per-container proxy and permission policy (ADR-071), `0006` the one row of user preferences (ADR-072),
/// `0007` the memory budget column (F6), `0008` the per-card `memory_bytes` (F6), `0009` the resource profile (F6),
/// `0010` the resource profile values renamed to ADR-020's snake_case names.
pub const SCHEMA_VERSION: i64 = 10;

/// The schema version that means "the cards are SQLite-first" (ADR-069). A file at least this version already has the
/// cards' adoption behind it, which the `0004` migration records in `meta` (the per-store adoption markers live there,
/// not in `user_version`, because the four stores open one after another and must not depend on their order).
pub const CARD_SWITCH_VERSION: i64 = 3;

/// The tags travel as a JSON array in one column: SQLite has no array type, and a join table for a 12-item list
/// would cost more than it explains.
#[derive(Debug)]
pub struct Db {
    path: PathBuf,
    connection: Connection,
    /// The `user_version` the file reported *before* this open migrated it. It is how each store knows whether its
    /// JSON document is still the writer of record (and has to be adopted) or already an export (ADR-069/070).
    opened_at_version: i64,
}

#[derive(Debug, thiserror::Error)]
pub enum DbError {
    #[error("cannot open the store: {0}")]
    Sqlite(#[from] rusqlite::Error),
    #[error("cannot create the data directory: {0}")]
    Io(#[from] std::io::Error),
}

/// One row of user preferences: the live-card cap and the two hibernation timings.
///
/// Stored in the `user_prefs` table (schema 0007), one row only (`id = 1`, enforced by a CHECK constraint).
/// A profile that never chose anything gets `defaults()`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct UserPrefs {
    /// How many cards may stay live at once (1..=12).
    pub max_live_cards: u32,
    /// Idle time before a card hibernates (60000..=3600000 ms).
    pub idle_hibernate_ms: u64,
    /// Idle time before a card goes cold (3600000..=2592000000 ms).
    pub cold_card_ms: u64,
    /// The memory budget the user asked for (64..=4096 MiB). `system.memory` reports it as `budgetBytes`, so the
    /// grid can draw a used/budget bar instead of a bare number (F6).
    ///
    /// Defaulted on deserialisation so a profile bundle written before F6 (whose prefs lack this field) still imports:
    /// an older bundle keeps the factory budget instead of being refused.
    #[serde(default = "default_memory_budget_mb")]
    pub memory_budget_mb: u32,
    /// Which resource profile the memory governor follows (F6, ADR-020 / `09-CELHARDVER.md` §3): a built-in
    /// profile (Fast/Balanced/Saver/Ultra) or Manual. A built-in profile is a preset — on write, its hibernation
    /// window overrides the individual field; `Manual` leaves the individual values authoritative.
    #[serde(default = "default_resource_profile")]
    pub resource_profile: ProfileKind,
}

/// The factory memory budget, shared by `UserPrefs::default` and the `serde` default.
pub fn default_memory_budget_mb() -> u32 {
    512
}

/// The factory resource profile: `Balanced`, the every-day preset (ADR-020).
pub fn default_resource_profile() -> ProfileKind {
    ProfileKind::Balanced
}

/// The profile's snake_case wire/SQL name, kept in one place so the database and the JSON agree.
pub fn profile_kind_to_str(kind: ProfileKind) -> &'static str {
    match kind {
        ProfileKind::Fast => "fast",
        ProfileKind::Balanced => "balanced",
        ProfileKind::Saver => "saver",
        ProfileKind::Ultra => "ultra",
        ProfileKind::Manual => "manual",
    }
}

/// Read a stored profile name; anything unrecognised (an older build's value) falls back to `Balanced`.
pub fn profile_kind_from_str(value: &str) -> ProfileKind {
    match value {
        "fast" => ProfileKind::Fast,
        "saver" => ProfileKind::Saver,
        "ultra" => ProfileKind::Ultra,
        "manual" => ProfileKind::Manual,
        _ => ProfileKind::Balanced,
    }
}

impl Default for UserPrefs {
    fn default() -> Self {
        Self {
            max_live_cards: 5,
            // Ten minutes idle hibernates a card, a day without being opened makes it cold (ADR-072). The two are
            // ordered on purpose: cold is the later state, never the earlier one.
            idle_hibernate_ms: 600_000,
            cold_card_ms: 86_400_000,
            // Half a gibibyte is the factory budget (F6): generous for an idle shell, tight for many live cards.
            memory_budget_mb: default_memory_budget_mb(),
            resource_profile: ProfileKind::Balanced,
        }
    }
}

impl UserPrefs {
    /// The factory settings: 5 live cards, 10 min idle → hibernate, 24 h idle → cold.
    #[allow(dead_code)] // public API; used by tests and the portable bundle
    pub fn defaults() -> Self {
        Self::default()
    }

    /// Refuse values outside the sane ranges. The three levels are ordered: idle hibernation comes first, the cold
    /// state later (ADR-072), so a cold window shorter than the idle one describes a card that goes cold before it is
    /// ever hibernated — refused.
    pub fn validate(&self) -> Result<(), &'static str> {
        if !(1..=12).contains(&self.max_live_cards) {
            return Err("max_live_cards must be between 1 and 12");
        }
        if !(60_000..=3_600_000).contains(&self.idle_hibernate_ms) {
            return Err("idle_hibernate_ms must be between 60000 and 3600000");
        }
        if !(3_600_000..=2_592_000_000).contains(&self.cold_card_ms) {
            return Err("cold_card_ms must be between 3600000 and 2592000000");
        }
        if self.cold_card_ms <= self.idle_hibernate_ms {
            return Err("cold_card_ms must be greater than idle_hibernate_ms");
        }
        if !(64..=4096).contains(&self.memory_budget_mb) {
            return Err("memory_budget_mb must be between 64 and 4096");
        }
        Ok(())
    }
}

impl Db {
    /// Open (and create) the database next to the JSON files, then apply the migration. Opening twice is safe: the
    /// migration is a no-op when `user_version` is already `SCHEMA_VERSION`.
    pub fn open(data_dir: &Path) -> Result<Self, DbError> {
        std::fs::create_dir_all(data_dir)?;
        let path = data_dir.join(DB_FILE_NAME);
        let connection = Connection::open(&path)?;
        // WAL: a reader (the shell asking for a list) must not wait for a writer (a pin), and a crash must not lose
        // the last committed transaction. It is the point of moving to a database.
        connection.pragma_update(None, "journal_mode", "WAL")?;
        connection.pragma_update(None, "foreign_keys", "ON")?;
        let opened_at_version: i64 =
            connection.query_row("PRAGMA user_version", [], |row| row.get(0))?;
        let db = Self {
            path,
            connection,
            opened_at_version,
        };
        db.migrate()?;
        Ok(db)
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    /// The `user_version` the file reported *before* this open migrated it: which migrations ran (and, in the `0004`
    /// step, whether the cards' adoption was already behind the file).
    #[cfg_attr(not(test), allow(dead_code))]
    pub fn opened_at_version(&self) -> i64 {
        self.opened_at_version
    }

    /// The migration. Idempotent, and written so that a later version can add a step instead of rewriting this one.
    fn migrate(&self) -> Result<(), DbError> {
        if self.opened_at_version >= SCHEMA_VERSION {
            return Ok(());
        }
        if self.opened_at_version < 2 {
            self.connection.execute_batch(
                "BEGIN;
                 CREATE TABLE IF NOT EXISTS cards (
                     id                 TEXT PRIMARY KEY,
                     title              TEXT NOT NULL,
                     url                TEXT NOT NULL,
                     container          TEXT NOT NULL DEFAULT '',
                     status             TEXT NOT NULL DEFAULT 'unknown',
                     pinned             INTEGER NOT NULL DEFAULT 0,
                     notes              TEXT NOT NULL DEFAULT '',
                     preview_ref        TEXT NOT NULL DEFAULT '',
                     last_opened_at_ms  INTEGER NOT NULL DEFAULT 0,
                     page_text          TEXT NOT NULL DEFAULT '',
                     workspace          TEXT NOT NULL DEFAULT '',
                     tags               TEXT NOT NULL DEFAULT '[]'
                 );
                 CREATE INDEX IF NOT EXISTS cards_by_workspace ON cards (workspace);
                 CREATE INDEX IF NOT EXISTS cards_by_container ON cards (container);
                 CREATE TABLE IF NOT EXISTS containers (
                     id    TEXT PRIMARY KEY,
                     name  TEXT NOT NULL,
                     color TEXT NOT NULL
                 );
                 CREATE TABLE IF NOT EXISTS workspaces (
                     id   TEXT PRIMARY KEY,
                     name TEXT NOT NULL
                 );
                 CREATE TABLE IF NOT EXISTS sessions (
                     container_id   TEXT NOT NULL,
                     origin         TEXT NOT NULL,
                     state          TEXT NOT NULL,
                     last_used_at_ms INTEGER NOT NULL DEFAULT 0,
                     size_cookies   INTEGER NOT NULL DEFAULT 0,
                     size_bytes     INTEGER NOT NULL DEFAULT 0,
                     service_workers INTEGER NOT NULL DEFAULT 0,
                     PRIMARY KEY (container_id, origin)
                 );
                 PRAGMA user_version = 2;
                 COMMIT;",
            )?;
        }
        if self.opened_at_version < 4 {
            // 0004: the ledger row is what `sessions.json` always held, and the container-wide rules get a one-row
            // table. The 0002 `sessions` table is rebuilt only when it still has the *old* shape: four stores open in a
            // row before any of them writes, and a rebuild on every open would lose the rows an earlier store adopted.
            // The probe is the `policy` column, which only 0004 adds.
            let already_shaped: i64 = self.connection.query_row(
                "SELECT COUNT(*) FROM pragma_table_info('sessions') WHERE name = 'policy'",
                [],
                |row| row.get(0),
            )?;
            self.connection.execute_batch("BEGIN;")?;
            if already_shaped == 0 {
                self.connection
                    .execute_batch("DROP TABLE IF EXISTS sessions;")?;
            }
            self.connection.execute_batch(
                "CREATE TABLE IF NOT EXISTS sessions (
                     container_id        TEXT NOT NULL,
                     origin              TEXT NOT NULL,
                     state               TEXT NOT NULL,
                     policy              TEXT NOT NULL,
                     cookie_count        INTEGER NOT NULL DEFAULT 0,
                     cookie_bytes        INTEGER NOT NULL DEFAULT 0,
                     storage_bytes       INTEGER NOT NULL DEFAULT 0,
                     service_worker_count INTEGER NOT NULL DEFAULT 0,
                     last_seen_ms        INTEGER,
                     last_verified_ms    INTEGER,
                     live_ram_bytes      INTEGER,
                     keep_alive          INTEGER NOT NULL DEFAULT 0,
                     PRIMARY KEY (container_id, origin)
                 );
                 CREATE TABLE IF NOT EXISTS session_rules (
                     id                INTEGER PRIMARY KEY DEFAULT 1 CHECK (id = 1),
                     container_default TEXT NOT NULL,
                     ttl_ms            INTEGER NOT NULL,
                     wipe_on_app_lock  INTEGER NOT NULL DEFAULT 0
                 );
                 -- The per-store adoption markers live here, not in `user_version`: the four stores open one after
                 -- another and each must be able to finish its own adoption without depending on the order.
                 CREATE TABLE IF NOT EXISTS meta (
                     key   TEXT PRIMARY KEY,
                     value TEXT NOT NULL
                 );
                 PRAGMA user_version = 4;
                 COMMIT;",
            )?;
            if self.opened_at_version >= CARD_SWITCH_VERSION {
                // A file from the card switch (ADR-069) already has the cards' adoption behind it; recording that here
                // is what stops the new build from reading `cards.json` back over newer rows.
                self.mark_adopted("cards")?;
            }
        }
        if self.opened_at_version < 5 {
            // 0005: a container is a boundary, not a label (ADR-071), so it carries the proxy it reaches the network
            // through and the permissions it grants. The columns are added only when they are missing: an ALTER that
            // ran before a crash must not fail the next start with "duplicate column name".
            let already_extended: i64 = self.connection.query_row(
                "SELECT COUNT(*) FROM pragma_table_info('containers') WHERE name = 'proxy_mode'",
                [],
                |row| row.get(0),
            )?;
            if already_extended == 0 {
                self.connection.execute_batch(
                    "BEGIN;
                     ALTER TABLE containers ADD COLUMN proxy_mode    TEXT NOT NULL DEFAULT 'system';
                     ALTER TABLE containers ADD COLUMN proxy_host    TEXT NOT NULL DEFAULT '';
                     ALTER TABLE containers ADD COLUMN proxy_port    INTEGER NOT NULL DEFAULT 0;
                     ALTER TABLE containers ADD COLUMN proxy_bypass  TEXT NOT NULL DEFAULT '';
                     ALTER TABLE containers ADD COLUMN permissions   TEXT NOT NULL DEFAULT '{}';
                     PRAGMA user_version = 5;
                     COMMIT;",
                )?;
            }
        }
        if self.opened_at_version < 6 {
            // 0006: the one row of user preferences (ADR-072). One row only, enforced by `CHECK (id = 1)`; the seeded
            // values are the same three the `UserPrefs::default()` carries, so a profile that never chose anything and a
            // profile that stored the defaults are indistinguishable.
            self.connection.execute_batch(
                "BEGIN;
                 CREATE TABLE IF NOT EXISTS user_prefs (
                     id                INTEGER PRIMARY KEY DEFAULT 1 CHECK (id = 1),
                     max_live_cards    INTEGER NOT NULL DEFAULT 5,
                     idle_hibernate_ms INTEGER NOT NULL DEFAULT 600000,
                     cold_card_ms      INTEGER NOT NULL DEFAULT 86400000
                 );
                 INSERT INTO user_prefs (id, max_live_cards, idle_hibernate_ms, cold_card_ms)
                 VALUES (1, 5, 600000, 86400000)
                 ON CONFLICT (id) DO NOTHING;
                 PRAGMA user_version = 6;
                 COMMIT;",
            )?;
        }
        if self.opened_at_version < 7 {
            // 0007: the memory budget (F6). One column on the existing one-row table; added only when missing, so a
            // crash after the ALTER (or a v7 file re-labelled older) does not fail with "duplicate column name".
            let budget_column_exists: i64 = self.connection.query_row(
                "SELECT COUNT(*) FROM pragma_table_info('user_prefs') WHERE name = 'memory_budget_mb'",
                [],
                |row| row.get(0),
            )?;
            if budget_column_exists == 0 {
                self.connection.execute_batch(
                    "BEGIN;
                     ALTER TABLE user_prefs ADD COLUMN memory_budget_mb INTEGER NOT NULL DEFAULT 512;
                     PRAGMA user_version = 7;
                     COMMIT;",
                )?;
            }
        }
        if self.opened_at_version < 8 {
            // 0008: the per-card working set (F6). A card that was never frozen reports 0, which is exactly
            // "not measured" — no NULL needed. Added only when missing (the 0007 idiom).
            let memory_column_exists: i64 = self.connection.query_row(
                "SELECT COUNT(*) FROM pragma_table_info('cards') WHERE name = 'memory_bytes'",
                [],
                |row| row.get(0),
            )?;
            if memory_column_exists == 0 {
                self.connection.execute_batch(
                    "BEGIN;
                     ALTER TABLE cards ADD COLUMN memory_bytes INTEGER NOT NULL DEFAULT 0;
                     PRAGMA user_version = 8;
                     COMMIT;",
                )?;
            }
        }
        if self.opened_at_version < 9 {
            // 0009: the resource profile (F6). One text column on the one-row preferences table; added only when
            // missing (the 0007 idiom).
            let profile_column_exists: i64 = self.connection.query_row(
                "SELECT COUNT(*) FROM pragma_table_info('user_prefs') WHERE name = 'resource_profile'",
                [],
                |row| row.get(0),
            )?;
            if profile_column_exists == 0 {
                self.connection.execute_batch(
                    "BEGIN;
                     ALTER TABLE user_prefs ADD COLUMN resource_profile TEXT NOT NULL DEFAULT 'kiegyensulyozott';
                     PRAGMA user_version = 9;
                     COMMIT;",
                )?;
            }
        }
        if self.opened_at_version < 10 {
            // 0010: the resource profile values now follow ADR-020's snake_case names (`ProfileKind`), not the
            // Hungarian names the 0009 column shipped with. Rewrite the one row, then bump.
            self.connection.execute_batch(
                "BEGIN;
                 UPDATE user_prefs SET resource_profile = 'fast' WHERE resource_profile = 'gyors';
                 UPDATE user_prefs SET resource_profile = 'balanced' WHERE resource_profile = 'kiegyensulyozott';
                 UPDATE user_prefs SET resource_profile = 'saver' WHERE resource_profile = 'takarekos';
                 UPDATE user_prefs SET resource_profile = 'manual' WHERE resource_profile = 'kezi';
                 PRAGMA user_version = 10;
                 COMMIT;",
            )?;
        }
        // The version is written last, monotonically: a step that crashed half-way leaves an older number behind, so the
        // next start retries it instead of believing a migration finished because a table happened to exist.
        self.connection
            .execute_batch(&format!("PRAGMA user_version = {SCHEMA_VERSION};"))?;
        Ok(())
    }

    /// Has this store's pre-switch document already been adopted? The marker is written after the adoption succeeded, so
    /// a crash (or an unreadable document) leaves the question open and the next start retries instead of serving a
    /// half-adopted profile (ADR-069/070).
    pub fn was_adopted(&self, store: &str) -> Result<bool, DbError> {
        let key = format!("adopted:{store}");
        let value: Option<String> = self
            .connection
            .query_row(
                "SELECT value FROM meta WHERE key = ?1",
                params![key],
                |row| row.get(0),
            )
            .optional()?;
        Ok(value.as_deref() == Some("1"))
    }

    /// Record that `store` adopted its pre-switch document. Idempotent.
    pub fn mark_adopted(&self, store: &str) -> Result<(), DbError> {
        let key = format!("adopted:{store}");
        self.connection.execute(
            "INSERT INTO meta (key, value) VALUES (?1, '1')
             ON CONFLICT (key) DO UPDATE SET value = excluded.value",
            params![key],
        )?;
        Ok(())
    }

    /// The version the file reports, for the `db_status` reply (and for the test that proves the migration is once).
    pub fn schema_version(&self) -> Result<i64, DbError> {
        Ok(self
            .connection
            .query_row("PRAGMA user_version", [], |row| row.get(0))?)
    }

    /// The one row of user preferences, or the factory defaults when nothing was ever chosen.
    pub fn user_prefs(&self) -> Result<UserPrefs, DbError> {
        let row: Option<UserPrefs> = self.connection.query_row(
            "SELECT max_live_cards, idle_hibernate_ms, cold_card_ms, memory_budget_mb, resource_profile
             FROM user_prefs WHERE id = 1",
            [],
            |row| {
                Ok(UserPrefs {
                    max_live_cards: row.get::<_, i64>(0)? as u32,
                    idle_hibernate_ms: row.get::<_, i64>(1)? as u64,
                    cold_card_ms: row.get::<_, i64>(2)? as u64,
                    memory_budget_mb: row.get::<_, i64>(3)? as u32,
                    resource_profile: profile_kind_from_str(&row.get::<_, String>(4)?),
                })
            },
        ).optional()?;
        Ok(row.unwrap_or_default())
    }

    /// Replace the one row of user preferences. The caller must have validated first.
    pub fn set_user_prefs(&self, prefs: &UserPrefs) -> Result<(), DbError> {
        self.connection.execute(
            "INSERT INTO user_prefs (id, max_live_cards, idle_hibernate_ms, cold_card_ms, memory_budget_mb, resource_profile)
             VALUES (1, ?1, ?2, ?3, ?4, ?5)
             ON CONFLICT (id) DO UPDATE SET
                 max_live_cards = excluded.max_live_cards,
                 idle_hibernate_ms = excluded.idle_hibernate_ms,
                 cold_card_ms = excluded.cold_card_ms,
                 memory_budget_mb = excluded.memory_budget_mb,
                 resource_profile = excluded.resource_profile",
            params![
                prefs.max_live_cards as i64,
                prefs.idle_hibernate_ms as i64,
                prefs.cold_card_ms as i64,
                prefs.memory_budget_mb as i64,
                profile_kind_to_str(prefs.resource_profile),
            ],
        )?;
        Ok(())
    }

    /// Write a consistent copy of this database to `path` (ADR-069): the one-time adoption lets `cards.json` win, and
    /// a row it discards must stay recoverable instead of vanishing with the rewrite.
    ///
    /// `VACUUM INTO` is the only copy that is safe while WAL is on: copying the file bytes would miss a transaction that
    /// is still only in the `-wal` file. It refuses an existing target, so the target is removed first.
    pub fn snapshot_to(&self, path: &Path) -> Result<(), DbError> {
        if path.exists() {
            std::fs::remove_file(path)?;
        }
        self.connection
            .execute("VACUUM INTO ?1", params![path.to_string_lossy()])?;
        Ok(())
    }

    /// Write one card, replacing the row with the same id.
    pub fn upsert_card(&self, card: &Card) -> Result<(), DbError> {
        upsert_card_row(&self.connection, card)?;
        Ok(())
    }

    /// Write the card's last-known working set in bytes (F6, per-card breakdown). A single-column update, so a
    /// capture never touches the note or title the user just changed.
    pub fn set_card_memory(&self, card_id: &str, bytes: i64) -> Result<(), DbError> {
        self.connection.execute(
            "UPDATE cards SET memory_bytes = ?2 WHERE id = ?1",
            params![card_id, bytes],
        )?;
        Ok(())
    }

    /// Replace the whole card table in one transaction — the import path (`profile_import`, ADR-059) and the first
    /// write of a fresh or migrated profile. A reader never sees half a profile, and a failed insert leaves the old
    /// list intact instead of half of a new one.
    pub fn replace_all_cards(&mut self, cards: &[Card]) -> Result<(), DbError> {
        let transaction = self.connection.transaction()?;
        transaction.execute("DELETE FROM cards", [])?;
        for card in cards {
            upsert_card_row(&transaction, card)?;
        }
        transaction.commit()?;
        Ok(())
    }

    /// One card by id. The store's writes (a pin, a page capture) read the row, change one field and write it back,
    /// so a capture never overwrites the note the user just typed.
    pub fn card(&self, card_id: &str) -> Result<Option<Card>, DbError> {
        let row = self
            .connection
            .query_row(
                "SELECT id, title, url, container, status, pinned, notes, preview_ref,
                        last_opened_at_ms, page_text, workspace, tags, memory_bytes
                 FROM cards WHERE id = ?1",
                params![card_id],
                |row| {
                    let pinned: i64 = row.get(5)?;
                    let opened: i64 = row.get(8)?;
                    let tags: String = row.get(11)?;
                    Ok(Card {
                        id: row.get(0)?,
                        title: row.get(1)?,
                        url: row.get(2)?,
                        container: row.get(3)?,
                        status: row.get(4)?,
                        pinned: pinned != 0,
                        notes: row.get(6)?,
                        preview_ref: row.get(7)?,
                        last_opened_at_ms: opened as u64,
                        page_text: row.get(9)?,
                        workspace: row.get(10)?,
                        tags: serde_json::from_str(&tags).unwrap_or_default(),
                        memory_bytes: row.get(12)?,
                    })
                },
            )
            .optional()?;
        Ok(row)
    }

    /// Drop one card (`card_delete`, ADR-061). A delete that left the row behind would show the card again on the
    /// next list — the exact bug the old parity check found when the mirror was upsert-only.
    pub fn delete_card(&self, card_id: &str) -> Result<bool, DbError> {
        let removed = self
            .connection
            .execute("DELETE FROM cards WHERE id = ?1", params![card_id])?;
        Ok(removed > 0)
    }

    /// Move every card from one container to another in **one statement** (`container_delete` with `move_to`,
    /// ADR-054/ADR-066). The old store rewrote the whole JSON document and the old mirror had to walk the moved set
    /// by hand; here the database is what changes, so there is no second copy left to get out of step.
    pub fn move_container(&self, from: &str, to: &str) -> Result<usize, DbError> {
        let moved = self.connection.execute(
            "UPDATE cards SET container = ?2 WHERE container = ?1",
            params![from, to],
        )?;
        Ok(moved)
    }

    /// Every card the database holds, in the grid's order (ADR-067): newest-opened first, then title. The JSON store
    /// sorted in Rust; a database read without `ORDER BY` would return insertion order instead, and the grid would
    /// quietly stop reordering when a card is opened.
    pub fn all_cards(&self) -> Result<Vec<Card>, DbError> {
        let mut statement = self.connection.prepare(
            "SELECT id, title, url, container, status, pinned, notes, preview_ref,
                    last_opened_at_ms, page_text, workspace, tags, memory_bytes
             FROM cards
             ORDER BY last_opened_at_ms DESC, LOWER(title) ASC",
        )?;
        let rows = statement.query_map([], |row| {
            let pinned: i64 = row.get(5)?;
            let opened: i64 = row.get(8)?;
            let tags: String = row.get(11)?;
            Ok(Card {
                id: row.get(0)?,
                title: row.get(1)?,
                url: row.get(2)?,
                container: row.get(3)?,
                status: row.get(4)?,
                pinned: pinned != 0,
                notes: row.get(6)?,
                preview_ref: row.get(7)?,
                last_opened_at_ms: opened as u64,
                page_text: row.get(9)?,
                workspace: row.get(10)?,
                tags: serde_json::from_str(&tags).unwrap_or_default(),
                memory_bytes: row.get(12)?,
            })
        })?;
        let mut cards = Vec::new();
        for card in rows {
            cards.push(card?);
        }
        Ok(cards)
    }

    /// How many cards say they are in one workspace — the first real query the database does better than a JSON walk.
    pub fn cards_in_workspace(&self, workspace: &str) -> Result<i64, DbError> {
        Ok(self.connection.query_row(
            "SELECT COUNT(*) FROM cards WHERE workspace = ?1",
            params![workspace],
            |row| row.get(0),
        )?)
    }

    /// Every container, in the order it was inserted — the legend's order (ADR-070). The old JSON list kept that order,
    /// and a `SELECT` without `ORDER BY` on a table with a TEXT primary key happens to return rowid order, which is not
    /// something a UI order should rest on; `rowid` is named here so a future index cannot reorder the chips.
    pub fn all_containers(&self) -> Result<Vec<Container>, DbError> {
        let mut statement = self.connection.prepare(
            "SELECT id, name, color, proxy_mode, proxy_host, proxy_port, proxy_bypass, permissions
             FROM containers ORDER BY rowid",
        )?;
        let rows = statement.query_map([], |row| {
            let permissions_text: String = row.get(7)?;
            Ok(Container {
                id: row.get(0)?,
                name: row.get(1)?,
                color: row.get(2)?,
                proxy_mode: row.get(3)?,
                proxy_host: row.get(4)?,
                proxy_port: row.get::<_, i64>(5)? as u16,
                proxy_bypass: row.get(6)?,
                // An unreadable policy column is reported as empty instead of failing the whole list: the grants are
                // additive, and refusing to show the containers because one policy blob is corrupt would be worse than
                // showing them without the grants (the engine then denies what it does not know).
                permissions: serde_json::from_str(&permissions_text).unwrap_or_default(),
            })
        })?;
        let mut containers = Vec::new();
        for container in rows {
            containers.push(container?);
        }
        Ok(containers)
    }

    pub fn upsert_container(&self, container: &Container) -> Result<(), DbError> {
        self.connection.execute(
            "INSERT INTO containers (id, name, color, proxy_mode, proxy_host, proxy_port, proxy_bypass,
                                     permissions)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)
             ON CONFLICT (id) DO UPDATE SET
                 name = excluded.name, color = excluded.color, proxy_mode = excluded.proxy_mode,
                 proxy_host = excluded.proxy_host, proxy_port = excluded.proxy_port,
                 proxy_bypass = excluded.proxy_bypass, permissions = excluded.permissions",
            params![
                container.id,
                container.name,
                container.color,
                container.proxy_mode,
                container.proxy_host,
                i64::from(container.proxy_port),
                container.proxy_bypass,
                container_permissions_text(container)
            ],
        )?;
        Ok(())
    }

    pub fn delete_container(&self, container_id: &str) -> Result<bool, DbError> {
        let removed = self.connection.execute(
            "DELETE FROM containers WHERE id = ?1",
            params![container_id],
        )?;
        Ok(removed > 0)
    }

    /// Replace the whole container table in one transaction, for an adoption or a profile import.
    ///
    /// The insertion order is the legend's order, so the rows go in the list's order.
    pub fn replace_containers(&mut self, containers: &[Container]) -> Result<(), DbError> {
        let transaction = self.connection.transaction()?;
        transaction.execute("DELETE FROM containers", [])?;
        for container in containers {
            transaction.execute(
                "INSERT INTO containers (id, name, color, proxy_mode, proxy_host, proxy_port, proxy_bypass,
                                         permissions)
                 VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
                params![
                    container.id,
                    container.name,
                    container.color,
                    container.proxy_mode,
                    container.proxy_host,
                    i64::from(container.proxy_port),
                    container.proxy_bypass,
                    container_permissions_text(container)
                ],
            )?;
        }
        transaction.commit()?;
        Ok(())
    }

    /// Every workspace, in insertion (tab-row) order — the same rule as the containers.
    pub fn all_workspaces(&self) -> Result<Vec<Workspace>, DbError> {
        let mut statement = self
            .connection
            .prepare("SELECT id, name FROM workspaces ORDER BY rowid")?;
        let rows = statement.query_map([], |row| {
            Ok(Workspace {
                id: row.get(0)?,
                name: row.get(1)?,
            })
        })?;
        let mut workspaces = Vec::new();
        for workspace in rows {
            workspaces.push(workspace?);
        }
        Ok(workspaces)
    }

    pub fn upsert_workspace(&self, workspace: &Workspace) -> Result<(), DbError> {
        self.connection.execute(
            "INSERT INTO workspaces (id, name) VALUES (?1, ?2)
             ON CONFLICT (id) DO UPDATE SET name = excluded.name",
            params![workspace.id, workspace.name],
        )?;
        Ok(())
    }

    pub fn delete_workspace(&self, workspace_id: &str) -> Result<bool, DbError> {
        let removed = self.connection.execute(
            "DELETE FROM workspaces WHERE id = ?1",
            params![workspace_id],
        )?;
        Ok(removed > 0)
    }

    /// Replace the whole workspace table in one transaction (adoption, profile import), keeping the tab order.
    pub fn replace_workspaces(&mut self, workspaces: &[Workspace]) -> Result<(), DbError> {
        let transaction = self.connection.transaction()?;
        transaction.execute("DELETE FROM workspaces", [])?;
        for workspace in workspaces {
            transaction.execute(
                "INSERT INTO workspaces (id, name) VALUES (?1, ?2)",
                params![workspace.id, workspace.name],
            )?;
        }
        transaction.commit()?;
        Ok(())
    }

    /// The ledger rows, ordered by origin — the panel's and the reply's stable order (the JSON ledger sorted the same
    /// way). A row whose `state` or `policy` text the core does not know is refused loudly instead of being guessed at:
    /// a ledger that lies is worse than a ledger that is missing a row (ADR-033).
    pub fn all_sessions(&self) -> Result<Vec<SessionLedgerEntry>, DbError> {
        let mut statement = self.connection.prepare(
            "SELECT container_id, origin, state, policy, cookie_count, cookie_bytes, storage_bytes,
                    service_worker_count, last_seen_ms, last_verified_ms, live_ram_bytes, keep_alive
             FROM sessions ORDER BY origin",
        )?;
        let rows = statement.query_map([], |row| {
            let state_text: String = row.get(2)?;
            let policy_text: String = row.get(3)?;
            let state = LoginState::from_db(&state_text).ok_or_else(|| {
                rusqlite::Error::FromSqlConversionFailure(
                    2,
                    rusqlite::types::Type::Text,
                    Box::new(std::io::Error::other(format!(
                        "unknown session state `{state_text}`"
                    ))),
                )
            })?;
            let policy = SessionPolicy::from_db(&policy_text).ok_or_else(|| {
                rusqlite::Error::FromSqlConversionFailure(
                    3,
                    rusqlite::types::Type::Text,
                    Box::new(std::io::Error::other(format!(
                        "unknown session policy `{policy_text}`"
                    ))),
                )
            })?;
            Ok(SessionLedgerEntry {
                container_id: row.get(0)?,
                origin: row.get(1)?,
                state,
                policy,
                cookie_count: row.get::<_, i64>(4)? as u32,
                cookie_bytes: row.get::<_, i64>(5)? as u64,
                storage_bytes: row.get::<_, i64>(6)? as u64,
                service_worker_count: row.get::<_, i64>(7)? as u32,
                last_seen_ms: row.get::<_, Option<i64>>(8)?.map(|value| value as u64),
                last_verified_ms: row.get::<_, Option<i64>>(9)?.map(|value| value as u64),
                live_ram_bytes: row.get::<_, Option<i64>>(10)?.map(|value| value as u64),
                keep_alive: row.get::<_, i64>(11)? != 0,
            })
        })?;
        let mut sessions = Vec::new();
        for session in rows {
            sessions.push(session?);
        }
        Ok(sessions)
    }

    pub fn upsert_session(&self, entry: &SessionLedgerEntry) -> Result<(), DbError> {
        upsert_session_row(&self.connection, entry)?;
        Ok(())
    }

    pub fn delete_session(&self, container_id: &str, origin: &str) -> Result<bool, DbError> {
        let removed = self.connection.execute(
            "DELETE FROM sessions WHERE container_id = ?1 AND origin = ?2",
            params![container_id, origin],
        )?;
        Ok(removed > 0)
    }

    /// The container-wide rules, or `None` while the profile has not stored any (the caller uses the policy module's
    /// defaults). `None` is not an empty policy: it says "nothing was ever chosen".
    pub fn session_rules(&self) -> Result<Option<SessionRules>, DbError> {
        let row = self
            .connection
            .query_row(
                "SELECT container_default, ttl_ms, wipe_on_app_lock FROM session_rules WHERE id = 1",
                [],
                |row| {
                    let default_text: String = row.get(0)?;
                    Ok((
                        default_text,
                        row.get::<_, i64>(1)?,
                        row.get::<_, i64>(2)? != 0,
                    ))
                },
            )
            .optional()?;
        Ok(match row {
            Some((default_text, ttl_ms, wipe_on_app_lock)) => {
                let container_default = SessionPolicy::from_db(&default_text).ok_or_else(|| {
                    rusqlite::Error::FromSqlConversionFailure(
                        0,
                        rusqlite::types::Type::Text,
                        Box::new(std::io::Error::other(format!(
                            "unknown session policy `{default_text}`"
                        ))),
                    )
                })?;
                Some(SessionRules {
                    container_default,
                    ttl_ms: ttl_ms as u64,
                    wipe_on_app_lock,
                })
            }
            None => None,
        })
    }

    pub fn set_session_rules(&self, rules: &SessionRules) -> Result<(), DbError> {
        self.connection.execute(
            "INSERT INTO session_rules (id, container_default, ttl_ms, wipe_on_app_lock)
             VALUES (1, ?1, ?2, ?3)
             ON CONFLICT (id) DO UPDATE SET
                 container_default = excluded.container_default, ttl_ms = excluded.ttl_ms,
                 wipe_on_app_lock = excluded.wipe_on_app_lock",
            params![
                rules.container_default.as_db(),
                rules.ttl_ms as i64,
                i64::from(rules.wipe_on_app_lock)
            ],
        )?;
        Ok(())
    }

    /// Replace the whole ledger — rows and rules together, because an imported profile's cleanup policy is part of the
    /// profile (ADR-059) — in one transaction.
    pub fn replace_sessions(
        &mut self,
        entries: &[SessionLedgerEntry],
        rules: &SessionRules,
    ) -> Result<(), DbError> {
        let transaction = self.connection.transaction()?;
        transaction.execute("DELETE FROM sessions", [])?;
        for entry in entries {
            upsert_session_row(&transaction, entry)?;
        }
        transaction.execute(
            "INSERT INTO session_rules (id, container_default, ttl_ms, wipe_on_app_lock)
             VALUES (1, ?1, ?2, ?3)
             ON CONFLICT (id) DO UPDATE SET
                 container_default = excluded.container_default, ttl_ms = excluded.ttl_ms,
                 wipe_on_app_lock = excluded.wipe_on_app_lock",
            params![
                rules.container_default.as_db(),
                rules.ttl_ms as i64,
                i64::from(rules.wipe_on_app_lock)
            ],
        )?;
        transaction.commit()?;
        Ok(())
    }

    /// The row counts, for `db_status`: the shell can show what the database actually holds without guessing.
    pub fn counts(&self) -> Result<(i64, i64, i64, i64), DbError> {
        let one = |sql: &str| -> Result<i64, DbError> {
            Ok(self.connection.query_row(sql, [], |row| row.get(0))?)
        };
        Ok((
            one("SELECT COUNT(*) FROM cards")?,
            one("SELECT COUNT(*) FROM containers")?,
            one("SELECT COUNT(*) FROM workspaces")?,
            one("SELECT COUNT(*) FROM sessions")?,
        ))
    }
}

/// The permission policy as one JSON column (`{}` when the container grants nothing). One text column, the same shape
/// the tags use for the cards: the rules are read and written whole, never queried one by one.
fn container_permissions_text(container: &Container) -> String {
    serde_json::to_string(&container.permissions).unwrap_or_else(|_| "{}".to_string())
}

/// One `INSERT … ON CONFLICT` used by both the single-card write and the whole-list replace, so the two can never
/// write different columns.
fn upsert_card_row(connection: &Connection, card: &Card) -> Result<(), rusqlite::Error> {
    let tags = serde_json::to_string(&card.tags).unwrap_or_else(|_| "[]".to_string());
    connection.execute(
        "INSERT INTO cards (id, title, url, container, status, pinned, notes, preview_ref,
                            last_opened_at_ms, page_text, workspace, tags, memory_bytes)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13)
         ON CONFLICT (id) DO UPDATE SET
             title = excluded.title, url = excluded.url, container = excluded.container,
             status = excluded.status, pinned = excluded.pinned, notes = excluded.notes,
             preview_ref = excluded.preview_ref, last_opened_at_ms = excluded.last_opened_at_ms,
             page_text = excluded.page_text, workspace = excluded.workspace, tags = excluded.tags,
             memory_bytes = excluded.memory_bytes",
        params![
            card.id,
            card.title,
            card.url,
            card.container,
            card.status,
            i64::from(card.pinned),
            card.notes,
            card.preview_ref,
            card.last_opened_at_ms as i64,
            card.page_text,
            card.workspace,
            tags,
            card.memory_bytes
        ],
    )?;
    Ok(())
}

/// One `INSERT … ON CONFLICT` used by both the single-session write and the whole-ledger replace.
fn upsert_session_row(
    connection: &Connection,
    entry: &SessionLedgerEntry,
) -> Result<(), rusqlite::Error> {
    connection.execute(
        "INSERT INTO sessions (container_id, origin, state, policy, cookie_count, cookie_bytes,
                               storage_bytes, service_worker_count, last_seen_ms, last_verified_ms,
                               live_ram_bytes, keep_alive)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)
         ON CONFLICT (container_id, origin) DO UPDATE SET
             state = excluded.state, policy = excluded.policy, cookie_count = excluded.cookie_count,
             cookie_bytes = excluded.cookie_bytes, storage_bytes = excluded.storage_bytes,
             service_worker_count = excluded.service_worker_count, last_seen_ms = excluded.last_seen_ms,
             last_verified_ms = excluded.last_verified_ms, live_ram_bytes = excluded.live_ram_bytes,
             keep_alive = excluded.keep_alive",
        params![
            entry.container_id,
            entry.origin,
            entry.state.as_db(),
            entry.policy.as_db(),
            i64::from(entry.cookie_count),
            entry.cookie_bytes as i64,
            entry.storage_bytes as i64,
            i64::from(entry.service_worker_count),
            entry.last_seen_ms.map(|value| value as i64),
            entry.last_verified_ms.map(|value| value as i64),
            entry.live_ram_bytes.map(|value| value as i64),
            i64::from(entry.keep_alive)
        ],
    )?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_dir(name: &str) -> PathBuf {
        let stamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .expect("clock")
            .as_nanos();
        let dir = std::env::temp_dir().join(format!("antikolt-db-{name}-{stamp}"));
        std::fs::create_dir_all(&dir).expect("temp dir");
        dir
    }

    fn sample_card() -> Card {
        Card {
            id: "01DBCARD0000000000000001".to_string(),
            title: "Adatbázis-próba".to_string(),
            url: "https://pelda.hu/cikk".to_string(),
            container: "c-work".to_string(),
            status: "unknown".to_string(),
            pinned: true,
            notes: "jegyzet".to_string(),
            preview_ref: "01PREVIEW".to_string(),
            last_opened_at_ms: 1_700_000_000_123,
            page_text: "a lap szövege".to_string(),
            workspace: "w-work".to_string(),
            tags: vec!["próba".to_string(), "adatbázis".to_string()],
            memory_bytes: 112_525_312,
        }
    }

    /// A database file as an older build wrote it: the tables of the schema version named in the argument, and nothing
    /// from the later migrations.
    fn write_pre_switch_file(dir: &Path, version: i64) {
        let connection = Connection::open(dir.join(DB_FILE_NAME)).expect("raw open");
        connection
            .execute_batch(&format!(
                "CREATE TABLE cards (
                     id TEXT PRIMARY KEY, title TEXT NOT NULL, url TEXT NOT NULL,
                     container TEXT NOT NULL DEFAULT '', status TEXT NOT NULL DEFAULT 'unknown',
                     pinned INTEGER NOT NULL DEFAULT 0, notes TEXT NOT NULL DEFAULT '',
                     preview_ref TEXT NOT NULL DEFAULT '', last_opened_at_ms INTEGER NOT NULL DEFAULT 0,
                     page_text TEXT NOT NULL DEFAULT '', workspace TEXT NOT NULL DEFAULT '',
                     tags TEXT NOT NULL DEFAULT '[]');
                 CREATE TABLE containers (id TEXT PRIMARY KEY, name TEXT NOT NULL, color TEXT NOT NULL);
                 CREATE TABLE workspaces (id TEXT PRIMARY KEY, name TEXT NOT NULL);
                 CREATE TABLE sessions (
                     container_id    TEXT NOT NULL,
                     origin          TEXT NOT NULL,
                     state           TEXT NOT NULL,
                     last_used_at_ms INTEGER NOT NULL DEFAULT 0,
                     size_cookies    INTEGER NOT NULL DEFAULT 0,
                     size_bytes      INTEGER NOT NULL DEFAULT 0,
                     service_workers INTEGER NOT NULL DEFAULT 0,
                     PRIMARY KEY (container_id, origin)
                 );
                 PRAGMA user_version = {version};"
            ))
            .expect("write a pre-switch file");
    }

    #[test]
    fn a_fresh_database_migrates_to_the_current_schema_and_remembers_its_adoptions() {
        let dir = temp_dir("migrate");
        let db = Db::open(&dir).expect("open");
        assert!(db.path().exists());
        let (cards, containers, workspaces, sessions) = db.counts().expect("counts");
        assert_eq!((cards, containers, workspaces, sessions), (0, 0, 0, 0));
        // A brand-new file reports version 0 on open (it was created now) and is at the current schema afterwards.
        assert_eq!(db.opened_at_version(), 0);
        assert_eq!(db.schema_version().expect("version"), SCHEMA_VERSION);
        // Every store still has to decide where its first rows come from: nothing is marked as adopted yet.
        for store in ["cards", "containers", "workspaces", "sessions"] {
            assert!(!db.was_adopted(store).expect("adopted?"), "{store}");
        }
        db.mark_adopted("cards").expect("mark the cards");
        assert!(db.was_adopted("cards").expect("adopted?"));
        assert!(!db.was_adopted("containers").expect("adopted?"));
        drop(db);

        // Opening again must not run the migration twice or lose the version or the markers.
        let again = Db::open(&dir).expect("reopen");
        assert_eq!(again.schema_version().expect("version"), SCHEMA_VERSION);
        assert!(again.was_adopted("cards").expect("adopted?"));
        assert!(
            !again.was_adopted("containers").expect("adopted?"),
            "an adoption marker is per store"
        );
        std::fs::remove_dir_all(dir).ok();
    }

    #[test]
    fn a_v2_file_has_no_adoption_markers_and_a_v3_file_already_adopted_the_cards() {
        // A v2 file predates both switches: every store must adopt its own JSON document.
        let dir = temp_dir("v2");
        write_pre_switch_file(&dir, 2);
        let db = Db::open(&dir).expect("open");
        assert_eq!(db.opened_at_version(), 2);
        assert_eq!(db.schema_version().expect("version"), SCHEMA_VERSION);
        assert!(
            !db.was_adopted("cards").expect("adopted?"),
            "cards were still a mirror at v2, so the JSON document wins"
        );
        drop(db);

        // A v3 file (the card switch, ADR-069) already adopted the cards: the marker must say so, or the new build
        // would read `cards.json` back over newer rows.
        let dir = temp_dir("v3");
        write_pre_switch_file(&dir, 3);
        let db = Db::open(&dir).expect("open");
        assert_eq!(db.opened_at_version(), 3);
        assert!(
            db.was_adopted("cards").expect("adopted?"),
            "the migration records that v3 already switched the cards"
        );
        assert!(!db.was_adopted("containers").expect("adopted?"));
        assert!(!db.was_adopted("workspaces").expect("adopted?"));
        assert!(!db.was_adopted("sessions").expect("adopted?"));
        assert_eq!(db.schema_version().expect("version"), SCHEMA_VERSION);
        std::fs::remove_dir_all(dir).ok();
    }

    #[test]
    fn a_card_round_trips_with_every_field_the_json_store_has() {
        let dir = temp_dir("roundtrip");
        let db = Db::open(&dir).expect("open");
        let card = sample_card();
        db.upsert_card(&card).expect("write");
        let read = db.card(&card.id).expect("read").expect("row");
        assert_eq!(read, card, "every field survives the round trip");
        assert_eq!(db.cards_in_workspace("w-work").expect("count"), 1);
        assert_eq!(db.cards_in_workspace("w-home").expect("count"), 0);

        // A second write is an update, not a duplicate: the id is the primary key.
        let mut changed = card.clone();
        changed.title = "Átnevezve".to_string();
        db.upsert_card(&changed).expect("update");
        let (cards, _, _, _) = db.counts().expect("counts");
        assert_eq!(cards, 1);
        assert_eq!(
            db.card(&card.id).expect("read").expect("row").title,
            "Átnevezve"
        );
        std::fs::remove_dir_all(dir).ok();
    }

    #[test]
    fn the_database_returns_the_stores_order_not_the_insertion_order() {
        // ADR-067: the grid must keep reordering when a card is opened. The JSON store sorted in Rust; the database
        // has to sort in SQL, and this test fails if someone removes the ORDER BY.
        let dir = temp_dir("order");
        let db = Db::open(&dir).expect("open");
        let mut old = sample_card();
        old.id = "01OLD".to_string();
        old.title = "Régi".to_string();
        old.last_opened_at_ms = 1;
        let mut new = sample_card();
        new.id = "01NEW".to_string();
        new.title = "Új".to_string();
        new.last_opened_at_ms = 2_000_000_000_000;
        // Written oldest-first, so insertion order and the store order disagree.
        db.upsert_card(&old).expect("old");
        db.upsert_card(&new).expect("new");
        let listed = db.all_cards().expect("list");
        assert_eq!(listed.first().map(|card| card.id.as_str()), Some("01NEW"));
        assert_eq!(listed.last().map(|card| card.id.as_str()), Some("01OLD"));
        std::fs::remove_dir_all(dir).ok();
    }

    #[test]
    fn moving_a_container_is_one_statement_that_touches_only_its_cards() {
        let dir = temp_dir("move");
        let mut db = Db::open(&dir).expect("open");
        let mut first = sample_card();
        first.id = "01A".to_string();
        first.container = "c-from".to_string();
        let mut second = sample_card();
        second.id = "01B".to_string();
        second.container = "c-from".to_string();
        let mut other = sample_card();
        other.id = "01C".to_string();
        other.container = "c-stay".to_string();
        db.replace_all_cards(&[first, second, other]).expect("seed");

        let moved = db.move_container("c-from", "c-to").expect("move");
        assert_eq!(moved, 2, "both cards of the source container moved");
        let listed = db.all_cards().expect("list");
        let in_to = listed
            .iter()
            .filter(|card| card.container == "c-to")
            .count();
        let in_stay = listed
            .iter()
            .filter(|card| card.container == "c-stay")
            .count();
        assert_eq!((in_to, in_stay), (2, 1), "nothing outside the move changed");
        assert_eq!(
            db.move_container("c-nobody", "c-to").expect("no-op"),
            0,
            "moving out of an unused container is a no-op, not an error"
        );
        std::fs::remove_dir_all(dir).ok();
    }

    #[test]
    fn replacing_the_whole_list_drops_the_rows_that_are_gone() {
        // The import path (ADR-059): a replace must not leave a deleted card behind. An upsert-only writer would,
        // and the old parity check found exactly that when the mirror was upsert-only.
        let dir = temp_dir("replace");
        let mut db = Db::open(&dir).expect("open");
        let mut first = sample_card();
        first.id = "01KEEP".to_string();
        let mut second = sample_card();
        second.id = "01DROP".to_string();
        db.replace_all_cards(&[first.clone(), second])
            .expect("seed");
        db.replace_all_cards(&[first]).expect("replace");
        let listed = db.all_cards().expect("list");
        assert_eq!(listed.len(), 1);
        assert_eq!(listed[0].id, "01KEEP");
        std::fs::remove_dir_all(dir).ok();
    }

    #[test]
    fn deleting_a_card_removes_exactly_that_row() {
        let dir = temp_dir("delete");
        let db = Db::open(&dir).expect("open");
        db.upsert_card(&sample_card()).expect("write");
        assert!(db.delete_card(&sample_card().id).expect("delete"));
        assert!(!db.delete_card("nincs-ilyen").expect("delete twice"));
        assert_eq!(db.all_cards().expect("list").len(), 0);
        std::fs::remove_dir_all(dir).ok();
    }

    fn sample_container(id: &str) -> Container {
        Container {
            id: id.to_string(),
            name: format!("Konténer {id}"),
            color: "#123456".to_string(),
            proxy_mode: "system".to_string(),
            proxy_host: String::new(),
            proxy_port: 0,
            proxy_bypass: String::new(),
            permissions: std::collections::BTreeMap::new(),
        }
    }

    fn sample_workspace(id: &str) -> Workspace {
        Workspace {
            id: id.to_string(),
            name: format!("Tér {id}"),
        }
    }

    fn sample_session(origin: &str) -> SessionLedgerEntry {
        SessionLedgerEntry {
            container_id: "c-private".to_string(),
            origin: origin.to_string(),
            state: LoginState::Expired,
            policy: SessionPolicy::Sweep,
            cookie_count: 12,
            cookie_bytes: 4096,
            storage_bytes: 65536,
            service_worker_count: 1,
            last_seen_ms: Some(1_700_000_000_000),
            last_verified_ms: None,
            live_ram_bytes: Some(48_000_000),
            keep_alive: false,
        }
    }

    #[test]
    fn containers_round_trip_in_legend_order_and_a_replace_drops_rows() {
        let dir = temp_dir("containers");
        let mut db = Db::open(&dir).expect("open");
        let first = sample_container("c-egy");
        let second = sample_container("c-ketto");
        db.replace_containers(&[first.clone(), second.clone()])
            .expect("seed");
        // The legend order is the insertion order, not the alphabet: the page draws the chips in this order.
        assert_eq!(
            db.all_containers().expect("list"),
            vec![first.clone(), second.clone()]
        );

        let mut renamed = first.clone();
        renamed.name = "Átnevezve".to_string();
        renamed.color = "#ABCDEF".to_string();
        db.upsert_container(&renamed).expect("rename");
        assert_eq!(
            db.all_containers().expect("list")[0].name,
            "Átnevezve",
            "an upsert replaces the row with the same id"
        );
        assert_eq!(db.all_containers().expect("list").len(), 2);

        assert!(db.delete_container("c-ketto").expect("delete"));
        assert!(!db.delete_container("c-ketto").expect("delete again"));
        assert_eq!(
            db.all_containers().expect("list"),
            vec![renamed.clone()],
            "a deleted container leaves the table"
        );

        // A replace is a replace, not an append: rows that are gone must not survive it.
        db.replace_containers(std::slice::from_ref(&second))
            .expect("replace");
        assert_eq!(db.all_containers().expect("list"), vec![second]);
        std::fs::remove_dir_all(dir).ok();
    }

    #[test]
    fn workspaces_round_trip_in_tab_order_and_a_replace_drops_rows() {
        let dir = temp_dir("workspaces");
        let mut db = Db::open(&dir).expect("open");
        let home = sample_workspace("w-home");
        let work = sample_workspace("w-work");
        db.replace_workspaces(&[home.clone(), work.clone()])
            .expect("seed");
        assert_eq!(
            db.all_workspaces().expect("list"),
            vec![home.clone(), work.clone()]
        );

        let mut renamed = work.clone();
        renamed.name = "Munka 2".to_string();
        db.upsert_workspace(&renamed).expect("rename");
        assert_eq!(db.all_workspaces().expect("list")[1].name, "Munka 2");
        assert!(db.delete_workspace("w-home").expect("delete"));
        assert!(!db.delete_workspace("w-home").expect("delete again"));
        assert_eq!(
            db.all_workspaces().expect("list"),
            vec![renamed.clone()],
            "a deleted workspace leaves the table"
        );

        db.replace_workspaces(&[home]).expect("replace");
        assert_eq!(
            db.all_workspaces().expect("list"),
            vec![sample_workspace("w-home")]
        );
        std::fs::remove_dir_all(dir).ok();
    }

    #[test]
    fn a_session_row_round_trips_with_every_ledger_field() {
        let dir = temp_dir("sessions");
        let db = Db::open(&dir).expect("open");
        let entry = sample_session("https://example.org");
        db.upsert_session(&entry).expect("write");
        assert_eq!(
            db.all_sessions().expect("list"),
            vec![entry.clone()],
            "every ledger field survives the round trip"
        );

        // The pair (container, origin) is the key: a second write replaces the row.
        let mut changed = entry.clone();
        changed.state = LoginState::LoggedIn;
        changed.policy = SessionPolicy::Wipe;
        changed.keep_alive = true;
        changed.last_verified_ms = Some(1_700_000_001_000);
        changed.live_ram_bytes = None;
        db.upsert_session(&changed).expect("update");
        let (_, _, _, sessions) = db.counts().expect("counts");
        assert_eq!(sessions, 1, "no duplicate row was created");
        assert_eq!(db.all_sessions().expect("list"), vec![changed]);

        // A different origin is a different row, and the list is ordered by origin — the panel's stable order.
        db.upsert_session(&sample_session("https://a.example"))
            .expect("second row");
        let origins: Vec<String> = db
            .all_sessions()
            .expect("list")
            .into_iter()
            .map(|row| row.origin)
            .collect();
        assert_eq!(origins, vec!["https://a.example", "https://example.org"]);

        assert!(db
            .delete_session("c-private", "https://a.example")
            .expect("close"));
        assert!(!db
            .delete_session("c-private", "https://a.example")
            .expect("close again"));
        std::fs::remove_dir_all(dir).ok();
    }

    #[test]
    fn the_session_rules_are_one_row_that_starts_absent() {
        let dir = temp_dir("session-rules");
        let db = Db::open(&dir).expect("open");
        assert_eq!(
            db.session_rules().expect("read"),
            None,
            "nothing was ever chosen, so the policy module's defaults apply"
        );
        let rules = SessionRules {
            container_default: SessionPolicy::Wipe,
            ttl_ms: 3_600_000,
            wipe_on_app_lock: true,
        };
        db.set_session_rules(&rules).expect("write");
        assert_eq!(db.session_rules().expect("read"), Some(rules));
        let kept = SessionRules {
            container_default: SessionPolicy::Keep,
            ttl_ms: 7_200_000,
            wipe_on_app_lock: false,
        };
        db.set_session_rules(&kept).expect("update");
        assert_eq!(
            db.session_rules().expect("read"),
            Some(kept),
            "the rules are one row, not an append"
        );
        std::fs::remove_dir_all(dir).ok();
    }

    #[test]
    fn replacing_the_ledger_takes_rows_and_rules_together() {
        let dir = temp_dir("replace-ledger");
        let mut db = Db::open(&dir).expect("open");
        db.upsert_session(&sample_session("https://stale.example"))
            .expect("stale row");
        let rules = SessionRules {
            container_default: SessionPolicy::Wipe,
            ttl_ms: 3_600_000,
            wipe_on_app_lock: true,
        };
        let first = sample_session("https://one.example");
        let second = sample_session("https://two.example");
        db.replace_sessions(&[first.clone(), second.clone()], &rules)
            .expect("replace");
        assert_eq!(db.all_sessions().expect("list"), vec![first, second]);
        assert_eq!(db.session_rules().expect("read"), Some(rules));
        std::fs::remove_dir_all(dir).ok();
    }

    #[test]
    fn user_prefs_validate_refuses_out_of_range_and_cold_longer_than_idle() {
        // The factory settings pass.
        assert!(UserPrefs::defaults().validate().is_ok());

        // max_live_cards: only 1..=12.
        let mut bad = UserPrefs::defaults();
        bad.max_live_cards = 0;
        assert_eq!(
            bad.validate(),
            Err("max_live_cards must be between 1 and 12")
        );
        bad.max_live_cards = 13;
        assert_eq!(
            bad.validate(),
            Err("max_live_cards must be between 1 and 12")
        );
        bad.max_live_cards = 12;
        assert!(bad.validate().is_ok());

        // idle_hibernate_ms: only 60000..=3600000.
        bad = UserPrefs::defaults();
        bad.idle_hibernate_ms = 59_999;
        assert_eq!(
            bad.validate(),
            Err("idle_hibernate_ms must be between 60000 and 3600000")
        );
        bad.idle_hibernate_ms = 3_600_001;
        assert_eq!(
            bad.validate(),
            Err("idle_hibernate_ms must be between 60000 and 3600000")
        );
        bad.idle_hibernate_ms = 3_600_000;
        bad.cold_card_ms = 86_400_000;
        assert!(
            bad.validate().is_ok(),
            "a one-hour idle window with a one-day cold window is the documented upper pair"
        );

        // cold_card_ms: only 3600000..=2592000000.
        bad = UserPrefs::defaults();
        bad.cold_card_ms = 3_599_999;
        assert_eq!(
            bad.validate(),
            Err("cold_card_ms must be between 3600000 and 2592000000")
        );
        bad.cold_card_ms = 2_592_000_001;
        assert_eq!(
            bad.validate(),
            Err("cold_card_ms must be between 3600000 and 2592000000")
        );
        // An in-range value that is still equal to the idle window is refused by the ordering rule, not the range.
        bad.idle_hibernate_ms = 3_600_000;
        bad.cold_card_ms = 3_600_000;
        assert_eq!(
            bad.validate(),
            Err("cold_card_ms must be greater than idle_hibernate_ms")
        );

        // The order of the levels is the rule: idle hibernation first, the cold state later. Both are put in range
        // first, so the ordering rule is what fails, not the range check above it.
        bad = UserPrefs::defaults();
        bad.idle_hibernate_ms = 3_600_000;
        bad.cold_card_ms = bad.idle_hibernate_ms;
        assert_eq!(
            bad.validate(),
            Err("cold_card_ms must be greater than idle_hibernate_ms")
        );
        bad.cold_card_ms = bad.idle_hibernate_ms + 1;
        assert!(
            bad.validate().is_ok(),
            "one millisecond longer is a valid, if eccentric, ordering"
        );

        // memory_budget_mb: only 64..=4096.
        bad = UserPrefs::defaults();
        bad.memory_budget_mb = 63;
        assert_eq!(
            bad.validate(),
            Err("memory_budget_mb must be between 64 and 4096")
        );
        bad.memory_budget_mb = 4097;
        assert_eq!(
            bad.validate(),
            Err("memory_budget_mb must be between 64 and 4096")
        );
        bad.memory_budget_mb = 512;
        assert!(bad.validate().is_ok());
    }

    #[test]
    fn the_profile_name_and_kind_round_trip_and_map_to_adr020_params() {
        // The stored string ↔ enum round trip (one place, so the database and the wire agree).
        assert_eq!(profile_kind_to_str(ProfileKind::Fast), "fast");
        assert_eq!(profile_kind_to_str(ProfileKind::Manual), "manual");
        assert_eq!(profile_kind_from_str("saver"), ProfileKind::Saver);
        assert_eq!(
            profile_kind_from_str("kiegyensulyozott"),
            ProfileKind::Balanced,
            "an older build's name falls back to the factory profile"
        );
        // The built-in profile's hibernation window is ADR-020's (15/10/5/2 minutes), and Manual has none.
        assert_eq!(
            ProfileKind::Fast.params().map(|p| p.idle_hibernate_ms),
            Some(900_000)
        );
        assert_eq!(
            ProfileKind::Balanced.params().map(|p| p.idle_hibernate_ms),
            Some(600_000)
        );
        assert_eq!(
            ProfileKind::Saver.params().map(|p| p.idle_hibernate_ms),
            Some(300_000)
        );
        assert_eq!(
            ProfileKind::Ultra.params().map(|p| p.idle_hibernate_ms),
            Some(120_000)
        );
        assert!(ProfileKind::Manual.params().is_none());
    }

    #[test]
    fn user_prefs_round_trips_and_starts_at_defaults() {
        let dir = temp_dir("user-prefs");
        let db = Db::open(&dir).expect("open");

        // Absent row → defaults.
        let first = db.user_prefs().expect("read");
        assert_eq!(first, UserPrefs::defaults());

        // One row: write, read back, and the row is one row, not an append.
        let changed = UserPrefs {
            max_live_cards: 7,
            idle_hibernate_ms: 300_000,
            cold_card_ms: 900_000,
            memory_budget_mb: 768,
            resource_profile: ProfileKind::Saver,
        };
        db.set_user_prefs(&changed).expect("write");
        assert_eq!(db.user_prefs().expect("read"), changed);

        let back = UserPrefs {
            max_live_cards: 3,
            idle_hibernate_ms: 120_000,
            cold_card_ms: 60_000,
            memory_budget_mb: 256,
            resource_profile: ProfileKind::Ultra,
        };
        db.set_user_prefs(&back).expect("update");
        assert_eq!(
            db.user_prefs().expect("read"),
            back,
            "prefs are one row, not an append"
        );
        std::fs::remove_dir_all(dir).ok();
    }
}
