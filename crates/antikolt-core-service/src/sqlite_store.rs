// SPDX-License-Identifier: MPL-2.0

//! The card store's home: one SQLite file instead of a JSON document (ADR-064 → ADR-069).
//!
//! Why: the JSON files are atomic and readable, but nothing *queries* them. Every "which cards are in this
//! container", "what does this session hold", "what is due" question walks the whole list in the core. That is fine
//! for 10 cards and wrong for 5 000, and it is the reason the roadmap listed the store swap as the next big step.
//!
//! Since ADR-069 the **cards are SQLite-first**: `cards.json` is a derived export, written when the core starts,
//! shuts down, or is asked to (`db_status` with `sync: true`), and read only once — to migrate a profile that was
//! written before the switch. The containers, workspaces and sessions are still JSON documents; their tables exist
//! here and are refreshed on demand, but nothing reads them yet.
//!
//! The schema version is a number in `PRAGMA user_version` — not a table — because it is the SQLite idiom and it
//! survives any table rename: `0002` adds `page_text`, `tags` and `workspace`; `0003` records the switch itself (the
//! file is no longer a mirror of `cards.json`, so that document must not be imported again).

use std::path::{Path, PathBuf};

use rusqlite::{params, Connection, OptionalExtension};

use crate::containers::Container;
use crate::store::Card;
use crate::workspaces::Workspace;

/// The name of the database file inside the profile directory. One file, next to the JSON documents it replaces.
pub const DB_FILE_NAME: &str = "antikolt.sqlite3";

/// The schema this core writes. Bumped only with a migration; `0001` was the JSON files, `0002` added `page_text`,
/// `tags` and `workspace`, `0003` is the card-store switch (ADR-069).
pub const SCHEMA_VERSION: i64 = 3;

/// The tags travel as a JSON array in one column: SQLite has no array type, and a join table for a 12-item list
/// would cost more than it explains.
#[derive(Debug)]
pub struct Db {
    path: PathBuf,
    connection: Connection,
    /// The `user_version` the file reported *before* this open migrated it. It is how the core knows that the file
    /// predates the switch, so `cards.json` was still the writer of record and has to be read one last time
    /// (ADR-069).
    opened_at_version: i64,
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

    /// True when this file was written by a pre-switch build (`user_version` below [`SCHEMA_VERSION`]) *at the moment
    /// it was opened*: back then `cards.json` was the truth and this file was its mirror, so the JSON document wins
    /// one last time and is imported (ADR-069). A file created by this build reports `false`, so an export is never
    /// imported back.
    pub fn was_upgraded(&self) -> bool {
        self.opened_at_version < SCHEMA_VERSION
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
        // Version 3 has no table of its own: the number says "this file is the writer, not a mirror of `cards.json`".
        // It is written by [`Db::mark_switched`] *after* the JSON import, never here — a failed import must leave the
        // file pre-switch so the next start retries instead of silently ignoring cards that were never read.
        Ok(())
    }

    /// Record that the one-time migration from `cards.json` finished (ADR-069). Until this is called the file keeps
    /// reporting the pre-switch version, so a crash (or an unreadable JSON document) cannot make the core believe an
    /// import already happened when it did not.
    pub fn mark_switched(&self) -> Result<(), DbError> {
        self.connection.execute_batch("PRAGMA user_version = 3;")?;
        Ok(())
    }

    /// The version the file reports, for the `db_status` reply (and for the test that proves the migration is once).
    pub fn schema_version(&self) -> Result<i64, DbError> {
        Ok(self
            .connection
            .query_row("PRAGMA user_version", [], |row| row.get(0))?)
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

    /// Bring the two reference tables in step with the JSON stores that still own them (containers, workspaces).
    ///
    /// Nothing reads these tables yet: the containers and workspaces switch to the database in a later slice
    /// (ADR-069 keeps the scope to the cards). Refreshing them on demand — instead of on every card write — keeps
    /// the database an honest preview of what the migration will hold, without pretending the switch already
    /// happened for stores it did not.
    pub fn sync_reference_tables(
        &mut self,
        containers: &[Container],
        workspaces: &[Workspace],
    ) -> Result<(), DbError> {
        let transaction = self.connection.transaction()?;
        transaction.execute("DELETE FROM containers", [])?;
        transaction.execute("DELETE FROM workspaces", [])?;
        for container in containers {
            transaction.execute(
                "INSERT INTO containers (id, name, color) VALUES (?1, ?2, ?3)",
                params![container.id, container.name, container.color],
            )?;
        }
        for workspace in workspaces {
            transaction.execute(
                "INSERT INTO workspaces (id, name) VALUES (?1, ?2)",
                params![workspace.id, workspace.name],
            )?;
        }
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

/// One `INSERT … ON CONFLICT` used by both the single-card write and the whole-list replace, so the two can never
/// write different columns.
fn upsert_card_row(connection: &Connection, card: &Card) -> Result<(), rusqlite::Error> {
    let tags = serde_json::to_string(&card.tags).unwrap_or_else(|_| "[]".to_string());
    connection.execute(
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
        }
    }

    #[test]
    fn a_fresh_database_migrates_to_the_current_schema_and_keeps_it() {
        let dir = temp_dir("migrate");
        let db = Db::open(&dir).expect("open");
        assert!(db.path().exists());
        let (cards, containers, workspaces, sessions) = db.counts().expect("counts");
        assert_eq!((cards, containers, workspaces, sessions), (0, 0, 0, 0));
        // A brand-new file is "upgraded" (0 → tables): the core still has to decide where the first cards come from.
        assert!(db.was_upgraded(), "a brand-new file reports its upgrade");
        // The switch is recorded only after that decision — see `mark_switched`.
        db.mark_switched().expect("mark");
        assert_eq!(db.schema_version().expect("version"), SCHEMA_VERSION);
        drop(db);
        // Opening again must not run the migration twice or lose the version.
        let again = Db::open(&dir).expect("reopen");
        assert_eq!(again.schema_version().expect("version"), SCHEMA_VERSION);
        assert!(
            !again.was_upgraded(),
            "a file this build already wrote is not imported from JSON again"
        );
        std::fs::remove_dir_all(dir).ok();
    }

    #[test]
    fn a_pre_switch_database_reports_itself_as_an_upgrade() {
        // The old builds wrote `user_version = 2` with a mirror of the JSON files. That file must be recognisable
        // as pre-switch on open, because its `cards.json` is still the writer of record (ADR-069).
        let dir = temp_dir("v2");
        {
            let connection = Connection::open(dir.join(DB_FILE_NAME)).expect("raw open");
            connection
                .execute_batch(
                    "CREATE TABLE cards (id TEXT PRIMARY KEY, title TEXT NOT NULL, url TEXT NOT NULL);
                     PRAGMA user_version = 2;",
                )
                .expect("write a v2 file");
        }
        let db = Db::open(&dir).expect("open");
        assert!(db.was_upgraded(), "a v2 file predates the switch");
        db.mark_switched().expect("mark");
        assert_eq!(db.schema_version().expect("version"), SCHEMA_VERSION);
        drop(db);
        let again = Db::open(&dir).expect("reopen");
        assert!(
            !again.was_upgraded(),
            "the switch is recorded on disk, so the import happens once"
        );
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

    #[test]
    fn the_reference_tables_are_refreshed_from_the_json_stores() {
        let dir = temp_dir("tables");
        let mut db = Db::open(&dir).expect("open");
        db.sync_reference_tables(
            &[Container {
                id: "c-proba".to_string(),
                name: "Próba".to_string(),
                color: "#123456".to_string(),
            }],
            &[Workspace {
                id: "w-proba".to_string(),
                name: "Próba-tér".to_string(),
            }],
        )
        .expect("sync");
        let (_, containers, workspaces, _) = db.counts().expect("counts");
        assert_eq!((containers, workspaces), (1, 1));

        // A refresh is a replace, not an append: a deleted container must leave the table too.
        db.sync_reference_tables(&[], &[]).expect("empty sync");
        let (_, containers, workspaces, _) = db.counts().expect("counts");
        assert_eq!((containers, workspaces), (0, 0));
        std::fs::remove_dir_all(dir).ok();
    }
}
