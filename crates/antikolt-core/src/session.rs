// SPDX-License-Identifier: MPL-2.0

//! Session policy and the session ledger (`docs/HU/10-MUNKAMENET.md`, ADR-009, ADR-019).
//!
//! The point of this module is a claim that is easy to state and easy to get wrong:
//! **keeping a login costs no RAM.** Cookies and stored tokens live in the engine's on-disk
//! partition; while a card is hibernated they cost nothing. What costs memory is a *live page*,
//! a warm engine, a background connection or a running service worker.
//!
//! So the 24-hour rule does not delete logins. It deletes *by-products*: `keep` deletes nothing,
//! `sweep` clears caches and ephemeral state (you stay signed in), `wipe` clears the origin (you
//! are signed out — the choice for banks).

use serde::{Deserialize, Serialize};

use crate::clock::is_due;

/// What happens to a session once its policy's time-to-live has passed.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SessionPolicy {
    /// Delete nothing; the login survives indefinitely.
    Keep,
    /// Delete caches and ephemeral state; the login survives. This is the container default.
    #[default]
    Sweep,
    /// Delete everything for the origin: you are signed out.
    Wipe,
}

impl SessionPolicy {
    /// Parse the value stored in the `session_policy` column.
    pub fn from_db(value: &str) -> Option<Self> {
        match value {
            "keep" => Some(Self::Keep),
            "sweep" => Some(Self::Sweep),
            "wipe" => Some(Self::Wipe),
            _ => None,
        }
    }

    pub fn as_db(self) -> &'static str {
        match self {
            Self::Keep => "keep",
            Self::Sweep => "sweep",
            Self::Wipe => "wipe",
        }
    }
}

/// Container-wide defaults; per-card overrides win.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct SessionRules {
    pub container_default: SessionPolicy,
    /// Time-to-live in milliseconds, counted from the card's last open.
    pub ttl_ms: u64,
    /// Sensitive containers are wiped when the operating system session locks.
    pub wipe_on_app_lock: bool,
}

impl Default for SessionRules {
    fn default() -> Self {
        Self {
            container_default: SessionPolicy::Sweep,
            ttl_ms: 24 * 60 * 60_000,
            wipe_on_app_lock: false,
        }
    }
}

impl SessionRules {
    /// The effective policy for a card: its own override, else the container default.
    pub fn resolve(&self, card_override: Option<SessionPolicy>) -> SessionPolicy {
        card_override.unwrap_or(self.container_default)
    }
}

/// Exactly which storages a cleanup touches.
///
/// The flags map one-to-one onto the storage types accepted by Chromium's
/// `Storage.clearDataForOrigin`, so the engine host never has to guess.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct CleanupPlan {
    pub policy: SessionPolicy,
    pub session_storage: bool,
    pub cache_storage: bool,
    pub service_workers: bool,
    pub code_cache: bool,
    pub local_storage: bool,
    pub indexed_db: bool,
    pub cookies: bool,
}

impl CleanupPlan {
    /// The plan for one policy. `Keep` produces a plan that clears nothing.
    pub fn for_policy(policy: SessionPolicy) -> Self {
        match policy {
            SessionPolicy::Keep => Self {
                policy,
                session_storage: false,
                cache_storage: false,
                service_workers: false,
                code_cache: false,
                local_storage: false,
                indexed_db: false,
                cookies: false,
            },
            SessionPolicy::Sweep => Self {
                policy,
                session_storage: true,
                cache_storage: true,
                service_workers: true,
                code_cache: true,
                local_storage: false,
                indexed_db: false,
                cookies: false,
            },
            SessionPolicy::Wipe => Self {
                policy,
                session_storage: true,
                cache_storage: true,
                service_workers: true,
                code_cache: true,
                local_storage: true,
                indexed_db: true,
                cookies: true,
            },
        }
    }

    /// A plan that would not delete anything is not a plan: the caller should skip the round trip.
    pub fn is_noop(&self) -> bool {
        !(self.session_storage
            || self.cache_storage
            || self.service_workers
            || self.code_cache
            || self.local_storage
            || self.indexed_db
            || self.cookies)
    }

    /// `true` when this plan signs the user out.
    pub fn signs_out(&self) -> bool {
        self.cookies || self.local_storage
    }

    /// The storage identifiers to send with the clear request.
    pub fn storage_types(&self) -> Vec<&'static str> {
        let mut out = Vec::new();
        if self.session_storage {
            out.push("session_storage");
        }
        if self.cache_storage {
            out.push("cache_storage");
        }
        if self.service_workers {
            out.push("service_workers");
        }
        if self.code_cache {
            out.push("code_cache");
        }
        if self.local_storage {
            out.push("local_storage");
        }
        if self.indexed_db {
            out.push("indexed_db");
        }
        if self.cookies {
            out.push("cookies");
        }
        out
    }
}

/// Decide whether anything should be cleared right now.
///
/// Returns `None` when nothing is due. The caller must hibernate the card before executing a plan
/// that touches a live origin (`docs/HU/02` §4.3).
pub fn plan_cleanup(
    rules: &SessionRules,
    card_override: Option<SessionPolicy>,
    last_seen_ms: u64,
    now_ms: u64,
    app_locked: bool,
) -> Option<CleanupPlan> {
    if app_locked && rules.wipe_on_app_lock {
        return Some(CleanupPlan::for_policy(SessionPolicy::Wipe));
    }

    let policy = rules.resolve(card_override);
    if policy == SessionPolicy::Keep {
        return None;
    }
    if !is_due(now_ms, last_seen_ms, rules.ttl_ms) {
        return None;
    }
    let plan = CleanupPlan::for_policy(policy);
    if plan.is_noop() {
        None
    } else {
        Some(plan)
    }
}

/// What the engine's page told us about the login state. It is a *hint*, never proof.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum LoginState {
    LoggedIn,
    LoggedOut,
    /// The origin used to have a session but the last load looked signed out.
    Expired,
    /// Not enough signal; the UI must not pretend to know.
    Unknown,
}

impl LoginState {
    /// The value stored in the `sessions.state` column (ADR-070). The same strings the JSON ledger wrote, so a
    /// profile migrated from `sessions.json` reads back exactly as it was.
    pub fn as_db(self) -> &'static str {
        match self {
            Self::LoggedIn => "logged_in",
            Self::LoggedOut => "logged_out",
            Self::Expired => "expired",
            Self::Unknown => "unknown",
        }
    }

    pub fn from_db(value: &str) -> Option<Self> {
        match value {
            "logged_in" => Some(Self::LoggedIn),
            "logged_out" => Some(Self::LoggedOut),
            "expired" => Some(Self::Expired),
            "unknown" => Some(Self::Unknown),
            _ => None,
        }
    }
}

/// Signals the injected watcher can observe without reading any secret.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
pub struct LoginSignals {
    pub has_password_field: bool,
    pub has_logout_control: bool,
    pub http_status: Option<u16>,
}

impl LoginSignals {
    pub fn classify(&self) -> LoginState {
        match self {
            // A password field plus no way to sign out is the classic "you are not signed in" page.
            LoginSignals {
                has_password_field: true,
                has_logout_control: false,
                ..
            } => LoginState::LoggedOut,
            // A logout control is the strongest cheap signal that a session exists.
            LoginSignals {
                has_logout_control: true,
                ..
            } => LoginState::LoggedIn,
            LoginSignals {
                http_status: Some(401 | 403),
                ..
            } => LoginState::Expired,
            _ => LoginState::Unknown,
        }
    }
}

/// What the user loses if this session is wiped — shown in the Munkamenetek panel.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct LossEstimate {
    pub needs_password: bool,
    pub needs_totp: bool,
    /// Daily re-logins are what trigger challenge screens on some providers.
    pub may_trigger_captcha: bool,
}

impl LossEstimate {
    /// Estimate the loss for one origin.
    ///
    /// `has_saved_credential` means the vault can re-enter the password, `has_totp` that it can
    /// also produce the second factor. Without both, a wipe means the user must find credentials.
    pub fn assess(has_saved_credential: bool, has_totp: bool) -> Self {
        Self {
            needs_password: !has_saved_credential,
            needs_totp: !has_totp,
            // Only a wipe that leaves the user to log in again can trip an anti-automation check.
            may_trigger_captcha: has_saved_credential && has_totp,
        }
    }
}

/// One row of the session ledger (`docs/HU/10-MUNKAMENET.md` §4).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct SessionLedgerEntry {
    pub container_id: String,
    pub origin: String,
    pub state: LoginState,
    pub policy: SessionPolicy,
    pub cookie_count: u32,
    pub cookie_bytes: u64,
    pub storage_bytes: u64,
    pub service_worker_count: u32,
    pub last_seen_ms: Option<u64>,
    pub last_verified_ms: Option<u64>,
    /// Measured RAM while the card is live; `None` while dormant, because it costs nothing.
    pub live_ram_bytes: Option<u64>,
    pub keep_alive: bool,
}

/// The measured size of one row — what a cleanup would actually delete.
///
/// This is *not* `LossEstimate`: that one describes the friction of losing a login (password, TOTP, captcha),
/// while this one is the amount. Both are shown, because "3 cookies" and "you will need your password" are
/// different kinds of bad. Nothing here is guessed: the numbers come from the ledger's own fields.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct SessionSize {
    pub cookies: u32,
    pub bytes: u64,
    pub service_workers: u32,
}

impl SessionLedgerEntry {
    /// The RAM this session costs *right now*, which is the number that matters.
    pub fn current_ram_bytes(&self) -> u64 {
        self.live_ram_bytes.unwrap_or(0)
    }

    /// The measured size of this row: what a cleanup would remove.
    pub fn size(&self) -> SessionSize {
        SessionSize {
            cookies: self.cookie_count,
            bytes: self.disk_bytes(),
            service_workers: self.service_worker_count,
        }
    }

    /// `true` when the row holds a login, so a wipe means signing in again on that origin.
    pub fn loses_login(&self) -> bool {
        self.state == LoginState::LoggedIn
    }

    pub fn disk_bytes(&self) -> u64 {
        self.cookie_bytes + self.storage_bytes
    }

    /// Reject rows that would make the ledger lie (e.g. a negative measured RAM, an empty origin).
    pub fn validate(&self) -> Result<(), LedgerError> {
        if self.origin.trim().is_empty() {
            return Err(LedgerError::EmptyOrigin);
        }
        if !self.origin.starts_with("https://") && !self.origin.starts_with("http://") {
            return Err(LedgerError::NonHttpOrigin(self.origin.clone()));
        }
        if self.container_id.trim().is_empty() {
            return Err(LedgerError::EmptyContainer);
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum LedgerError {
    #[error("a session ledger entry needs an origin")]
    EmptyOrigin,
    #[error("unknown login state")]
    UnknownState,
    #[error("origin {0:?} is not http(s)")]
    NonHttpOrigin(String),
    #[error("a session ledger entry needs a container")]
    EmptyContainer,
}

#[cfg(test)]
mod tests {
    #[test]
    fn session_size_counts_what_a_wipe_removes() {
        let entry = SessionLedgerEntry {
            container_id: "c-private".into(),
            origin: "https://example.org".into(),
            state: LoginState::LoggedIn,
            policy: SessionPolicy::Wipe,
            cookie_count: 7,
            cookie_bytes: 1024,
            storage_bytes: 4096,
            service_worker_count: 2,
            last_seen_ms: Some(1_700_000_000_000),
            last_verified_ms: None,
            live_ram_bytes: None,
            keep_alive: false,
        };
        let size = entry.size();
        assert_eq!(size.cookies, 7);
        assert_eq!(size.bytes, 5120);
        assert_eq!(size.service_workers, 2);
        assert!(entry.loses_login(), "a logged-in row loses its login");

        let mut signed_out = entry.clone();
        signed_out.state = LoginState::LoggedOut;
        signed_out.live_ram_bytes = Some(64 * 1024 * 1024);
        assert!(!signed_out.loses_login());
        // Dormant costs nothing; a live card costs what was measured.
        assert_eq!(signed_out.current_ram_bytes(), 64 * 1024 * 1024);
        assert_eq!(entry.current_ram_bytes(), 0);
    }

    use super::*;

    const HOUR: u64 = 60 * 60_000;
    const DAY: u64 = 24 * HOUR;

    fn rules(policy: SessionPolicy) -> SessionRules {
        SessionRules {
            container_default: policy,
            ttl_ms: DAY,
            wipe_on_app_lock: false,
        }
    }

    #[test]
    fn card_override_beats_the_container_default() {
        let rules = rules(SessionPolicy::Sweep);
        assert_eq!(rules.resolve(None), SessionPolicy::Sweep);
        assert_eq!(
            rules.resolve(Some(SessionPolicy::Keep)),
            SessionPolicy::Keep
        );
        assert_eq!(
            rules.resolve(Some(SessionPolicy::Wipe)),
            SessionPolicy::Wipe
        );
    }

    #[test]
    fn keep_never_plans_anything() {
        let plan = plan_cleanup(&rules(SessionPolicy::Keep), None, 0, 100 * DAY, false);
        assert!(plan.is_none());
        // Even with an explicit Wipe override the card's own policy wins; here Keep was not chosen.
        assert!(plan_cleanup(
            &rules(SessionPolicy::Keep),
            Some(SessionPolicy::Keep),
            0,
            100 * DAY,
            false
        )
        .is_none());
    }

    #[test]
    fn sweep_clears_byproducts_but_keeps_the_login() {
        let plan = plan_cleanup(&rules(SessionPolicy::Sweep), None, 0, DAY, false).unwrap();
        assert!(plan.session_storage);
        assert!(plan.cache_storage);
        assert!(plan.service_workers);
        assert!(plan.code_cache);
        assert!(!plan.cookies, "sweeping must not sign the user out");
        assert!(!plan.local_storage);
        assert!(!plan.indexed_db);
        assert!(!plan.signs_out());
    }

    #[test]
    fn wipe_clears_everything() {
        let plan = plan_cleanup(&rules(SessionPolicy::Wipe), None, 0, DAY, false).unwrap();
        assert!(plan.cookies);
        assert!(plan.local_storage);
        assert!(plan.indexed_db);
        assert!(plan.service_workers);
        assert!(plan.signs_out());
        let types = plan.storage_types();
        for expected in ["cookies", "local_storage", "indexed_db", "service_workers"] {
            assert!(types.contains(&expected), "missing {expected} in {types:?}");
        }
    }

    #[test]
    fn nothing_is_due_before_the_ttl() {
        assert!(plan_cleanup(&rules(SessionPolicy::Sweep), None, 0, DAY - 1, false).is_none());
        assert!(plan_cleanup(&rules(SessionPolicy::Sweep), None, 0, DAY, false).is_some());
    }

    #[test]
    fn an_override_of_keep_protects_a_sweep_container() {
        let plan = plan_cleanup(
            &rules(SessionPolicy::Sweep),
            Some(SessionPolicy::Keep),
            0,
            100 * DAY,
            false,
        );
        assert!(plan.is_none());
    }

    #[test]
    fn lock_wipes_sensitive_containers_regardless_of_policy() {
        let mut rules = rules(SessionPolicy::Keep);
        rules.wipe_on_app_lock = true;
        let plan = plan_cleanup(&rules, Some(SessionPolicy::Keep), 0, 0, true).unwrap();
        assert_eq!(plan.policy, SessionPolicy::Wipe);
        assert!(plan.cookies);
    }

    #[test]
    fn lock_without_the_flag_does_nothing() {
        let rules = rules(SessionPolicy::Sweep);
        assert!(plan_cleanup(&rules, None, 0, 0, true).is_none());
    }

    #[test]
    fn policy_text_maps_to_and_from_the_database() {
        for policy in [
            SessionPolicy::Keep,
            SessionPolicy::Sweep,
            SessionPolicy::Wipe,
        ] {
            assert_eq!(SessionPolicy::from_db(policy.as_db()), Some(policy));
        }
        assert_eq!(SessionPolicy::from_db("nope"), None);
    }

    #[test]
    fn login_classification_uses_only_cheap_signals() {
        assert_eq!(
            LoginSignals {
                has_logout_control: true,
                ..Default::default()
            }
            .classify(),
            LoginState::LoggedIn
        );
        assert_eq!(
            LoginSignals {
                has_password_field: true,
                ..Default::default()
            }
            .classify(),
            LoginState::LoggedOut
        );
        assert_eq!(
            LoginSignals {
                http_status: Some(401),
                ..Default::default()
            }
            .classify(),
            LoginState::Expired
        );
        assert_eq!(LoginSignals::default().classify(), LoginState::Unknown);
    }

    #[test]
    fn loss_estimate_is_honest_about_having_no_vault_entry() {
        let with_vault = LossEstimate::assess(true, true);
        assert!(!with_vault.needs_password);
        assert!(!with_vault.needs_totp);
        assert!(with_vault.may_trigger_captcha);

        let without_vault = LossEstimate::assess(false, false);
        assert!(without_vault.needs_password);
        assert!(without_vault.needs_totp);
        assert!(!without_vault.may_trigger_captcha);
    }

    #[test]
    fn a_dormant_session_costs_no_ram() {
        let entry = SessionLedgerEntry {
            container_id: "c1".into(),
            origin: "https://example.com".into(),
            state: LoginState::LoggedIn,
            policy: SessionPolicy::Keep,
            cookie_count: 14,
            cookie_bytes: 9_000,
            storage_bytes: 1_500_000,
            service_worker_count: 0,
            last_seen_ms: Some(1),
            last_verified_ms: Some(1),
            live_ram_bytes: None,
            keep_alive: false,
        };
        assert_eq!(entry.current_ram_bytes(), 0);
        assert_eq!(entry.disk_bytes(), 1_509_000);
        assert!(entry.validate().is_ok());
    }

    #[test]
    fn ledger_rejects_nonsense_rows() {
        let mut entry = SessionLedgerEntry {
            container_id: "c1".into(),
            origin: "https://example.com".into(),
            state: LoginState::Unknown,
            policy: SessionPolicy::Sweep,
            cookie_count: 0,
            cookie_bytes: 0,
            storage_bytes: 0,
            service_worker_count: 0,
            last_seen_ms: None,
            last_verified_ms: None,
            live_ram_bytes: None,
            keep_alive: false,
        };
        assert!(entry.validate().is_ok());

        entry.origin = "file:///etc/passwd".into();
        assert!(matches!(
            entry.validate(),
            Err(LedgerError::NonHttpOrigin(_))
        ));

        entry.origin = "  ".into();
        assert_eq!(entry.validate(), Err(LedgerError::EmptyOrigin));

        entry.origin = "https://example.com".into();
        entry.container_id = "".into();
        assert_eq!(entry.validate(), Err(LedgerError::EmptyContainer));
    }
}
