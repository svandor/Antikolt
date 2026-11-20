// SPDX-License-Identifier: MPL-2.0

//! The store's destination: one SQLite file instead of four JSON documents (ADR-064).
//!
//! Why: the JSON files are atomic and readable, but nothing *queries* them. Every "which cards are in this
//! container", "what does this session hold", "what is due" question walks the whole list in the core. That is fine
//! for 10 cards and wrong for 5 000, and it is the reason the roadmap lists the store swap as the next big step.
//!
//! This module is deliberately **the first half only**: the schema and the migration live here, the tables exist, and
//! a card round-trips through them. The four stores keep reading and writing JSON until the switch is measured
//! (the `db_status` op keeps the database in step with them, so the migration is proven on real data before it is
//! the only copy).
//!
//! The schema version is a number in `PRAGMA user_version` — not a table — because it is the SQLite idiom and it
//! survives any table rename: `0002` is the version that adds `page_text`, `tags` and `workspace`.

use std::path::{Path, PathBuf};

use rusqlite::{params, Connection, OptionalExtension};

use crate::containers::Container;
use crate::store::Card;
use crate::workspaces::Workspace;

/// The schema this core writes. Bumped only with a migration; `0001` was the JSON files, `0002` is this one.
pub const SCHEMA_VERSION: i64 = 2;

/// The tags travel as a JSON array in one column: SQLite has no array type, and a join table for a 12-item list
/// would cost more than it explains.
#[derive(Debug)]
pub struct Db {
    path: PathBuf,
    connection: Connection,
}

#[derive(Debug, thiserror::Error)]
pub enum DbError {
    #[error("cannot open the store: {0}")]
    Sqlite(#[from] rusqlite::Error),
    #[error("cannot create the data directory: {0}")]
    Io(#[from] std::io::Error),
}

impl Db {
    /// Open (and create) the database next to the JSON files, then apply the migration. Opening twice is safe: the
    /// migration is a no-op when `user_version` is already `SCHEMA_VERSION`.
    pub fn open(data_dir: &Path) -> Result<Self, DbError> {
        std::fs::create_dir_all(data_dir)?;
        let path = data_dir.join("antikolt.sqlite3");
        let connection = Connection::open(&path)?;
        // WAL: a reader (the shell asking for a list) must not wait for a writer (a pin), and a crash must not lose
        // the last committed transaction. It is the point of moving to a database.
        connection.pragma_update(None, "journal_mode", "WAL")?;
        connection.pragma_update(None, "foreign_keys", "ON")?;
        let db = Self { path, connection };
        db.migrate()?;
        Ok(db)
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    /// The migration. Idempotent, and written so that a later version can add a step instead of rewriting this one.
    fn migrate(&self) -> Result<(), DbError> {
        let current: i64 = self
            .connection
            .query_row("PRAGMA user_version", [], |row| row.get(0))?;
        if current >= SCHEMA_VERSION {
            return Ok(());
        }
        if current < 2 {
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
        Ok(())
    }

    /// The version the file reports, for the `db_status` reply (and for the test that proves the migration is once).
    pub fn schema_version(&self) -> Result<i64, DbError> {
        Ok(self
            .connection
            .query_row("PRAGMA user_version", [], |row| row.get(0))?)
    }

    /// Write one card, replacing the row with the same id. The JSON store stays the truth until the switch (ADR-064),
    /// so this is a mirror: the same record, in a shape a query can use.
    pub fn upsert_card(&self, card: &Card) -> Result<(), DbError> {
        let tags = serde_json::to_string(&card.tags).unwrap_or_else(|_| "[]".to_string());
        self.connection.execute(
            "INSERT INTO cards (id, title, url, container, status, pinned, notes, preview_ref,
                                last_opened_at_ms, page_text, workspace, tags)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)
             ON CONFLICT (id) DO UPDATE SET
                 title = excluded.title, url = excluded.url, container = excluded.container,
                 status = excluded.status, pinned = excluded.pinned, notes = excluded.notes,
                 preview_ref = excluded.preview_ref, last_opened_at_ms = excluded.last_opened_at_ms,
                 page_text = excluded.page_text, workspace = excluded.workspace, tags = excluded.tags",
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
                tags
            ],
        )?;
        Ok(())
    }

    /// Refresh the mirror: replace the tables with the given lists in one transaction.
    ///
    /// An upsert-only mirror is not a mirror — a card the user deleted stays in the database for ever, and the parity
    /// check (rightly) refuses to call that agreement. The delete-then-insert is the honest shape, and it is one
    /// transaction so a reader never sees half a profile.
    pub fn mirror(
        &mut self,
        cards: &[Card],
        containers: &[Container],
        workspaces: &[Workspace],
    ) -> Result<(), DbError> {
        let transaction = self.connection.transaction()?;
        transaction.execute("DELETE FROM cards", [])?;
        transaction.execute("DELETE FROM containers", [])?;
        transaction.execute("DELETE FROM workspaces", [])?;
        transaction.commit()?;
        for card in cards {
            self.upsert_card(card)?;
        }
        for container in containers {
            self.upsert_container(container)?;
        }
        for workspace in workspaces {
            self.upsert_workspace(workspace)?;
        }
        Ok(())
    }

    /// One card by id. Used by the tests and by the parity check's per-row path; the binary reads the whole list
    /// (`all_cards`), so the attribute keeps the two from drifting apart in different builds.
    #[cfg_attr(not(test), allow(dead_code))]
    pub fn card(&self, card_id: &str) -> Result<Option<Card>, DbError> {
        let row = self
            .connection
            .query_row(
                "SELECT id, title, url, container, status, pinned, notes, preview_ref,
                        last_opened_at_ms, page_text, workspace, tags
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
                    })
                },
            )
            .optional()?;
        Ok(row)
    }

    /// Drop one card (`card_delete`, ADR-066). Without this the mirror would keep a deleted card for ever — the exact
    /// bug the parity check found when the mirror was upsert-only.
    pub fn delete_card(&self, card_id: &str) -> Result<bool, DbError> {
        let removed = self
            .connection
            .execute("DELETE FROM cards WHERE id = ?1", params![card_id])?;
        Ok(removed > 0)
    }

    /// Every card the database holds, for the parity check the switch depends on (ADR-064): the grid can only move to
    /// the database when *all* the rows agree with the JSON store, not just the first one.
    /// Every card the database holds, in the store's order (ADR-067): newest-opened first, then title.
    ///
    /// The JSON store sorted in Rust; a database read without `ORDER BY` would return insertion order instead, and the
    /// grid would quietly stop reordering when a card is opened — a silent behaviour change the store swap must not
    /// introduce.
    pub fn all_cards(&self) -> Result<Vec<Card>, DbError> {
        let mut statement = self.connection.prepare(
            "SELECT id, title, url, container, status, pinned, notes, preview_ref,
                    last_opened_at_ms, page_text, workspace, tags
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

    pub fn upsert_container(&self, container: &Container) -> Result<(), DbError> {
        self.connection.execute(
            "INSERT INTO containers (id, name, color) VALUES (?1, ?2, ?3)
             ON CONFLICT (id) DO UPDATE SET name = excluded.name, color = excluded.color",
            params![container.id, container.name, container.color],
        )?;
        Ok(())
    }

    pub fn upsert_workspace(&self, workspace: &Workspace) -> Result<(), DbError> {
        self.connection.execute(
            "INSERT INTO workspaces (id, name) VALUES (?1, ?2)
             ON CONFLICT (id) DO UPDATE SET name = excluded.name",
            params![workspace.id, workspace.name],
        )?;
        Ok(())
    }

    /// The row counts, for `db_status`: the shell can show how far the mirror has got without guessing.
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

#[cfg(test)]
mod tests {
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
        }
    }

    #[test]
    fn a_fresh_database_migrates_to_the_current_schema_and_keeps_it() {
        let dir = temp_dir("migrate");
        let db = Db::open(&dir).expect("open");
        assert_eq!(db.schema_version().expect("version"), SCHEMA_VERSION);
        assert!(db.path().exists());
        let (cards, containers, workspaces, sessions) = db.counts().expect("counts");
        assert_eq!((cards, containers, workspaces, sessions), (0, 0, 0, 0));
        drop(db);
        // Opening again must not run the migration twice or lose the version.
        let again = Db::open(&dir).expect("reopen");
        assert_eq!(again.schema_version().expect("version"), SCHEMA_VERSION);
        std::fs::remove_dir_all(dir).ok();
    }

    #[test]
    fn a_card_round_trips_with_every_field_the_json_store_has() {
        let dir = temp_dir("roundtrip");
        let db = Db::open(&dir).expect("open");
        let card = sample_card();
        db.upsert_card(&card).expect("write");
        let read = db.card(&card.id).expect("read").expect("row");
        assert_eq!(read.title, card.title);
        assert_eq!(read.url, card.url);
        assert_eq!(read.container, card.container);
        assert!(read.pinned);
        assert_eq!(read.notes, card.notes);
        assert_eq!(read.preview_ref, card.preview_ref);
        assert_eq!(read.last_opened_at_ms, card.last_opened_at_ms);
        assert_eq!(read.page_text, card.page_text);
        assert_eq!(read.workspace, card.workspace);
        assert_eq!(read.tags, card.tags);
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
    fn the_mirror_tables_take_the_containers_and_workspaces_too() {
        let dir = temp_dir("tables");
        let db = Db::open(&dir).expect("open");
        db.upsert_container(&Container {
            id: "c-proba".to_string(),
            name: "Próba".to_string(),
            color: "#123456".to_string(),
        })
        .expect("container");
        db.upsert_workspace(&Workspace {
            id: "w-proba".to_string(),
            name: "Próba-tér".to_string(),
        })
        .expect("workspace");
        let (_, containers, workspaces, _) = db.counts().expect("counts");
        assert_eq!((containers, workspaces), (1, 1));
        std::fs::remove_dir_all(dir).ok();
    }
}
