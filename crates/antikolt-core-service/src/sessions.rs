// SPDX-License-Identifier: MPL-2.0

//! The session ledger (`<data_dir>/sessions.json` export; rows in SQLite, ADR-070).
//!
//! The *logic* lives in `antikolt_core::session` (policies, TTLs, what a cleanup touches); this module is only the
//! storage and the ledger's two rules:
//!
//! 1. a row that cannot be true is refused instead of written (`SessionLedgerEntry::validate`), because a ledger that
//!    lies is worse than a ledger that is missing a row;
//! 2. the store commits through SQLite, and the JSON document beside it is a derived export written atomically —
//!    a crash must never leave half a ledger behind, and after the switch nothing reads the document back.
//!
//! `docs/HU/10-MUNKAMENET.md` §4 is the specification; ADR-033 records why the policy is evaluated here and not in
//! the shell.

use std::fs;
use std::path::{Path, PathBuf};

use antikolt_core::session::{
    plan_cleanup, CleanupPlan, LedgerError, SessionLedgerEntry, SessionRules,
};
use serde::{Deserialize, Serialize};

use crate::persist::write_json_atomic;
use crate::sqlite_store::{Db, DbError, UserPrefs};

/// The export beside the database: the legacy ledger document, and the readable copy this build keeps.
pub const FILE_NAME: &str = "sessions.json";

/// The whole ledger as the export writes it: the container-wide rules plus one row per (container, origin).
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
/// The plan travels with the row so the caller cannot pick the wrong storage set: the decision and what it touches
/// come from the same evaluation.
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
    /// The pre-switch document could not be read: it was the only copy of the ledger at that moment.
    #[error("cannot read the session ledger written by an earlier version ({path}): {source}")]
    Legacy {
        path: PathBuf,
        #[source]
        source: serde_json::Error,
    },
    #[error("the ledger refused the row: {0}")]
    Rejected(#[from] LedgerError),
    /// A rule that is not a policy. Its own variant, so the reply says what was wrong instead of borrowing a
    /// row-level error (a TTL typo reported as "unknown state" would be a lie).
    #[error("the session rules were refused: {0}")]
    Rules(&'static str),
    #[error("the database refused the session ledger: {0}")]
    Db(#[from] DbError),
}

impl SessionError {
    /// `true` when the request itself was wrong, rather than the storage.
    pub fn is_invalid_argument(&self) -> bool {
        matches!(self, Self::Rejected(_) | Self::Rules(_))
    }
}

/// The ledger: rows in SQLite, with `sessions.json` as a derived export.
#[derive(Debug)]
pub struct SessionStore {
    data_dir: PathBuf,
    db: Db,
}

impl SessionStore {
    /// Open the ledger, adopting the pre-switch `sessions.json` once (ADR-070). A missing document starts an empty
    /// ledger with the policy module's defaults.
    pub fn open(data_dir: &Path) -> Result<Self, SessionError> {
        fs::create_dir_all(data_dir)?;
        let db = Db::open(data_dir)?;
        let mut store = Self {
            data_dir: data_dir.to_path_buf(),
            db,
        };
        if !store.db.was_adopted("sessions")? {
            store.adopt_legacy()?;
            store.db.mark_adopted("sessions")?;
        }
        Ok(store)
    }

    /// The export's path inside the profile.
    pub fn path(&self) -> PathBuf {
        self.data_dir.join(FILE_NAME)
    }

    /// The container-wide rules. A profile that never stored any gets the policy module's defaults — `None` in the
    /// table means "nothing was chosen", not "an empty policy".
    pub fn rules(&self) -> Result<SessionRules, SessionError> {
        Ok(self.db.session_rules()?.unwrap_or_default())
    }

    /// Replace the container-wide rules, after checking them.
    ///
    /// The check is here, not in the caller: a TTL of a thousand years is a typo, not a policy, and it would be stored
    /// as one. `ttl_ms` must be a real retention window (an hour to thirty days), because a zero TTL would sweep every
    /// session the moment it is written.
    pub fn set_rules(&mut self, rules: SessionRules) -> Result<SessionRules, SessionError> {
        const MIN_TTL_MS: u64 = 60 * 60 * 1000;
        const MAX_TTL_MS: u64 = 30 * 24 * 60 * 60 * 1000;
        if rules.ttl_ms < MIN_TTL_MS || rules.ttl_ms > MAX_TTL_MS {
            return Err(SessionError::Rules(
                "the TTL must be between one hour and thirty days",
            ));
        }
        self.db.set_session_rules(&rules)?;
        Ok(rules)
    }

    /// The user preferences: one row of live-card cap and hibernation timings. A profile that never chose anything
    /// gets the factory defaults.
    pub fn user_prefs(&self) -> Result<UserPrefs, SessionError> {
        Ok(self.db.user_prefs()?)
    }

    /// Replace the one row of user preferences, after checking them.
    pub fn set_user_prefs(&mut self, prefs: UserPrefs) -> Result<UserPrefs, SessionError> {
        prefs.validate().map_err(SessionError::Rules)?;
        self.db.set_user_prefs(&prefs)?;
        Ok(prefs)
    }

    /// Rows ordered by origin, so the panel and the reply are stable (SQL does the ordering).
    pub fn list(&self) -> Result<Vec<SessionLedgerEntry>, SessionError> {
        Ok(self.db.all_sessions()?)
    }

    /// Insert or replace one row, after the ledger's own check.
    ///
    /// The check runs *before* the write: a refused row must not reach the store, not even briefly.
    pub fn upsert(
        &mut self,
        entry: SessionLedgerEntry,
    ) -> Result<SessionLedgerEntry, SessionError> {
        entry.validate()?;
        self.db.upsert_session(&entry)?;
        Ok(entry)
    }

    /// Drop one row. `false` means there was nothing to drop.
    pub fn close(&mut self, container_id: &str, origin: &str) -> Result<bool, SessionError> {
        Ok(self.db.delete_session(container_id, origin)?)
    }

    /// Which rows a cleanup round would run, given the clock and whether the app is locked.
    ///
    /// A row the user asked to keep is never due; a row without a last-seen time cannot age, so it is not due either.
    /// The row carries the policy resolved for it when it was written (card override or container default), so this
    /// does not re-resolve it and cannot drift from the panel.
    pub fn due(&self, now_ms: u64, app_locked: bool) -> Result<Vec<DueEntry>, SessionError> {
        let rules = self.rules()?;
        let mut due = Vec::new();
        for entry in self.list()? {
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
                session: entry,
                plan,
            });
        }
        Ok(due)
    }

    /// Replace the whole ledger, for an import (`profile_import`, ADR-059): rows and rules arrive together, because
    /// an imported profile's cleanup policy is part of the profile.
    pub fn replace_all(
        &mut self,
        entries: Vec<SessionLedgerEntry>,
        rules: SessionRules,
    ) -> Result<usize, SessionError> {
        let count = entries.len();
        self.db.replace_sessions(&entries, &rules)?;
        Ok(count)
    }

    /// Write `sessions.json` from the rows: one document, rules and rows together, exactly the shape the old ledger
    /// had. Nothing reads it after the switch.
    pub fn export_json(&self) -> Result<PathBuf, SessionError> {
        let path = self.path();
        let file = LedgerFile {
            rules: self.rules()?,
            entries: self.list()?,
        };
        write_json_atomic(&path, &file)?;
        Ok(path)
    }

    /// Compare the export with the store: `(checked, mismatches)`, `-1` meaning the file is unreadable
    /// (`not measured`). The rules count as one checked row, because they are part of what the document says.
    pub fn verify_export(&self) -> Result<(i64, i64), SessionError> {
        let expected_rules = self.rules()?;
        let expected = self.list()?;
        let checked = expected.len() as i64 + 1;
        let text = match fs::read_to_string(self.path()) {
            Ok(text) => text,
            Err(_) => return Ok((checked, -1)),
        };
        let exported: LedgerFile = match serde_json::from_str(&text) {
            Ok(exported) => exported,
            Err(_) => return Ok((checked, -1)),
        };
        let mut mismatches = 0i64;
        if exported.rules != expected_rules || exported.entries.len() != expected.len() {
            mismatches += 1;
        }
        for entry in &expected {
            match exported
                .entries
                .iter()
                .find(|row| row.container_id == entry.container_id && row.origin == entry.origin)
            {
                Some(row) if row == entry => {}
                _ => mismatches += 1,
            }
        }
        Ok((checked, mismatches))
    }

    /// Take the pre-switch document over exactly once: the JSON was the writer of record until this build, so its rows
    /// and rules win — and a corrupt one stops the core instead of being replaced with an empty ledger.
    fn adopt_legacy(&mut self) -> Result<(), SessionError> {
        let path = self.path();
        let file = if path.exists() {
            let text = fs::read_to_string(&path)?;
            serde_json::from_str::<LedgerFile>(&text).map_err(|source| SessionError::Legacy {
                path: path.clone(),
                source,
            })?
        } else {
            LedgerFile {
                rules: SessionRules::default(),
                entries: Vec::new(),
            }
        };
        self.db.replace_sessions(&file.entries, &file.rules)?;
        self.export_json()?;
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
        assert!(store.list().expect("list").is_empty());
        assert_eq!(store.rules().expect("rules"), SessionRules::default());
        assert!(dir.join(FILE_NAME).exists(), "the export is written");

        store.upsert(row("https://example.org")).expect("upsert");
        drop(store);

        let reopened = SessionStore::open(&dir).expect("reopen");
        assert_eq!(
            reopened.list().expect("list"),
            vec![row("https://example.org")]
        );
        drop(reopened);
        fs::remove_dir_all(&dir).expect("cleanup");
    }

    #[test]
    fn a_pre_switch_ledger_is_adopted_with_its_rules_once() {
        let dir = scratch_dir();
        // A profile written by the old build: one row, and a TTL the user chose.
        let legacy = LedgerFile {
            rules: rules_with_ttl(2 * DAY_MS),
            entries: vec![row("https://pelda.hu")],
        };
        fs::write(
            dir.join(FILE_NAME),
            serde_json::to_string_pretty(&legacy).expect("json"),
        )
        .expect("write the legacy ledger");

        let store = SessionStore::open(&dir).expect("open");
        assert_eq!(store.list().expect("list"), vec![row("https://pelda.hu")]);
        assert_eq!(
            store.rules().expect("rules"),
            rules_with_ttl(2 * DAY_MS),
            "the imported profile keeps its cleanup policy"
        );
        drop(store);

        // Once switched, the export is not adopted back: a row the user closed stays closed.
        let mut reopened = SessionStore::open(&dir).expect("reopen");
        assert!(reopened
            .close("c-private", "https://pelda.hu")
            .expect("close"));
        drop(reopened);
        let again = SessionStore::open(&dir).expect("reopen again");
        assert!(
            again.list().expect("list").is_empty(),
            "the switch is recorded, so the empty store is the truth"
        );
        drop(again);
        fs::remove_dir_all(&dir).expect("cleanup");
    }

    #[test]
    fn an_unreadable_legacy_ledger_stops_the_core() {
        let dir = scratch_dir();
        fs::write(dir.join(FILE_NAME), "{ not a ledger").expect("write");
        let error = SessionStore::open(&dir).expect_err("the open must fail loudly");
        assert!(
            matches!(error, SessionError::Legacy { .. }),
            "the failure names the document: {error}"
        );
        assert!(dir.join(FILE_NAME).exists());
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

        // None of the refused rows reached the store.
        assert!(store.list().expect("list").is_empty());
        assert!(SessionStore::open(&dir)
            .expect("reopen")
            .list()
            .expect("list")
            .is_empty());
        drop(store);
        fs::remove_dir_all(&dir).expect("cleanup");
    }

    #[test]
    fn a_row_is_due_only_after_its_ttl_and_a_kept_row_is_never_due() {
        let dir = scratch_dir();
        let mut store = SessionStore::open(&dir).expect("open");
        store
            .set_rules(rules_with_ttl(DAY_MS))
            .expect("set the TTL");
        store.upsert(row("https://example.org")).expect("upsert");

        assert!(store.due(DAY_MS - 1, false).expect("due").is_empty());
        let due = store.due(DAY_MS, false).expect("due");
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
        assert_eq!(store.due(DAY_MS * 7, false).expect("due").len(), 1);

        // A refused TTL is not a policy, and the old one survives the refusal.
        assert!(store.set_rules(rules_with_ttl(1)).is_err());
        assert_eq!(store.rules().expect("rules"), rules_with_ttl(DAY_MS));
        drop(store);
        fs::remove_dir_all(&dir).expect("cleanup");
    }

    #[test]
    fn a_rows_own_policy_overrides_the_container_default() {
        let dir = scratch_dir();
        let mut store = SessionStore::open(&dir).expect("open");
        // The container default is sweep (keep the login), but one origin is a bank: it asked for wipe.
        store.set_rules(rules_with_ttl(DAY_MS)).expect("set rules");
        let mut bank = row("https://bank.example");
        bank.policy = SessionPolicy::Wipe;
        store.upsert(bank).expect("upsert the wipe");

        let due = store.due(DAY_MS, false).expect("due");
        assert_eq!(due.len(), 1);
        assert_eq!(due[0].session.origin, "https://bank.example");
        // The per-origin wipe wins over the sweep default: it signs the user out.
        assert!(
            due[0].plan.signs_out(),
            "a per-origin wipe must sign out even when the container default is sweep"
        );
        assert!(
            due[0].plan.storage_types().contains(&"cookies"),
            "wipe must clear the cookies"
        );

        // The inverse direction too: a keep override wins over a wipe container default.
        let mut wipe_rules = rules_with_ttl(DAY_MS);
        wipe_rules.container_default = SessionPolicy::Wipe;
        store.set_rules(wipe_rules).expect("wipe default");
        let mut kept = row("https://keep.example");
        kept.policy = SessionPolicy::Keep;
        store.upsert(kept).expect("upsert the keep");
        let due_after = store.due(DAY_MS, false).expect("due");
        assert!(
            !due_after
                .iter()
                .any(|entry| entry.session.origin == "https://keep.example"),
            "a per-origin keep clears nothing (no plan), even when the container default is wipe"
        );

        drop(store);
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
        assert!(store.list().expect("list").is_empty());
        drop(store);
        fs::remove_dir_all(&dir).expect("cleanup");
    }

    #[test]
    fn the_export_is_written_from_the_rows_and_can_be_checked() {
        let dir = scratch_dir();
        let mut store = SessionStore::open(&dir).expect("open");
        store.upsert(row("https://example.org")).expect("upsert");
        let path = store.export_json().expect("export");
        // One row plus the rules count as one checked item each.
        assert_eq!(store.verify_export().expect("verify"), (2, 0));

        // A document that no longer agrees is reported, and a missing one is `not measured`.
        let mut stale: LedgerFile =
            serde_json::from_str(&fs::read_to_string(&path).expect("read")).expect("parse");
        stale.entries.clear();
        fs::write(&path, serde_json::to_string(&stale).expect("json")).expect("write");
        let stale_check = store.verify_export().expect("verify");
        assert_eq!(stale_check.0, 2);
        assert!(stale_check.1 > 0);
        fs::remove_file(&path).expect("remove");
        assert_eq!(store.verify_export().expect("verify").1, -1);
        drop(store);
        fs::remove_dir_all(&dir).expect("cleanup");
    }
}
