// SPDX-License-Identifier: MPL-2.0

//! The session ledger on disk (`<data_dir>/sessions.json`).
//!
//! The *logic* lives in `antikolt_core::session` (policies, TTLs, what a cleanup touches); this module
//! is only the storage and the ledger's two rules:
//!
//! 1. a row that cannot be true is refused instead of written (`SessionLedgerEntry::validate`), because
//!    a ledger that lies is worse than a ledger that is missing a row;
//! 2. the file is rewritten atomically (temp file + rename), exactly like the card store — a crash must
//!    never leave half a ledger behind.
//!
//! `docs/HU/10-MUNKAMENET.md` §4 is the specification; ADR-033 records why the policy is evaluated
//! here and not in the shell.

use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

use antikolt_core::session::{
    plan_cleanup, CleanupPlan, LedgerError, SessionLedgerEntry, SessionRules,
};
use serde::{Deserialize, Serialize};

/// The whole ledger: the container-wide rules plus one row per (container, origin).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct LedgerFile {
    /// Defaults when the file predates a field: the policy module's own defaults.
    #[serde(default)]
    pub rules: SessionRules,
    #[serde(default)]
    pub entries: Vec<SessionLedgerEntry>,
}

/// One row that is due, together with the plan that would run for it.
///
/// The plan travels with the row so the caller cannot pick the wrong storage set: the decision and
/// what it touches come from the same evaluation.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct DueEntry {
    pub session: SessionLedgerEntry,
    pub plan: CleanupPlan,
}

/// Everything that can go wrong with the ledger.
#[derive(Debug, thiserror::Error)]
pub enum SessionError {
    #[error("cannot read or write the session ledger: {0}")]
    Io(#[from] std::io::Error),
    #[error("the session ledger is not valid JSON: {0}")]
    Json(#[from] serde_json::Error),
    #[error("the ledger refused the row: {0}")]
    Rejected(#[from] LedgerError),
    /// A rule that is not a policy. Its own variant, so the reply says what was wrong instead of
    /// borrowing a row-level error (a TTL typo reported as "unknown state" would be a lie).
    #[error("the session rules were refused: {0}")]
    Rules(&'static str),
}

impl SessionError {
    /// `true` when the request itself was wrong, rather than the storage.
    pub fn is_invalid_argument(&self) -> bool {
        matches!(self, Self::Rejected(_) | Self::Rules(_))
    }
}

/// The ledger file, in memory and on disk.
#[derive(Debug)]
pub struct SessionStore {
    path: PathBuf,
    file: LedgerFile,
}

impl SessionStore {
    /// Open the ledger, creating an empty one when the file does not exist yet.
    pub fn open(data_dir: &Path) -> Result<Self, SessionError> {
        let path = data_dir.join("sessions.json");
        if !path.exists() {
            let store = Self {
                path,
                file: LedgerFile {
                    rules: SessionRules::default(),
                    entries: Vec::new(),
                },
            };
            store.save()?;
            return Ok(store);
        }
        let text = fs::read_to_string(&path)?;
        let file: LedgerFile = serde_json::from_str(&text)?;
        Ok(Self { path, file })
    }

    /// The container-wide rules.
    pub fn rules(&self) -> SessionRules {
        self.file.rules
    }

    /// Replace the container-wide rules, after checking them.
    ///
    /// The check is here, not in the caller: a TTL of a thousand years is a typo, not a policy, and it would
    /// be stored as one. `ttl_ms` must be a real retention window (an hour to thirty days), because a zero
    /// TTL would sweep every session the moment it is written.
    pub fn set_rules(&mut self, rules: SessionRules) -> Result<SessionRules, SessionError> {
        const MIN_TTL_MS: u64 = 60 * 60 * 1000;
        const MAX_TTL_MS: u64 = 30 * 24 * 60 * 60 * 1000;
        if rules.ttl_ms < MIN_TTL_MS || rules.ttl_ms > MAX_TTL_MS {
            return Err(SessionError::Rules(
                "the TTL must be between one hour and thirty days",
            ));
        }
        self.file.rules = rules;
        self.save()?;
        Ok(rules)
    }

    /// Rows ordered by origin, so the panel and the reply are stable.
    pub fn list(&self) -> Vec<SessionLedgerEntry> {
        let mut entries = self.file.entries.clone();
        entries.sort_by(|left, right| left.origin.cmp(&right.origin));
        entries
    }

    /// Insert or replace one row, after the ledger's own check.
    ///
    /// The check runs *before* the write: a refused row must not reach the file, not even briefly.
    pub fn upsert(
        &mut self,
        entry: SessionLedgerEntry,
    ) -> Result<SessionLedgerEntry, SessionError> {
        entry.validate()?;
        match self
            .file
            .entries
            .iter_mut()
            .find(|row| row.container_id == entry.container_id && row.origin == entry.origin)
        {
            Some(existing) => *existing = entry.clone(),
            None => self.file.entries.push(entry.clone()),
        }
        self.save()?;
        Ok(entry)
    }

    /// Drop one row. `false` means there was nothing to drop.
    pub fn close(&mut self, container_id: &str, origin: &str) -> Result<bool, SessionError> {
        let before = self.file.entries.len();
        self.file
            .entries
            .retain(|row| !(row.container_id == container_id && row.origin == origin));
        if self.file.entries.len() == before {
            return Ok(false);
        }
        self.save()?;
        Ok(true)
    }

    /// Which rows a cleanup round would run, given the clock and whether the app is locked.
    ///
    /// A row the user asked to keep is never due; a row without a last-seen time cannot age, so it is
    /// not due either. The row carries the policy resolved for it when it was written (card override
    /// or container default), so this does not re-resolve it and cannot drift from the panel.
    pub fn due(&self, now_ms: u64, app_locked: bool) -> Vec<DueEntry> {
        let rules = self.file.rules;
        let mut due = Vec::new();
        for entry in &self.file.entries {
            if entry.keep_alive {
                continue;
            }
            let Some(last_seen_ms) = entry.last_seen_ms else {
                continue;
            };
            let Some(plan) =
                plan_cleanup(&rules, Some(entry.policy), last_seen_ms, now_ms, app_locked)
            else {
                continue;
            };
            if plan.is_noop() {
                continue;
            }
            due.push(DueEntry {
                session: entry.clone(),
                plan,
            });
        }
        due.sort_by(|left, right| left.session.origin.cmp(&right.session.origin));
        due
    }

    /// The atomic write: a crash leaves the previous ledger, never a mixture of two.
    fn save(&self) -> Result<(), SessionError> {
        let text = serde_json::to_string_pretty(&self.file)?;
        if let Some(parent) = self.path.parent() {
            fs::create_dir_all(parent)?;
        }
        let temporary = self.path.with_extension("json.tmp");
        {
            let mut file = fs::File::create(&temporary)?;
            file.write_all(text.as_bytes())?;
            file.sync_all()?;
        }
        fs::rename(&temporary, &self.path)?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use std::sync::atomic::{AtomicUsize, Ordering};

    use antikolt_core::session::{LoginState, SessionPolicy};

    use super::*;

    const DAY_MS: u64 = 24 * 60 * 60 * 1000;

    static SCRATCH: AtomicUsize = AtomicUsize::new(0);

    /// A directory nobody else uses, removed by the caller.
    fn scratch_dir() -> PathBuf {
        let unique = SCRATCH.fetch_add(1, Ordering::Relaxed);
        let dir =
            std::env::temp_dir().join(format!("antikolt-sessions-{}-{unique}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).expect("scratch dir");
        dir
    }

    fn row(origin: &str) -> SessionLedgerEntry {
        SessionLedgerEntry {
            container_id: "c-private".to_string(),
            origin: origin.to_string(),
            state: LoginState::Expired,
            policy: SessionPolicy::Sweep,
            cookie_count: 12,
            cookie_bytes: 4096,
            storage_bytes: 65536,
            service_worker_count: 1,
            last_seen_ms: Some(0),
            last_verified_ms: Some(0),
            live_ram_bytes: None,
            keep_alive: false,
        }
    }

    fn rules_with_ttl(ttl_ms: u64) -> SessionRules {
        SessionRules {
            container_default: SessionPolicy::Sweep,
            ttl_ms,
            wipe_on_app_lock: false,
        }
    }

    #[test]
    fn a_fresh_ledger_is_empty_and_survives_a_reopen() {
        let dir = scratch_dir();
        let mut store = SessionStore::open(&dir).expect("open");
        assert!(store.list().is_empty());
        assert_eq!(store.rules(), SessionRules::default());
        assert!(dir.join("sessions.json").exists());

        store.upsert(row("https://example.org")).expect("upsert");
        drop(store);

        let reopened = SessionStore::open(&dir).expect("reopen");
        assert_eq!(reopened.list(), vec![row("https://example.org")]);
        fs::remove_dir_all(&dir).expect("cleanup");
    }

    #[test]
    fn the_ledger_refuses_rows_that_would_make_it_lie() {
        let dir = scratch_dir();
        let mut store = SessionStore::open(&dir).expect("open");

        let empty_origin = store.upsert(row("  ")).expect_err("empty origin");
        assert!(matches!(
            empty_origin,
            SessionError::Rejected(LedgerError::EmptyOrigin)
        ));
        let not_http = store
            .upsert(row("file:///etc/passwd"))
            .expect_err("file url");
        assert!(matches!(
            not_http,
            SessionError::Rejected(LedgerError::NonHttpOrigin(_))
        ));
        let mut no_container = row("https://example.org");
        no_container.container_id.clear();
        let no_container = store.upsert(no_container).expect_err("no container");
        assert!(matches!(
            no_container,
            SessionError::Rejected(LedgerError::EmptyContainer)
        ));

        // None of the refused rows reached the file.
        assert!(store.list().is_empty());
        assert!(SessionStore::open(&dir).expect("reopen").list().is_empty());
        fs::remove_dir_all(&dir).expect("cleanup");
    }

    #[test]
    fn a_row_is_due_only_after_its_ttl_and_a_kept_row_is_never_due() {
        let dir = scratch_dir();
        let mut store = SessionStore::open(&dir).expect("open");
        store.file.rules = rules_with_ttl(DAY_MS);
        store.upsert(row("https://example.org")).expect("upsert");

        assert!(store.due(DAY_MS - 1, false).is_empty());
        let due = store.due(DAY_MS, false);
        assert_eq!(due.len(), 1);
        assert_eq!(due[0].session.origin, "https://example.org");
        // Sweep keeps the login: it is a cleanup, not a sign-out.
        assert!(!due[0].plan.signs_out());
        let touched = due[0].plan.storage_types();
        for storage in [
            "session_storage",
            "cache_storage",
            "service_workers",
            "code_cache",
        ] {
            assert!(touched.contains(&storage), "sweep should touch {storage}");
        }
        assert!(
            !touched.contains(&"cookies"),
            "sweep must not sign the user out"
        );

        let mut kept = row("https://keep.example");
        kept.keep_alive = true;
        store.upsert(kept).expect("upsert kept");
        assert_eq!(store.due(DAY_MS * 7, false).len(), 1);
        fs::remove_dir_all(&dir).expect("cleanup");
    }

    #[test]
    fn closing_a_row_removes_it_once() {
        let dir = scratch_dir();
        let mut store = SessionStore::open(&dir).expect("open");
        store.upsert(row("https://example.org")).expect("upsert");

        assert!(store
            .close("c-private", "https://example.org")
            .expect("close"));
        assert!(!store
            .close("c-private", "https://example.org")
            .expect("close again"));
        assert!(store.list().is_empty());
        fs::remove_dir_all(&dir).expect("cleanup");
    }
}
