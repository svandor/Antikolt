// SPDX-License-Identifier: MPL-2.0

//! The card store the core service serves.
//!
//! Storage is a single JSON document for now. The schema in `antikolt_core::migrations` is the
//! destination (SQLite); this module exists so the shell can already show real cards instead of a
//! mock, and so the shape of a card is decided in one place. The file is rewritten atomically (write
//! a temporary file, then replace), because a half-written card list is worse than an old one.

use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

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
}

/// The card store: a list of cards backed by one JSON file.
#[derive(Debug)]
pub struct CardStore {
    path: PathBuf,
    cards: Vec<Card>,
}

impl CardStore {
    /// Open the store at `data_dir/cards.json`, seeding it with the example cards on first run.
    ///
    /// Seeding is deliberate: an empty grid on the very first start looks like a bug, and the user has
    /// nothing to click. The examples are clearly editable data, not hidden state.
    pub fn open(data_dir: &Path) -> Result<Self, StoreError> {
        let path = data_dir.join("cards.json");
        if !path.exists() {
            let store = Self {
                path,
                cards: seed_cards(),
            };
            store.save()?;
            return Ok(store);
        }
        let text = fs::read_to_string(&path)?;
        let cards: Vec<Card> = serde_json::from_str(&text)?;
        Ok(Self { path, cards })
    }

    /// Every card, newest-opened first, then by title — the grid's default order.
    pub fn list(&self) -> Vec<Card> {
        let mut cards = self.cards.clone();
        cards.sort_by(|a, b| {
            b.last_opened_at_ms
                .cmp(&a.last_opened_at_ms)
                .then_with(|| a.title.to_lowercase().cmp(&b.title.to_lowercase()))
        });
        cards
    }

    /// Record that a card was opened now. Returns the card, or `None` when the id is unknown.
    pub fn mark_opened(&mut self, id: &str, now_ms: u64) -> Result<Option<Card>, StoreError> {
        let Some(card) = self.cards.iter_mut().find(|card| card.id == id) else {
            return Ok(None);
        };
        card.last_opened_at_ms = now_ms;
        let updated = card.clone();
        self.save()?;
        Ok(Some(updated))
    }

    /// Insert or replace a card by id. The grid's editor will use this.
    pub fn upsert(&mut self, card: Card) -> Result<Card, StoreError> {
        match self.cards.iter_mut().find(|entry| entry.id == card.id) {
            Some(existing) => *existing = card.clone(),
            None => self.cards.push(card.clone()),
        }
        self.save()?;
        Ok(card)
    }

    /// Write the whole list out atomically.
    fn save(&self) -> Result<(), StoreError> {
        let text = serde_json::to_string_pretty(&self.cards)?;
        if let Some(parent) = self.path.parent() {
            fs::create_dir_all(parent)?;
        }
        let temporary = self.path.with_extension("json.tmp");
        {
            let mut file = fs::File::create(&temporary)?;
            file.write_all(text.as_bytes())?;
            // The rename below is only atomic if the bytes really reached the disk first.
            file.sync_all()?;
        }
        fs::rename(&temporary, &self.path)?;
        Ok(())
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

    #[test]
    fn a_fresh_store_is_seeded_and_survives_a_reopen() {
        let dir = temp_dir("seed");
        let store = CardStore::open(&dir).expect("open");
        assert_eq!(store.list().len(), 3, "first run seeds the examples");
        assert!(
            dir.join("cards.json").exists(),
            "the seed is written to disk"
        );

        let reopened = CardStore::open(&dir).expect("reopen");
        assert_eq!(reopened.list(), store.list(), "the file round-trips");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn opening_a_card_updates_its_timestamp_and_reorders_the_grid() {
        let dir = temp_dir("open");
        let mut store = CardStore::open(&dir).expect("open");
        let id = store.list()[0].id.clone();
        let opened = store
            .mark_opened(&id, 1_700_000_000_000)
            .expect("open card");
        assert_eq!(
            opened.expect("known card").last_opened_at_ms,
            1_700_000_000_000
        );
        assert_eq!(store.list()[0].id, id, "the most recent card comes first");
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
        let mut card = store.list()[0].clone();
        card.title = "Átnevezve".to_string();
        store.upsert(card.clone()).expect("upsert");
        assert_eq!(store.list().len(), 3, "no duplicate was created");
        assert!(store.list().iter().any(|entry| entry.title == "Átnevezve"));
        let _ = fs::remove_dir_all(&dir);
    }
}
