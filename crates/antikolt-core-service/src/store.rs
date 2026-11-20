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

/// How much of a page's text one card may keep (`card_page_text`, ADR-049). The card list is a single JSON
/// document, so this is a promise about the file's size, not about the page's length.
pub const MAX_PAGE_TEXT: usize = 16_000;

/// How many tags one card may carry, and how long one tag may be (`card_upsert`, ADR-055). A tag is a word or a
/// short phrase the user typed; the limits are what keeps a pasted paragraph from becoming a "tag".
pub const MAX_TAGS: usize = 12;
pub const MAX_TAG_LEN: usize = 32;

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
    /// `MAX_PAGE_TEXT`): the card list is one JSON document, and a searchable index is not worth a 200 MB file.
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
        fs::create_dir_all(data_dir)?;
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
    pub fn upsert(&mut self, mut card: Card) -> Result<Card, StoreError> {
        card.page_text = Self::cap_page_text(&card.page_text);
        card.tags = Self::normalise_tags(&card.tags);
        match self.cards.iter_mut().find(|entry| entry.id == card.id) {
            Some(existing) => {
                if card.page_text.is_empty() {
                    card.page_text = existing.page_text.clone();
                }
                *existing = card.clone();
            }
            None => self.cards.push(card.clone()),
        }
        self.save()?;
        Ok(card)
    }

    /// Store the page's text for one card (`card_page_text`, ADR-049). An unknown id is not an error at the
    /// store level — the caller decides what a missing card means.
    pub fn set_page_text(&mut self, card_id: &str, text: &str) -> Result<Option<Card>, StoreError> {
        let Some(card) = self.cards.iter_mut().find(|entry| entry.id == card_id) else {
            return Ok(None);
        };
        card.page_text = Self::cap_page_text(text);
        let stored = card.clone();
        self.save()?;
        Ok(Some(stored))
    }

    /// Move every card from one container to another (`container_delete` with `move_to`, ADR-054), in one save.
    ///
    /// This is the explicit exit from the "a used container is not deletable" rule: the caller has to say where
    /// the cards go, so nothing disappears by accident, and the count is returned for the reply.
    pub fn move_container(&mut self, from: &str, to: &str) -> Result<usize, StoreError> {
        let mut moved = 0usize;
        for card in self.cards.iter_mut() {
            if card.container == from {
                card.container = to.to_string();
                moved += 1;
            }
        }
        if moved > 0 {
            self.save()?;
        }
        Ok(moved)
    }

    /// Trim, de-duplicate (case-insensitively, keeping the first spelling) and cap the tags (ADR-055).
    ///
    /// The store is where every writer meets: the page, a future importer and a test. A rule that only lives in
    /// the page is a rule the importer will not follow.
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

    /// The text a search may see. The cap is a promise about the *file*, not about the page: this is far more
    /// than any title or note, and even five hundred of them stay small enough to keep the card list a list.
    fn cap_page_text(text: &str) -> String {
        let collapsed = text.split_whitespace().collect::<Vec<_>>().join(" ");
        collapsed.chars().take(MAX_PAGE_TEXT).collect()
    }

    /// Remove one card (`card_delete`, ADR-061). An unknown id is `Ok(false)`: the caller decides what a missing
    /// card means, and a delete that says "gone" twice must not be an error the second time.
    pub fn remove(&mut self, card_id: &str) -> Result<bool, StoreError> {
        let before = self.cards.len();
        self.cards.retain(|card| card.id != card_id);
        if self.cards.len() == before {
            return Ok(false);
        }
        self.save()?;
        Ok(true)
    }

    /// Replace the whole list, for an import (`profile_import`, ADR-059). Every card goes through the same
    /// normalisation a write does, so an imported profile cannot smuggle in a rule the store would refuse.
    pub fn replace_all(&mut self, cards: Vec<Card>) -> Result<usize, StoreError> {
        self.cards = cards
            .into_iter()
            .map(|mut card| {
                card.page_text = Self::cap_page_text(&card.page_text);
                card.tags = Self::normalise_tags(&card.tags);
                card
            })
            .collect();
        let count = self.cards.len();
        self.save()?;
        Ok(count)
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

    #[test]
    fn cards_can_be_moved_out_of_a_container_in_one_save() {
        let dir = temp_dir("move-container");
        let mut store = CardStore::open(&dir).expect("open");
        let id = store.list()[0].id.clone();
        let before = store.list()[0].container.clone();
        assert_eq!(before, "c-dev", "the seed card starts in the dev container");

        // The move is explicit and counted; the record keeps everything else.
        let moved = store.move_container("c-dev", "c-private").expect("move");
        assert!(moved >= 1, "at least the seed card moved");
        let after = store
            .list()
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
        let id = store.list()[0].id.clone();

        // A capture stores what the page said, with runs of whitespace collapsed (the search does not care
        // about line breaks, and the JSON file does).
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

        // The cap is about the file, not the page.
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
        let mut card = store.list()[0].clone();
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
}
