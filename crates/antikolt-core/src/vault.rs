// SPDX-License-Identifier: MPL-2.0

//! The vault's lock state and its crypto seam.
//!
//! **What is here:** the lock/unlock state machine, the idle auto-lock (ADR-010: five minutes), the
//! "is a secret operation allowed now" gate, and the [`SecretBox`] trait that a real cipher has to
//! implement.
//!
//! **What is deliberately not here:** the cipher. AES-256-GCM with per-record associated data, a data
//! key wrapped by the system key store or by Argon2id (ADR-010), needs crates (`aes-gcm`, `argon2`,
//! `zeroize`) that are **not available in this build environment** — there is no crates.io access and
//! the offline registry holds fourteen crates in total, none of them cryptographic. Writing AES by hand
//! is not an option, and this crate denies `unsafe_code`, so the platform key store (DPAPI) cannot be
//! reached either.
//!
//! A vault that cannot encrypt must **not** fall back to plaintext — that is the one rule ADR-010 makes
//! non-negotiable. So the seam is explicit and it fails closed: [`UnavailableSecrets`] refuses every
//! seal and open with [`VaultError::NoCipher`], the status says `can_store: false`, and the product can
//! wire the shell, the session ledger and the passkey flow against this shape today. When the crates
//! are vendored, one implementation of [`SecretBox`] replaces the refusal and nothing else moves.

use std::fmt;

use crate::clock::is_due;

/// How long the vault stays open without activity (ADR-010).
pub const AUTO_LOCK_IDLE_MS: u64 = 5 * 60 * 1000;

/// What a stored secret is for. Mirrors the `credential.kind` column of the schema.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SecretKind {
    /// A login password (or a password to re-enter after `wipe`).
    Password,
    /// A TOTP secret plus its parameters (`totp_params_json`).
    Totp,
    /// A WebAuthn credential: id, private key, user handle, relying party (ADR-027).
    Passkey,
}

impl SecretKind {
    /// The value stored in the `credential.kind` column.
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Password => "password",
            Self::Totp => "totp",
            Self::Passkey => "passkey",
        }
    }

    /// The hostile-input direction: an unknown kind is rejected, never guessed.
    pub fn parse(value: &str) -> Option<Self> {
        match value {
            "password" => Some(Self::Password),
            "totp" => Some(Self::Totp),
            "passkey" => Some(Self::Passkey),
            _ => None,
        }
    }
}

/// Why a secret operation was refused. The first two map straight onto the wire contract.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VaultError {
    /// The vault is locked: the wire answer is `vault_locked`.
    Locked,
    /// No cipher is available in this build, so the vault will not store or reveal anything.
    NoCipher,
    /// The payload the cipher was handed is not a valid sealed record.
    Corrupt,
}

impl VaultError {
    /// The `ErrorCode` wire string (`crates/antikolt-ipc`).
    pub fn code(self) -> &'static str {
        match self {
            Self::Locked => "vault_locked",
            Self::NoCipher => "not_supported",
            Self::Corrupt => "internal",
        }
    }
}

impl fmt::Display for VaultError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Locked => formatter.write_str("the vault is locked"),
            Self::NoCipher => formatter.write_str("no cipher is available in this build"),
            Self::Corrupt => formatter.write_str("the sealed record is not readable"),
        }
    }
}

impl std::error::Error for VaultError {}

/// The crypto seam: seal and open one record's payload.
///
/// The associated data is the record's **origin plus its id** (ADR-010): a sealed blob may therefore
/// never be replayed onto another origin, even by someone who can write the database.
pub trait SecretBox: fmt::Debug {
    /// Encrypt `plaintext`, binding it to `aad`.
    fn seal(&self, aad: &[u8], plaintext: &[u8]) -> Result<Vec<u8>, VaultError>;

    /// Decrypt `ciphertext`, refusing it unless `aad` is the same one used to seal it.
    fn open(&self, aad: &[u8], ciphertext: &[u8]) -> Result<Vec<u8>, VaultError>;

    /// True when this implementation can actually encrypt. The UI asks this before offering to save.
    fn available(&self) -> bool;
}

/// The only [`SecretBox`] in the tree: it refuses everything, and says why.
///
/// This is not a stub that pretends to work — it is the honest answer for a build without a cipher,
/// and it makes the refusal visible in the status instead of letting a plaintext record slip through.
#[derive(Debug, Default, Clone, Copy)]
pub struct UnavailableSecrets;

impl UnavailableSecrets {
    /// A human-readable reason for the refusal, for the status panel and the log.
    pub const REASON: &'static str =
        "no cipher in this build: vendoring aes-gcm, argon2 and zeroize is \
                                      required before a secret can be stored";

    pub const fn new() -> Self {
        Self
    }
}

impl SecretBox for UnavailableSecrets {
    fn seal(&self, _aad: &[u8], _plaintext: &[u8]) -> Result<Vec<u8>, VaultError> {
        Err(VaultError::NoCipher)
    }

    fn open(&self, _aad: &[u8], _ciphertext: &[u8]) -> Result<Vec<u8>, VaultError> {
        Err(VaultError::NoCipher)
    }

    fn available(&self) -> bool {
        false
    }
}

/// What the shell and the passkey layer may see about the vault. Never a secret.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VaultStatus {
    /// The vault is open and a secret operation is currently allowed.
    pub unlocked: bool,
    /// Idle time since the last activity, in milliseconds.
    pub idle_ms: u64,
    /// How many records the vault holds, without their contents.
    pub record_count: usize,
    /// Whether this build can seal at all.
    pub can_store: bool,
    /// Why it cannot, when it cannot.
    pub unavailable_reason: Option<&'static str>,
}

/// The lock state machine.
///
/// Deliberately time-parameterised like the rest of the crate: the caller passes `now_ms`, so the
/// five-minute auto-lock is testable without waiting five minutes.
#[derive(Debug)]
pub struct Vault {
    open_since_ms: u64,
    last_activity_ms: u64,
    unlocked: bool,
    record_count: usize,
    can_store: bool,
}

impl Default for Vault {
    fn default() -> Self {
        Self::new()
    }
}

impl Vault {
    /// A locked vault that cannot store (this build has no cipher).
    pub fn new() -> Self {
        Self {
            open_since_ms: 0,
            last_activity_ms: 0,
            unlocked: false,
            record_count: 0,
            can_store: false,
        }
    }

    /// A vault whose cipher exists: the shape the real implementation will hand over.
    pub fn with_cipher(now_ms: u64) -> Self {
        Self {
            open_since_ms: now_ms,
            last_activity_ms: now_ms,
            unlocked: false,
            record_count: 0,
            can_store: true,
        }
    }

    /// Open the vault. Refuses when there is no cipher: an "unlocked" vault that can never read a
    /// record would be a lie the UI would have to explain away.
    pub fn unlock(&mut self, now_ms: u64) -> Result<(), VaultError> {
        if !self.can_store {
            return Err(VaultError::NoCipher);
        }
        self.unlocked = true;
        self.open_since_ms = now_ms;
        self.last_activity_ms = now_ms;
        Ok(())
    }

    /// Close the vault: the key material is dropped by the cipher owner, not here.
    pub fn lock(&mut self) {
        self.unlocked = false;
        self.open_since_ms = 0;
        self.last_activity_ms = 0;
    }

    /// Record activity, so the idle timer restarts. Called on every use, not on every render.
    pub fn touch(&mut self, now_ms: u64) {
        if self.unlocked {
            self.last_activity_ms = now_ms;
        }
    }

    /// Run the idle check. Returns true when this call locked the vault, so the caller can notify the
    /// UI exactly once.
    pub fn tick(&mut self, now_ms: u64) -> bool {
        if !self.unlocked {
            return false;
        }
        if is_due(now_ms, self.last_activity_ms, AUTO_LOCK_IDLE_MS) {
            self.lock();
            return true;
        }
        false
    }

    /// The gate every secret operation goes through.
    pub fn require_open(&self) -> Result<(), VaultError> {
        if self.unlocked {
            Ok(())
        } else {
            Err(VaultError::Locked)
        }
    }

    /// Record that the store grew or shrank. Counts only: the vault never reports contents.
    pub fn set_record_count(&mut self, count: usize) {
        self.record_count = count;
    }

    /// The state the shell may display.
    pub fn status(&self, now_ms: u64) -> VaultStatus {
        VaultStatus {
            unlocked: self.unlocked,
            idle_ms: if self.unlocked {
                now_ms.saturating_sub(self.last_activity_ms)
            } else {
                0
            },
            record_count: self.record_count,
            can_store: self.can_store,
            unavailable_reason: if self.can_store {
                None
            } else {
                Some(UnavailableSecrets::REASON)
            },
        }
    }

    /// How long the vault has been continuously open, for the diagnostic log.
    pub fn open_for_ms(&self, now_ms: u64) -> Option<u64> {
        self.unlocked
            .then(|| now_ms.saturating_sub(self.open_since_ms))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const T0: u64 = 1_800_000_000_000;

    #[test]
    fn a_fresh_vault_is_locked_and_refuses_everything() {
        let mut vault = Vault::with_cipher(T0);
        assert_eq!(vault.require_open(), Err(VaultError::Locked));
        assert_eq!(vault.unlock(T0 + 100), Ok(()));
        assert_eq!(vault.require_open(), Ok(()));
        vault.lock();
        assert_eq!(vault.require_open(), Err(VaultError::Locked));
    }

    #[test]
    fn without_a_cipher_the_vault_refuses_to_open() {
        let mut vault = Vault::new();
        assert_eq!(vault.unlock(T0), Err(VaultError::NoCipher));
        let status = vault.status(T0);
        assert!(!status.unlocked);
        assert!(!status.can_store);
        assert!(status.unavailable_reason.is_some());
    }

    #[test]
    fn the_unavailable_seam_never_returns_plaintext() {
        let secrets = UnavailableSecrets::new();
        assert!(!secrets.available());
        assert_eq!(
            secrets.seal(b"origin", b"hunter2"),
            Err(VaultError::NoCipher)
        );
        assert_eq!(
            secrets.open(b"origin", b"sealed"),
            Err(VaultError::NoCipher)
        );
    }

    #[test]
    fn idle_lock_is_inclusive_at_five_minutes() {
        let mut vault = Vault::with_cipher(T0);
        vault.unlock(T0).expect("unlock");
        assert!(
            !vault.tick(T0 + AUTO_LOCK_IDLE_MS - 1),
            "4:59.999 must stay open"
        );
        assert!(vault.tick(T0 + AUTO_LOCK_IDLE_MS), "5:00 must lock");
        assert_eq!(vault.require_open(), Err(VaultError::Locked));
        // A second tick is a no-op: the UI is notified exactly once.
        assert!(!vault.tick(T0 + AUTO_LOCK_IDLE_MS + 1));
    }

    #[test]
    fn activity_restarts_the_idle_timer() {
        let mut vault = Vault::with_cipher(T0);
        vault.unlock(T0).expect("unlock");
        vault.touch(T0 + 4 * 60 * 1000);
        assert!(
            !vault.tick(T0 + 8 * 60 * 1000),
            "idle clock restarted at 4:00"
        );
        assert!(
            vault.tick(T0 + 9 * 60 * 1000),
            "5:00 after the last activity"
        );
    }

    #[test]
    fn a_clock_that_steps_backwards_cannot_extend_an_open_vault() {
        let mut vault = Vault::with_cipher(T0);
        vault.unlock(T0 + 10_000).expect("unlock");
        // `is_due` saturates, so an earlier `now` is "no time has passed", never a negative timer.
        assert!(!vault.tick(T0));
        assert_eq!(vault.require_open(), Ok(()));
    }

    #[test]
    fn status_never_carries_a_secret() {
        let mut vault = Vault::with_cipher(T0);
        vault.unlock(T0).expect("unlock");
        vault.set_record_count(7);
        let status = vault.status(T0 + 1_000);
        assert!(status.unlocked);
        assert_eq!(status.idle_ms, 1_000);
        assert_eq!(status.record_count, 7);
        assert!(status.can_store);
        assert_eq!(status.unavailable_reason, None);
        assert_eq!(vault.open_for_ms(T0 + 1_000), Some(1_000));
    }

    #[test]
    fn locking_clears_the_idle_and_open_clocks() {
        let mut vault = Vault::with_cipher(T0);
        vault.unlock(T0).expect("unlock");
        vault.lock();
        assert_eq!(vault.open_for_ms(T0 + 60_000), None);
        assert_eq!(vault.status(T0 + 60_000).idle_ms, 0);
        assert!(!vault.tick(T0 + 60_000));
    }

    #[test]
    fn secret_kinds_round_trip_and_reject_the_unknown() {
        for kind in [SecretKind::Password, SecretKind::Totp, SecretKind::Passkey] {
            assert_eq!(SecretKind::parse(kind.as_str()), Some(kind));
        }
        assert_eq!(SecretKind::parse("note"), None);
        assert_eq!(SecretKind::parse(""), None);
    }

    #[test]
    fn error_codes_match_the_wire_contract() {
        assert_eq!(VaultError::Locked.code(), "vault_locked");
        assert_eq!(VaultError::NoCipher.code(), "not_supported");
        assert_eq!(VaultError::Corrupt.code(), "internal");
        assert_eq!(VaultError::Locked.to_string(), "the vault is locked");
    }
}
