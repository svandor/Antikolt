// SPDX-License-Identifier: MPL-2.0

//! The portable profile bundle: one JSON file the user can carry to another machine (`profile_export` /
//! `profile_import`, ADR-059).
//!
//! The bundle is a *copy of the truth*, not a second truth: it is written from the stores and read back into
//! them, and both directions report what they moved. It carries a schema number, so a future change can refuse an
//! old file instead of mis-reading it, and the import validates before it writes anything.
//!
//! Why JSON and not a zip: the four stores are already JSON documents, the bundle is small (a card list), and a
//! format a person can open and read in a text editor is a format they can trust enough to back up.

use std::fs;
use std::io::Write;
use std::path::Path;

use antikolt_core::session::{SessionLedgerEntry, SessionRules};
use serde::{Deserialize, Serialize};

use crate::containers::Container;
use crate::sqlite_store::UserPrefs;
use crate::store::Card;
use crate::workspaces::Workspace;

/// The bundle's own schema. Bumped when a field's meaning changes; an unknown number is refused, not guessed.
pub const BUNDLE_SCHEMA: u32 = 1;

#[derive(Debug, thiserror::Error)]
pub enum BundleError {
    #[error("cannot read or write the bundle: {0}")]
    Io(#[from] std::io::Error),
    #[error("the bundle is not valid JSON: {0}")]
    Json(#[from] serde_json::Error),
    #[error("unsupported bundle schema: {0} (this core writes {BUNDLE_SCHEMA})")]
    Schema(u32),
}

/// Everything a profile's user data consists of.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Bundle {
    pub schema: u32,
    pub created_at_ms: u64,
    pub cards: Vec<Card>,
    pub containers: Vec<Container>,
    pub workspaces: Vec<Workspace>,
    pub sessions: Vec<SessionLedgerEntry>,
    pub rules: SessionRules,
    /// The user preferences at export time. `None` in a bundle written before this field existed — import then keeps
    /// the profile's current prefs instead of overwriting them with defaults (additive field, no schema bump).
    #[serde(default)]
    pub prefs: Option<UserPrefs>,
}

impl Bundle {
    pub fn new(
        created_at_ms: u64,
        cards: Vec<Card>,
        containers: Vec<Container>,
        workspaces: Vec<Workspace>,
        sessions: Vec<SessionLedgerEntry>,
        rules: SessionRules,
        prefs: UserPrefs,
    ) -> Self {
        Self {
            schema: BUNDLE_SCHEMA,
            created_at_ms,
            cards,
            containers,
            workspaces,
            sessions,
            rules,
            prefs: Some(prefs),
        }
    }

    /// Write the bundle atomically: a half-written backup is worse than an old one, exactly like the stores.
    pub fn write(&self, path: &Path) -> Result<(), BundleError> {
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)?;
        }
        let text = serde_json::to_string_pretty(self)?;
        let temporary = path.with_extension("json.tmp");
        {
            let mut file = fs::File::create(&temporary)?;
            file.write_all(text.as_bytes())?;
            file.sync_all()?;
        }
        fs::rename(&temporary, path)?;
        Ok(())
    }

    /// Read a bundle and refuse one this core cannot understand. The check happens before any store is touched,
    /// because an import that half-succeeds is worse than one that does not start.
    pub fn read(path: &Path) -> Result<Self, BundleError> {
        let text = fs::read_to_string(path)?;
        let bundle: Self = serde_json::from_str(&text)?;
        if bundle.schema != BUNDLE_SCHEMA {
            return Err(BundleError::Schema(bundle.schema));
        }
        Ok(bundle)
    }

    /// How many of each thing the bundle holds, for the reply a shell shows.
    pub fn counts(&self) -> (usize, usize, usize, usize) {
        (
            self.cards.len(),
            self.containers.len(),
            self.workspaces.len(),
            self.sessions.len(),
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_dir(name: &str) -> std::path::PathBuf {
        let stamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .expect("clock")
            .as_nanos();
        let dir = std::env::temp_dir().join(format!("antikolt-bundle-{name}-{stamp}"));
        fs::create_dir_all(&dir).expect("temp dir");
        dir
    }

    #[test]
    fn a_bundle_round_trips_and_a_wrong_schema_is_refused() {
        let dir = temp_dir("roundtrip");
        let path = dir.join("profile.antikolt.json");
        let bundle = Bundle::new(
            1_700_000_000_000,
            Vec::new(),
            Vec::new(),
            Vec::new(),
            Vec::new(),
            SessionRules::default(),
            UserPrefs::defaults(),
        );
        bundle.write(&path).expect("write");
        let read = Bundle::read(&path).expect("read");
        assert_eq!(read.schema, BUNDLE_SCHEMA);
        assert_eq!(read.created_at_ms, 1_700_000_000_000);

        // An older bundle is refused with a message that says what this core speaks, not a confusing serde error.
        let mut text = fs::read_to_string(&path).expect("read text");
        text = text.replace("\"schema\": 1", "\"schema\": 99");
        fs::write(&path, text).expect("rewrite");
        match Bundle::read(&path) {
            Err(BundleError::Schema(99)) => {}
            other => panic!("expected a schema refusal, got {other:?}"),
        }
        fs::remove_dir_all(dir).ok();
    }
}
