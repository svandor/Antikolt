// SPDX-License-Identifier: MPL-2.0

//! The card store the core service serves.
//!
//! Since ADR-069 the store is **SQLite** (`sqlite_store::Db`): the database is the reader *and* the writer, which is
//! what makes the index and the queries mean something instead of walking a list. `cards.json` survives as an
//! **export** — written when a profile is adopted, when the core shuts down, and whenever `db_status` is asked to
//! sync — and is read exactly once, to adopt a profile written before the switch.
//!
//! The switch is deliberately one-way and one-time: the file reports `user_version = 3` only after the adoption
//! finished, so an unreadable or half-written export can never be imported back over newer cards.

use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::containers::Container;
use crate::sqlite_store::{Db, DbError};
use crate::workspaces::Workspace;

/// How much of a page's text one card may keep (`card_page_text`, ADR-049). The cap is a promise about the store, not
/// about the page's length.
pub const MAX_PAGE_TEXT: usize = 16_000;

/// How many tags one card may carry, and how long one tag may be (`card_upsert`, ADR-055). A tag is a word or a
/// short phrase the user typed; the limits are what keeps a pasted paragraph from becoming a "tag".
pub const MAX_TAGS: usize = 12;
pub const MAX_TAG_LEN: usize = 32;

/// The file next to the database: the legacy document the pre-switch builds wrote, and the export this build keeps.
pub const JSON_FILE_NAME: &str = "cards.json";

/// Where the one-time adoption keeps the database it is about to rewrite (ADR-069). An adoption that discards rows —
/// the mirror lagged, or the document was truncated to `[]` — must leave them recoverable, not gone.
pub const ADOPTION_BACKUP_FILE_NAME: &str = "antikolt.pre-adoption.sqlite3";

/// One bookmark card, as the grid shows it.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Card {
    pub id: String,
    pub title: String,
    pub url: String,
    /// Container (workspace-ish grouping) id, e.g. `c-work`.
    #[serde(default)]
    pub container: String,
    /// `signed_in` | `expiring` | `signed_out` | `unknown`.
    #[serde(default = "default_status")]
    pub status: String,
    #[serde(default)]
    pub pinned: bool,
    #[serde(default)]
    pub notes: String,
    /// A preview image's name inside the profile's `previews` directory, served to the grid as
    /// `antikolt://blob/<preview_ref>` (ADR-035). Empty means "no preview yet".
    ///
    /// Additive like every other field: a store written before previews existed still parses, and the
    /// shell fills it in when it has a picture. It is a *name*, not a path and not an image — the image
    /// lives in the profile so the card list stays small.
    #[serde(default)]
    pub preview_ref: String,
    /// Epoch milliseconds of the last time the user opened this card; `0` means never.
    #[serde(default)]
    pub last_opened_at_ms: u64,
    /// The page's own text, captured when the card was frozen (`card_page_text`, ADR-049), so a search can find
    /// a word that appears only in the article and not in its title or note. Capped by the store (see
    /// `MAX_PAGE_TEXT`).
    #[serde(default)]
    pub page_text: String,
    /// The workspace (top-level tab) this card lives under (ADR-056). Empty means "the first workspace", which is
    /// what every card written before the workspaces were real means: a missing field must not move a card to a
    /// tab the user never chose.
    #[serde(default)]
    pub workspace: String,
    /// Free-form tags, written by the user on the card (ADR-055). The search has always promised "címke", and a
    /// card model without a tag field made that a promise the product could not keep. Normalised by the store:
    /// trimmed, de-duplicated case-insensitively, at most [`MAX_TAGS`] of them, each at most [`MAX_TAG_LEN`].
    #[serde(default)]
    pub tags: Vec<String>,
}

fn default_status() -> String {
    "unknown".to_string()
}

#[derive(Debug, thiserror::Error)]
pub enum StoreError {
    #[error("cannot read or write the card store: {0}")]
    Io(#[from] std::io::Error),
    #[error("the card store is not valid JSON: {0}")]
    Json(#[from] serde_json::Error),
    /// The pre-switch document could not be read: it was the only copy of the cards at that moment, so the core stops
    /// instead of replacing it with seeds.
    #[error("cannot read the card list written by an earlier version ({path}): {source}")]
    Legacy {
        path: PathBuf,
        #[source]
        source: serde_json::Error,
    },
    #[error("the database refused the card store: {0}")]
    Db(#[from] DbError),
}

/// What `db_status` reports: the database's own numbers, and how well the export it just wrote agrees with them.
#[derive(Debug, Clone)]
pub struct StorageReport {
    pub path: PathBuf,
    pub schema: i64,
    pub cards: i64,
    pub containers: i64,
    pub workspaces: i64,
    pub sessions: i64,
    pub export_path: PathBuf,
    /// How many rows were compared against the export — the card count, not one sample.
    pub checked: i64,
    /// How many rows disagreed; `-1` means the export could not be read at all (unknown, not zero).
    pub mismatches: i64,
    pub verified: bool,
    pub cards_in_workspace: i64,
}

/// The card store: the cards in SQLite, with the JSON document as a derived export.
#[derive(Debug)]
pub struct CardStore {
    data_dir: PathBuf,
    db: Db,
}

impl CardStore {
    /// Open the store at `data_dir/antikolt.sqlite3`, adopting a pre-switch `cards.json` once, and seeding the
    /// example cards on a truly first run.
    ///
    /// Seeding is deliberate: an empty grid on the very first start looks like a bug, and the user has nothing to
    /// click. It happens only when the database file is new *and* there is no legacy document to adopt — a profile
    /// whose cards the user deleted stays empty, because re-seeding it would resurrect them.
    pub fn open(data_dir: &Path) -> Result<Self, StoreError> {
        fs::create_dir_all(data_dir)?;
        let db = Db::open(data_dir)?;
        let upgraded = db.was_upgraded();
        let mut store = Self {
            data_dir: data_dir.to_path_buf(),
            db,
        };
        if upgraded {
            store.adopt_legacy_profile()?;
            // Only now is the switch recorded: if the adoption failed above, the file still says "pre-switch" and the
            // next start tries again instead of serving a half-adopted profile.
            store.db.mark_switched()?;
            // The adopted document is rewritten once so it carries the store's normalisation (tags trimmed, page text
            // capped) instead of the raw bytes of an older build. A normal start does **not** rewrite it: the export is
            // derived, and serialising every card on every launch would put the old O(n) write back in the startup path.
            if let Err(error) = store.export_json() {
                eprintln!("[core] store_export_failed: {error}");
            }
        }
        Ok(store)
    }

    /// Every card, newest-opened first, then by title — the grid's default order, decided in SQL (ADR-067).
    pub fn list(&self) -> Result<Vec<Card>, StoreError> {
        Ok(self.db.all_cards()?)
    }

    /// Record that a card was opened now. Returns the card, or `None` when the id is unknown.
    pub fn mark_opened(&mut self, id: &str, now_ms: u64) -> Result<Option<Card>, StoreError> {
        let Some(mut card) = self.db.card(id)? else {
            return Ok(None);
        };
        card.last_opened_at_ms = now_ms;
        self.db.upsert_card(&card)?;
        Ok(Some(card))
    }

    /// Insert or replace a card by id. The grid's editor uses this.
    pub fn upsert(&mut self, mut card: Card) -> Result<Card, StoreError> {
        card.page_text = Self::cap_page_text(&card.page_text);
        card.tags = Self::normalise_tags(&card.tags);
        // A pin or a note arrives from the page, which never carries the captured text: an empty field means "I did
        // not send it", not "delete the capture" (ADR-049).
        if card.page_text.is_empty() {
            if let Some(existing) = self.db.card(&card.id)? {
                card.page_text = existing.page_text;
            }
        }
        self.db.upsert_card(&card)?;
        Ok(card)
    }

    /// Store the page's text for one card (`card_page_text`, ADR-049). An unknown id is not an error at the
    /// store level — the caller decides what a missing card means.
    pub fn set_page_text(&mut self, card_id: &str, text: &str) -> Result<Option<Card>, StoreError> {
        let Some(mut card) = self.db.card(card_id)? else {
            return Ok(None);
        };
        card.page_text = Self::cap_page_text(text);
        self.db.upsert_card(&card)?;
        Ok(Some(card))
    }

    /// Move every card from one container to another (`container_delete` with `move_to`, ADR-054), in one statement.
    ///
    /// This is the explicit exit from "a used container is not deletable" rule: the caller has to say where the
    /// cards go, so nothing disappears by accident, and the count is returned for the reply. Because the database is
    /// the only copy now, there is no mirror left to walk (the bug ADR-066 had to fix by hand).
    pub fn move_container(&mut self, from: &str, to: &str) -> Result<usize, StoreError> {
        Ok(self.db.move_container(from, to)?)
    }

    /// Trim, de-duplicate (case-insensitively, keeping the first spelling) and cap the tags (ADR-055).
    ///
    /// The store is where every writer meets: the page, the importer and a test. A rule that only lives in the page
    /// is a rule the importer will not follow.
    pub fn normalise_tags(tags: &[String]) -> Vec<String> {
        let mut seen: Vec<String> = Vec::new();
        for tag in tags {
            let trimmed = tag.trim();
            if trimmed.is_empty() {
                continue;
            }
            let capped: String = trimmed.chars().take(MAX_TAG_LEN).collect();
            let key = capped.to_lowercase();
            if seen.iter().any(|existing| existing.to_lowercase() == key) {
                continue;
            }
            seen.push(capped);
            if seen.len() >= MAX_TAGS {
                break;
            }
        }
        seen
    }

    /// The text a search may see. The cap is a promise about the stored row, not about the page: this is far more
    /// than any title or note, and five hundred of them stay small enough for a database that is now the only copy.
    fn cap_page_text(text: &str) -> String {
        let collapsed = text.split_whitespace().collect::<Vec<_>>().join(" ");
        collapsed.chars().take(MAX_PAGE_TEXT).collect()
    }

    /// Remove one card (`card_delete`, ADR-061). An unknown id is `Ok(false)`: the caller decides what a missing
    /// card means, and a delete that says "gone" twice must not be an error the second time.
    pub fn remove(&mut self, card_id: &str) -> Result<bool, StoreError> {
        Ok(self.db.delete_card(card_id)?)
    }

    /// Replace the whole list, for an import (`profile_import`, ADR-059). Every card goes through the same
    /// normalisation a write does, so an imported profile cannot smuggle in a rule the store would refuse.
    pub fn replace_all(&mut self, cards: Vec<Card>) -> Result<usize, StoreError> {
        let cards: Vec<Card> = cards
            .into_iter()
            .map(|mut card| {
                card.page_text = Self::cap_page_text(&card.page_text);
                card.tags = Self::normalise_tags(&card.tags);
                card
            })
            .collect();
        let count = cards.len();
        self.db.replace_all_cards(&cards)?;
        Ok(count)
    }

    /// Write the legacy JSON document from the database: one atomic file, pretty-printed, in the grid's order.
    ///
    /// It is an export, not a store: nothing reads it (except the one-time adoption of a pre-switch profile), and a
    /// failure here never fails a user's write — the caller logs it.
    pub fn export_json(&self) -> Result<PathBuf, StoreError> {
        let cards = self.list()?;
        let text = serde_json::to_string_pretty(&cards)?;
        let path = self.data_dir.join(JSON_FILE_NAME);
        let temporary = path.with_extension("json.tmp");
        {
            let mut file = fs::File::create(&temporary)?;
            file.write_all(text.as_bytes())?;
            // The rename below is only atomic if the bytes really reached the disk first.
            file.sync_all()?;
        }
        fs::rename(&temporary, &path)?;
        Ok(path)
    }

    /// Refresh the two reference tables the cards do not own yet (containers, workspaces). Called from
    /// `db_status(sync: true)`, because nothing reads those tables until their own switch.
    pub fn sync_reference_tables(
        &mut self,
        containers: &[Container],
        workspaces: &[Workspace],
    ) -> Result<(), StoreError> {
        Ok(self.db.sync_reference_tables(containers, workspaces)?)
    }

    /// The numbers `db_status` reports, and — when `export` is set — the write-then-read-back proof that the export
    /// agrees with the database row by row.
    ///
    /// The check is deliberately the *export* direction now: the database is the truth, so "verified" means "the file
    /// this core just wrote can be parsed back into exactly these cards". An unreadable export is reported as
    /// `mismatches = -1` (`not measured`), never as agreement.
    pub fn storage_report(&mut self, export: bool) -> Result<StorageReport, StoreError> {
        let export_path = if export {
            self.export_json()?
        } else {
            self.data_dir.join(JSON_FILE_NAME)
        };
        let cards = self.list()?;
        let mut mismatches = 0i64;
        match fs::read_to_string(&export_path) {
            Ok(text) => match serde_json::from_str::<Vec<Card>>(&text) {
                Ok(exported) => {
                    if exported.len() != cards.len() {
                        mismatches += 1;
                    }
                    for card in &cards {
                        match exported.iter().find(|row| row.id == card.id) {
                            Some(row) if row == card => {}
                            _ => mismatches += 1,
                        }
                    }
                }
                Err(error) => {
                    eprintln!("[core] db_export_parse_failed: {error}");
                    mismatches = -1;
                }
            },
            Err(error) => {
                eprintln!("[core] db_export_read_failed: {error}");
                mismatches = -1;
            }
        }
        let (stored, containers, workspaces, sessions) = self.db.counts()?;
        let cards_in_workspace = cards
            .first()
            .map(|card| self.db.cards_in_workspace(&card.workspace).unwrap_or(0))
            .unwrap_or(0);
        Ok(StorageReport {
            path: self.db.path().to_path_buf(),
            schema: self.db.schema_version()?,
            cards: stored,
            containers,
            workspaces,
            sessions,
            export_path,
            checked: cards.len() as i64,
            mismatches,
            verified: mismatches == 0,
            cards_in_workspace,
        })
    }

    /// Adopt the pre-switch profile exactly once (ADR-069): `cards.json` was the writer of record until this build,
    /// so it wins over the old mirrored database — but only when it actually says something different.
    ///
    /// A database that holds rows the document no longer has is copied aside first
    /// ([`ADOPTION_BACKUP_FILE_NAME`]): an empty document, or one that lost rows, must not destroy the only other copy
    /// of the user's cards silently.
    fn adopt_legacy_profile(&mut self) -> Result<(), StoreError> {
        let existing = self.db.all_cards()?;
        match self.read_legacy_json()? {
            Some(cards) => {
                if existing.is_empty() && cards.is_empty() {
                    let count = self.replace_all(seed_cards())?;
                    eprintln!("[core] store_seeded cards={count}");
                } else if !Self::same_cards(&existing, &cards) {
                    let backup = if existing.is_empty() {
                        None
                    } else {
                        Some(self.backup_before_adoption()?)
                    };
                    let count = self.replace_all(cards)?;
                    match backup {
                        Some(path) => eprintln!(
                            "[core] store_adopted_json cards={count} replaced={} backup={}",
                            existing.len(),
                            path.display()
                        ),
                        None => eprintln!("[core] store_adopted_json cards={count}"),
                    }
                }
            }
            None => {
                if existing.is_empty() {
                    let count = self.replace_all(seed_cards())?;
                    eprintln!("[core] store_seeded cards={count}");
                }
            }
        }
        Ok(())
    }

    /// The snapshot of the pre-switch database, written before the adoption rewrites it.
    fn backup_before_adoption(&self) -> Result<PathBuf, StoreError> {
        let path = self.data_dir.join(ADOPTION_BACKUP_FILE_NAME);
        self.db.snapshot_to(&path)?;
        Ok(path)
    }

    /// The legacy document, if the profile has one. An unreadable document is an error, never an empty list: at this
    /// point it is the only copy of the user's cards.
    fn read_legacy_json(&self) -> Result<Option<Vec<Card>>, StoreError> {
        let path = self.data_dir.join(JSON_FILE_NAME);
        if !path.exists() {
            return Ok(None);
        }
        let text = fs::read_to_string(&path)?;
        match serde_json::from_str::<Vec<Card>>(&text) {
            Ok(cards) => Ok(Some(cards)),
            Err(source) => Err(StoreError::Legacy { path, source }),
        }
    }

    /// Compare two card lists as sets: the old export was written in the store's order, but the order of a document
    /// found on disk is not something to rely on.
    fn same_cards(left: &[Card], right: &[Card]) -> bool {
        if left.len() != right.len() {
            return false;
        }
        let mut left: Vec<&Card> = left.iter().collect();
        let mut right: Vec<&Card> = right.iter().collect();
        left.sort_by(|a, b| a.id.cmp(&b.id));
        right.sort_by(|a, b| a.id.cmp(&b.id));
        left.iter().zip(right.iter()).all(|(a, b)| a == b)
    }
}

fn seed_cards() -> Vec<Card> {
    vec![
        Card {
            id: "01SEED000000000000000001".to_string(),
            title: "Antikolt projekt".to_string(),
            url: "https://github.com/antikolt/antikolt".to_string(),
            container: "c-dev".to_string(),
            status: "signed_in".to_string(),
            pinned: true,
            notes: "A fejlesztés itt zajlik".to_string(),
            preview_ref: String::new(),
            last_opened_at_ms: 0,
            page_text: String::new(),
            tags: Vec::new(),
            workspace: String::new(),
        },
        Card {
            id: "01SEED000000000000000002".to_string(),
            title: "Wikipedia — kezdőlap".to_string(),
            url: "https://hu.wikipedia.org/".to_string(),
            container: "c-private".to_string(),
            status: "unknown".to_string(),
            pinned: false,
            notes: String::new(),
            preview_ref: String::new(),
            last_opened_at_ms: 0,
            page_text: String::new(),
            tags: Vec::new(),
            workspace: String::new(),
        },
        Card {
            id: "01SEED000000000000000003".to_string(),
            title: "Hacker News".to_string(),
            url: "https://news.ycombinator.com/".to_string(),
            container: "c-private".to_string(),
            status: "unknown".to_string(),
            pinned: false,
            notes: String::new(),
            preview_ref: String::new(),
            last_opened_at_ms: 0,
            page_text: String::new(),
            tags: Vec::new(),
            workspace: String::new(),
        },
    ]
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_dir(name: &str) -> PathBuf {
        let stamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .expect("clock")
            .as_nanos();
        let dir = std::env::temp_dir().join(format!("antikolt-store-{name}-{stamp}"));
        fs::create_dir_all(&dir).expect("temp dir");
        dir
    }

    /// A minimal valid card, for the tests that care about one field rather than the whole record.
    fn db_card(id: &str, title: &str) -> Card {
        Card {
            id: id.to_string(),
            title: title.to_string(),
            url: "https://pelda.hu/".to_string(),
            container: String::new(),
            status: "unknown".to_string(),
            pinned: false,
            notes: String::new(),
            preview_ref: String::new(),
            last_opened_at_ms: 0,
            page_text: String::new(),
            workspace: String::new(),
            tags: Vec::new(),
        }
    }

    #[test]
    fn a_fresh_store_is_seeded_in_the_database_and_exported_to_json() {
        let dir = temp_dir("seed");
        let store = CardStore::open(&dir).expect("open");
        assert_eq!(store.list().expect("list").len(), 3, "first run seeds");
        assert!(dir.join("antikolt.sqlite3").exists(), "the cards are rows");
        assert!(
            dir.join(JSON_FILE_NAME).exists(),
            "the export is written next to the database"
        );

        let reopened = CardStore::open(&dir).expect("reopen");
        assert_eq!(
            reopened.list().expect("list"),
            store.list().expect("list"),
            "the rows round-trip"
        );
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_pre_switch_json_profile_is_adopted_once() {
        // A profile written before the switch: only `cards.json` exists. Its cards must arrive in the database with
        // the store's normalisation applied.
        let dir = temp_dir("adopt");
        let legacy = serde_json::json!([
            {
                "id": "01LEGACY0000000000000001",
                "title": "Örökölt kártya",
                "url": "https://pelda.hu/orokseg",
                "tags": ["  Munka  ", "MUNKA"]
            }
        ]);
        fs::write(
            dir.join(JSON_FILE_NAME),
            serde_json::to_string_pretty(&legacy).expect("json"),
        )
        .expect("write the legacy profile");

        let store = CardStore::open(&dir).expect("open");
        let cards = store.list().expect("list");
        assert_eq!(cards.len(), 1, "the JSON list is adopted, not seeded over");
        assert_eq!(cards[0].title, "Örökölt kártya");
        assert_eq!(cards[0].tags, vec!["Munka"], "adoption normalises tags too");
        drop(store);

        // A second open does not adopt the export back: the database is the writer now, and the file merely agrees.
        let again = CardStore::open(&dir).expect("reopen");
        assert_eq!(again.list().expect("list").len(), 1);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_profile_the_user_emptied_stays_empty_instead_of_reseeding() {
        // The bug a naive "seed when empty" repeats on every start: delete every card, restart, and the examples
        // come back as if the deletion never happened.
        let dir = temp_dir("emptied");
        let mut store = CardStore::open(&dir).expect("open");
        for card in store.list().expect("list") {
            assert!(store.remove(&card.id).expect("remove"));
        }
        assert!(store.list().expect("list").is_empty());
        drop(store);

        let reopened = CardStore::open(&dir).expect("reopen");
        assert!(
            reopened.list().expect("list").is_empty(),
            "an emptied profile is respected"
        );
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn an_unreadable_legacy_profile_stops_the_core_instead_of_being_replaced() {
        let dir = temp_dir("corrupt");
        fs::write(dir.join(JSON_FILE_NAME), "{ not a card list").expect("write");
        let error = CardStore::open(&dir).expect_err("the open must fail loudly");
        assert!(
            matches!(error, StoreError::Legacy { .. }),
            "the failure names the legacy document: {error}"
        );
        assert!(
            dir.join(JSON_FILE_NAME).exists(),
            "the unreadable file is kept for the user to fix"
        );
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn the_json_document_wins_over_a_stale_pre_switch_mirror() {
        // The real upgrade shape: an old build left a `user_version = 2` mirror that no longer agrees with the JSON
        // document it copied. Until the switch the JSON was the writer of record, so adoption must take its rows — not
        // the stale mirror's — and the mirror row must disappear.
        let dir = temp_dir("stale-mirror");
        let db = Db::open(&dir).expect("open");
        let mut stale = db_card("01STALE", "Régi tükör-sor");
        stale.container = "c-old".to_string();
        db.upsert_card(&stale).expect("write the stale mirror row");
        {
            let raw = rusqlite::Connection::open(dir.join(crate::sqlite_store::DB_FILE_NAME))
                .expect("raw open");
            raw.execute_batch("PRAGMA user_version = 2;")
                .expect("downgrade");
        }
        drop(db);
        let legacy = serde_json::json!([
            { "id": "01JSON", "title": "A JSON igazsága", "url": "https://pelda.hu/uj" }
        ]);
        fs::write(
            dir.join(JSON_FILE_NAME),
            serde_json::to_string_pretty(&legacy).expect("json"),
        )
        .expect("write the newer legacy document");

        let store = CardStore::open(&dir).expect("open");
        let cards = store.list().expect("list");
        assert_eq!(cards.len(), 1, "the mirror row is not kept beside the JSON");
        assert_eq!(cards[0].id, "01JSON");
        assert_eq!(cards[0].title, "A JSON igazsága");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn an_adoption_that_discards_rows_keeps_them_in_a_backup() {
        // The pre-switch database held a card the document no longer lists (a lagging mirror, or a document that lost
        // rows). The JSON wins — it was the writer — but the row it drops must remain recoverable, not vanish.
        let dir = temp_dir("adopt-backup");
        let db = Db::open(&dir).expect("open");
        db.upsert_card(&db_card("01LOST", "Csak az adatbázisban"))
            .expect("row");
        {
            let raw = rusqlite::Connection::open(dir.join(crate::sqlite_store::DB_FILE_NAME))
                .expect("raw open");
            raw.execute_batch("PRAGMA user_version = 2;")
                .expect("downgrade");
        }
        drop(db);
        fs::write(dir.join(JSON_FILE_NAME), "[]").expect("write the emptied document");

        let store = CardStore::open(&dir).expect("open");
        assert!(
            store.list().expect("list").is_empty(),
            "the document was the writer, so its empty list wins"
        );

        let backup = dir.join(ADOPTION_BACKUP_FILE_NAME);
        assert!(backup.exists(), "the discarded row is kept in a backup");
        let raw = rusqlite::Connection::open(&backup).expect("open the backup");
        let kept: i64 = raw
            .query_row("SELECT COUNT(*) FROM cards", [], |row| row.get(0))
            .expect("count");
        assert_eq!(kept, 1, "the backup holds the row the adoption dropped");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn opening_a_card_updates_its_timestamp_and_reorders_the_grid() {
        let dir = temp_dir("open");
        let mut store = CardStore::open(&dir).expect("open");
        let id = store.list().expect("list")[0].id.clone();
        let opened = store
            .mark_opened(&id, 1_700_000_000_000)
            .expect("open card");
        assert_eq!(
            opened.expect("known card").last_opened_at_ms,
            1_700_000_000_000
        );
        assert_eq!(
            store.list().expect("list")[0].id,
            id,
            "the most recent card comes first"
        );
        assert!(store
            .mark_opened("nope", 1)
            .expect("open unknown")
            .is_none());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn upsert_replaces_by_id_instead_of_duplicating() {
        let dir = temp_dir("upsert");
        let mut store = CardStore::open(&dir).expect("open");
        let mut card = store.list().expect("list")[0].clone();
        card.title = "Átnevezve".to_string();
        store.upsert(card).expect("upsert");
        let listed = store.list().expect("list");
        assert_eq!(listed.len(), 3, "no duplicate was created");
        assert!(listed.iter().any(|entry| entry.title == "Átnevezve"));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn cards_can_be_moved_out_of_a_container_in_one_write() {
        let dir = temp_dir("move-container");
        let mut store = CardStore::open(&dir).expect("open");
        let id = store.list().expect("list")[0].id.clone();
        let before = store.list().expect("list")[0].container.clone();
        assert_eq!(before, "c-dev", "the seed card starts in the dev container");

        // The move is explicit and counted; the record keeps everything else.
        let moved = store.move_container("c-dev", "c-private").expect("move");
        assert!(moved >= 1, "at least the seed card moved");
        let after = store
            .list()
            .expect("list")
            .into_iter()
            .find(|card| card.id == id)
            .expect("card");
        assert_eq!(after.container, "c-private");
        assert_eq!(after.title, "Antikolt projekt", "nothing else changed");

        // Moving out of a container nobody uses is a no-op, not an error.
        assert_eq!(
            store.move_container("c-nincs", "c-private").expect("move"),
            0
        );
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn page_text_is_captured_kept_and_capped() {
        let dir = temp_dir("page-text");
        let mut store = CardStore::open(&dir).expect("open");
        let id = store.list().expect("list")[0].id.clone();

        // A capture stores what the page said, with runs of whitespace collapsed.
        let stored = store
            .set_page_text(&id, "első   sor\n\nmásodik sor")
            .expect("set")
            .expect("the card exists");
        assert_eq!(stored.page_text, "első sor második sor");

        // A pin or a note arrives from the page, which never carries the text: the capture must survive it.
        let mut card = stored.clone();
        card.pinned = true;
        card.notes = "jegyzet".to_string();
        card.page_text = String::new();
        let after_pin = store.upsert(card).expect("upsert");
        assert_eq!(
            after_pin.page_text, "első sor második sor",
            "a pin must not delete the capture"
        );

        // The cap is about the stored row, not the page.
        let long = "a".repeat(MAX_PAGE_TEXT + 500);
        let capped = store.set_page_text(&id, &long).expect("set").expect("card");
        assert_eq!(capped.page_text.chars().count(), MAX_PAGE_TEXT);

        // An unknown id is answerable without a write.
        assert!(store
            .set_page_text("nincs-ilyen", "x")
            .expect("set")
            .is_none());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn tags_are_trimmed_deduplicated_and_capped() {
        let dir = temp_dir("tags");
        let mut store = CardStore::open(&dir).expect("open");
        let mut card = store.list().expect("list")[0].clone();
        card.id = "tag-probe".to_string();
        card.tags = vec![
            "  munka ".to_string(),
            "MUNKA".to_string(),
            "".to_string(),
            "a".repeat(MAX_TAG_LEN + 10),
        ];
        let saved = store.upsert(card).expect("upsert");
        assert_eq!(
            saved.tags.len(),
            2,
            "the duplicate and the empty tag are dropped"
        );
        assert_eq!(saved.tags[0], "munka", "the first spelling wins, trimmed");
        assert_eq!(
            saved.tags[1].chars().count(),
            MAX_TAG_LEN,
            "a tag is capped, not refused"
        );

        // The cap is a count, not an error: the first MAX_TAGS survive.
        let mut many = saved.clone();
        many.tags = (0..40).map(|index| format!("tag{index}")).collect();
        let capped = store.upsert(many).expect("upsert");
        assert_eq!(capped.tags.len(), MAX_TAGS);
        assert_eq!(capped.tags[0], "tag0");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn the_export_is_a_faithful_copy_the_report_can_verify() {
        let dir = temp_dir("export");
        let mut store = CardStore::open(&dir).expect("open");
        let mut card = store.list().expect("list")[0].clone();
        card.page_text = "export-próba".to_string();
        store.upsert(card.clone()).expect("write");

        let report = store.storage_report(true).expect("report");
        assert!(report.verified, "the export round-trips: {report:?}");
        assert_eq!(report.checked, 3, "every row is compared, not a sample");
        assert_eq!(report.mismatches, 0);
        assert_eq!(report.schema, 3);

        // The file really contains the cards, and in the grid's order.
        let text = fs::read_to_string(&report.export_path).expect("read the export");
        let exported: Vec<Card> = serde_json::from_str(&text).expect("parse");
        assert_eq!(exported, store.list().expect("list"));

        // A file that no longer agrees is caught row by row instead of being called agreement.
        fs::write(&report.export_path, "[]").expect("clobber the export");
        let stale = store.storage_report(false).expect("report");
        assert!(!stale.verified);
        assert!(stale.mismatches > 0);
        let _ = fs::remove_dir_all(&dir);
    }
}
