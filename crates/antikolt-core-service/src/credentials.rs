// SPDX-License-Identifier: MPL-2.0

//! The password vault's store: `credential` rows whose secret fields are AES-256-GCM ciphertext
//! (AAD = origin + id, ADR-010/082), a wrapped data key (DEK/KEK, ADR-010), and the lock state
//! machine from `antikolt_core::vault`.
//!
//! The database is the one writer (ADR-069/070): nothing else may seal or open a credential. The
//! store seals on write and opens on read, so a plaintext secret exists only inside the unlocked
//! store's memory — never in a `*.json` export and never at rest in the database.

use std::path::Path;

use antikolt_core::secrets::{dpapi, generate_salt, random_dek, AesGcmSecrets};
use antikolt_core::vault::{SecretBox, SecretKind, Vault, VaultStatus};
use serde::{Deserialize, Serialize};

use crate::sqlite_store::{Db, DbError};

/// The AAD that wraps the data key, distinct from a record's AAD so a record can never be confused
/// with the key that opens it.
const DEK_AAD: &[u8] = b"antikolt-vault-dek";

/// The sealed storage form of one credential row. The `*_ct` fields are ciphertext; a list never
/// carries them, only a `get` that has unlocked the vault.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Credential {
    pub id: String,
    pub container_id: Option<String>,
    pub origin: String,
    /// `password` | `totp` | `passkey`, validated against [`SecretKind`] before a write.
    pub kind: String,
    pub label: Option<String>,
    pub username_ct: Option<Vec<u8>>,
    pub password_ct: Option<Vec<u8>>,
    pub totp_secret_ct: Option<Vec<u8>>,
    pub totp_params_json: Option<String>,
    pub passkey_ct: Option<Vec<u8>>,
    pub last_used_at: Option<i64>,
    pub created_at: i64,
    pub updated_at: i64,
}

/// The metadata-only view the list and the create reply carry — never a secret.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CredentialMeta {
    pub id: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub container_id: Option<String>,
    pub origin: String,
    pub kind: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub label: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub last_used_at: Option<i64>,
}

/// The plaintext view a `get` returns — only while the vault is unlocked.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CredentialPlain {
    pub id: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub container_id: Option<String>,
    pub origin: String,
    pub kind: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub label: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub username: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub password: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub totp_secret: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub totp_params: Option<serde_json::Value>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub passkey: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub last_used_at: Option<i64>,
}

/// The plaintext fields a `create` takes. Nothing here is sealed yet: the store does that on write.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct NewCredential {
    pub origin: String,
    pub kind: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub container_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub label: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub username: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub password: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub totp_secret: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub totp_params: Option<serde_json::Value>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub passkey: Option<String>,
}

/// The one row of vault key material: how the data key is wrapped, and the wrapped bytes.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct VaultMeta {
    /// "" = never unlocked, `dpapi` = DPAPI-wrapped, `master` = Argon2id-wrapped.
    pub mode: String,
    /// Base64 salt (master mode only).
    pub salt: String,
    /// The wrapped data key.
    pub dek: Vec<u8>,
}

/// Everything that can go wrong with the vault.
#[derive(Debug, thiserror::Error)]
pub enum CredentialError {
    #[error("cannot read or write the vault: {0}")]
    Io(#[from] std::io::Error),
    #[error("the database refused the vault: {0}")]
    Db(#[from] DbError),
    #[error("the vault is locked")]
    Locked,
    #[error("no cipher is available in this build")]
    NoCipher,
    #[error("the credential was refused: {0}")]
    Rejected(&'static str),
}

impl CredentialError {
    /// The wire error code for the core protocol (`main.rs`).
    pub fn code(&self) -> &'static str {
        match self {
            Self::Locked | Self::NoCipher => "vault_locked",
            Self::Rejected(_) => "invalid_argument",
            Self::Io(_) | Self::Db(_) => "internal",
        }
    }
}

/// The vault: `credential` rows and the wrapped data key, plus the lock state.
#[derive(Debug)]
pub struct CredentialStore {
    db: Db,
    vault: Vault,
    cipher: Option<AesGcmSecrets>,
    id_salt: u64,
    id_counter: u64,
}

impl CredentialStore {
    /// Open the vault (locked). The cipher is built only on `unlock`; until then nothing can be read.
    pub fn open(data_dir: &Path) -> Result<Self, CredentialError> {
        std::fs::create_dir_all(data_dir)?;
        let db = Db::open(data_dir)?;
        let record_count = db.count_credentials()? as usize;
        let mut vault = Vault::with_cipher(0);
        vault.set_record_count(record_count);
        Ok(Self {
            db,
            vault,
            cipher: None,
            id_salt: u64::from(std::process::id()) ^ 0x5A17_0A11_7001_77A1,
            id_counter: 0,
        })
    }

    /// The lock state and the counts the shell may display. Never a secret.
    pub fn status(&self, now_ms: u64) -> VaultStatus {
        self.vault.status(now_ms)
    }

    /// Run the idle auto-lock. Returns true when this call locked the vault.
    pub fn tick(&mut self, now_ms: u64) -> bool {
        if self.vault.tick(now_ms) {
            self.cipher = None;
            true
        } else {
            false
        }
    }

    /// Unlock the vault. `password` selects the master-password path (Argon2id); `None` selects the
    /// platform key store (DPAPI on Windows, refused elsewhere). The first unlock fixes the mode.
    pub fn unlock(
        &mut self,
        password: Option<&str>,
        now_ms: u64,
    ) -> Result<VaultStatus, CredentialError> {
        let meta = self.db.vault_meta()?.unwrap_or_default();
        let dek = self.unwrap_dek(&meta, password)?;
        let mut key = [0u8; 32];
        key.copy_from_slice(&dek);
        self.cipher = Some(AesGcmSecrets::new(key));
        self.vault
            .unlock(now_ms)
            .map_err(|_| CredentialError::NoCipher)?;
        Ok(self.vault.status(now_ms))
    }

    /// Lock the vault: the key material is dropped with the cipher.
    pub fn lock(&mut self) {
        self.cipher = None;
        self.vault.lock();
    }

    /// Recover the data key, or (on the very first unlock) create and wrap one.
    fn unwrap_dek(
        &mut self,
        meta: &VaultMeta,
        password: Option<&str>,
    ) -> Result<Vec<u8>, CredentialError> {
        match meta.mode.as_str() {
            "master" => {
                let password = password.ok_or(CredentialError::Rejected(
                    "a master password is required for this vault",
                ))?;
                let kek = AesGcmSecrets::from_master_password(password, &meta.salt)
                    .map_err(|_| CredentialError::Locked)?;
                kek.open(DEK_AAD, &meta.dek)
                    .map_err(|_| CredentialError::Locked)
            }
            "dpapi" => dpapi_unprotect(&meta.dek),
            _ => self.first_unlock(password),
        }
    }

    /// The first unlock has no key yet: mint one, wrap it by the chosen mode, and store the wrap.
    fn first_unlock(&mut self, password: Option<&str>) -> Result<Vec<u8>, CredentialError> {
        let dek = random_dek();
        let meta = match password {
            Some(password) => {
                let salt = generate_salt();
                let kek = AesGcmSecrets::from_master_password(password, &salt)
                    .map_err(|_| CredentialError::Locked)?;
                let wrapped = kek
                    .seal(DEK_AAD, &dek)
                    .map_err(|_| CredentialError::Locked)?;
                VaultMeta {
                    mode: "master".to_string(),
                    salt,
                    dek: wrapped,
                }
            }
            None => dpapi_wrap(&dek)?,
        };
        self.db.set_vault_meta(&meta)?;
        Ok(dek.to_vec())
    }

    /// The gate every secret operation goes through; returns the live cipher.
    fn require_open(&self) -> Result<&AesGcmSecrets, CredentialError> {
        self.vault
            .require_open()
            .map_err(|_| CredentialError::Locked)?;
        self.cipher.as_ref().ok_or(CredentialError::Locked)
    }

    /// Metadata-only list, ordered by origin — the panel's stable order.
    pub fn list(&self) -> Result<Vec<CredentialMeta>, CredentialError> {
        Ok(self
            .db
            .all_credentials()?
            .into_iter()
            .map(meta_of)
            .collect())
    }

    /// Seal and store one credential. Requires the vault to be open.
    pub fn upsert(
        &mut self,
        input: NewCredential,
        now_ms: u64,
    ) -> Result<CredentialMeta, CredentialError> {
        SecretKind::parse(&input.kind)
            .ok_or(CredentialError::Rejected("unknown credential kind"))?;
        if input.origin.trim().is_empty() {
            return Err(CredentialError::Rejected("a credential needs an origin"));
        }
        let id = self.mint_id(now_ms)?;
        let aad = record_aad(&input.origin, &id);
        self.vault.touch(now_ms);
        let cipher = self.require_open()?;
        let seal = |plain: &Option<String>| -> Result<Option<Vec<u8>>, CredentialError> {
            match plain {
                Some(text) => cipher
                    .seal(&aad, text.as_bytes())
                    .map(Some)
                    .map_err(|_| CredentialError::Locked),
                None => Ok(None),
            }
        };
        let credential = Credential {
            id,
            container_id: input.container_id,
            origin: input.origin,
            kind: input.kind,
            label: input.label,
            username_ct: seal(&input.username)?,
            password_ct: seal(&input.password)?,
            totp_secret_ct: seal(&input.totp_secret)?,
            totp_params_json: input.totp_params.map(|value| value.to_string()),
            passkey_ct: seal(&input.passkey)?,
            last_used_at: None,
            created_at: now_ms as i64,
            updated_at: now_ms as i64,
        };
        self.db.upsert_credential(&credential)?;
        self.vault
            .set_record_count(self.db.count_credentials()? as usize);
        Ok(meta_of(credential))
    }

    /// Open and return one credential's secrets. Requires the vault to be open; `None` is an unknown id.
    pub fn get(
        &mut self,
        id: &str,
        now_ms: u64,
    ) -> Result<Option<CredentialPlain>, CredentialError> {
        self.vault.touch(now_ms);
        let cipher = self.require_open()?;
        let Some(credential) = self.db.credential(id)? else {
            return Ok(None);
        };
        let aad = record_aad(&credential.origin, &credential.id);
        let open = |sealed: &Option<Vec<u8>>| -> Result<Option<String>, CredentialError> {
            match sealed {
                Some(blob) => {
                    let bytes = cipher
                        .open(&aad, blob)
                        .map_err(|_| CredentialError::Locked)?;
                    let text = String::from_utf8(bytes)
                        .map_err(|_| CredentialError::Rejected("the sealed record is not UTF-8"))?;
                    Ok(Some(text))
                }
                None => Ok(None),
            }
        };
        let plain = CredentialPlain {
            id: credential.id,
            container_id: credential.container_id,
            origin: credential.origin,
            kind: credential.kind,
            label: credential.label,
            username: open(&credential.username_ct)?,
            password: open(&credential.password_ct)?,
            totp_secret: open(&credential.totp_secret_ct)?,
            totp_params: credential
                .totp_params_json
                .and_then(|text| serde_json::from_str(&text).ok()),
            passkey: open(&credential.passkey_ct)?,
            last_used_at: credential.last_used_at,
        };
        self.db.touch_credential(id, now_ms as i64)?;
        Ok(Some(plain))
    }

    /// The current TOTP code for a credential, plus the seconds until it rotates. `None` = unknown id.
    pub fn totp_code_for(
        &mut self,
        id: &str,
        now_ms: u64,
    ) -> Result<Option<(String, u64)>, CredentialError> {
        self.vault.touch(now_ms);
        let cipher = self.require_open()?;
        let Some(credential) = self.db.credential(id)? else {
            return Ok(None);
        };
        let Some(secret_blob) = credential.totp_secret_ct.as_ref() else {
            return Err(CredentialError::Rejected(
                "this credential has no TOTP secret",
            ));
        };
        let aad = record_aad(&credential.origin, &credential.id);
        let secret = String::from_utf8(
            cipher
                .open(&aad, secret_blob)
                .map_err(|_| CredentialError::Locked)?,
        )
        .map_err(|_| CredentialError::Rejected("the sealed record is not UTF-8"))?;
        let (digits, period_seconds, algorithm) =
            totp_params(credential.totp_params_json.as_deref());
        let code =
            antikolt_core::totp::totp_code(&secret, digits, period_seconds, algorithm, now_ms)
                .map_err(|_| CredentialError::Rejected("cannot compute a TOTP code"))?;
        let step_ms = period_seconds * 1000;
        let seconds_remaining = (step_ms - (now_ms % step_ms)) / 1000;
        self.db.touch_credential(id, now_ms as i64)?;
        Ok(Some((code, seconds_remaining)))
    }

    /// Drop one credential. Requires the vault to be open (a secret operation, ADR-010).
    pub fn delete(&mut self, id: &str, now_ms: u64) -> Result<bool, CredentialError> {
        self.vault.touch(now_ms);
        self.require_open()?;
        let removed = self.db.delete_credential(id)?;
        self.vault
            .set_record_count(self.db.count_credentials()? as usize);
        Ok(removed)
    }

    fn mint_id(&mut self, now_ms: u64) -> Result<String, CredentialError> {
        self.id_counter += 1;
        antikolt_core::ids::Id::mint(now_ms, self.id_counter, self.id_salt)
            .map(|id| id.as_str().to_string())
            .map_err(|_| CredentialError::Rejected("cannot mint a credential id"))
    }
}

/// The record AAD: origin + id (ADR-010), so a sealed blob can never be replayed onto another origin.
fn record_aad(origin: &str, id: &str) -> Vec<u8> {
    let mut aad = Vec::with_capacity(origin.len() + 1 + id.len());
    aad.extend_from_slice(origin.as_bytes());
    aad.push(b'|');
    aad.extend_from_slice(id.as_bytes());
    aad
}

fn meta_of(credential: Credential) -> CredentialMeta {
    CredentialMeta {
        id: credential.id,
        container_id: credential.container_id,
        origin: credential.origin,
        kind: credential.kind,
        label: credential.label,
        last_used_at: credential.last_used_at,
    }
}

/// The TOTP parameters from `totp_params_json`, with the RFC 6238 defaults (6 digits, 30 s, SHA-1).
fn totp_params(json: Option<&str>) -> (u32, u64, antikolt_core::totp::TotpAlgorithm) {
    let value: serde_json::Value = json
        .and_then(|text| serde_json::from_str(text).ok())
        .unwrap_or(serde_json::Value::Null);
    let digits = value
        .get("digits")
        .and_then(|v| v.as_u64())
        .filter(|d| *d == 6 || *d == 8)
        .unwrap_or(6) as u32;
    let period = value
        .get("period")
        .and_then(|v| v.as_u64())
        .filter(|p| *p > 0)
        .unwrap_or(30);
    let algorithm = value
        .get("algorithm")
        .and_then(|v| v.as_str())
        .and_then(antikolt_core::totp::TotpAlgorithm::parse)
        .unwrap_or(antikolt_core::totp::TotpAlgorithm::Sha1);
    (digits, period, algorithm)
}

/// DPAPI-wrapped key material; on non-Windows there is no platform key store, so the no-password
/// path refuses honestly instead of pretending a key it cannot recover.
#[cfg(windows)]
fn dpapi_unprotect(blob: &[u8]) -> Result<Vec<u8>, CredentialError> {
    dpapi::unprotect(blob).map_err(|_| CredentialError::NoCipher)
}

#[cfg(not(windows))]
fn dpapi_unprotect(_blob: &[u8]) -> Result<Vec<u8>, CredentialError> {
    Err(CredentialError::NoCipher)
}

#[cfg(windows)]
fn dpapi_wrap(dek: &[u8; 32]) -> Result<VaultMeta, CredentialError> {
    let wrapped = dpapi::protect(dek).map_err(|_| CredentialError::NoCipher)?;
    Ok(VaultMeta {
        mode: "dpapi".to_string(),
        salt: String::new(),
        dek: wrapped,
    })
}

#[cfg(not(windows))]
fn dpapi_wrap(_dek: &[u8; 32]) -> Result<VaultMeta, CredentialError> {
    Err(CredentialError::NoCipher)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch_dir() -> std::path::PathBuf {
        let unique = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .expect("clock")
            .as_nanos();
        let dir = std::env::temp_dir().join(format!("antikolt-vault-{unique}"));
        std::fs::create_dir_all(&dir).expect("scratch dir");
        dir
    }

    fn sample() -> NewCredential {
        NewCredential {
            origin: "https://pelda.hu".to_string(),
            kind: "password".to_string(),
            container_id: Some("c-private".to_string()),
            label: Some("Pelda".to_string()),
            username: Some("felhasznalo".to_string()),
            password: Some("hunter2".to_string()),
            totp_secret: None,
            totp_params: None,
            passkey: None,
        }
    }

    #[test]
    fn a_fresh_vault_is_locked_and_refuses_everything() {
        let dir = scratch_dir();
        let mut store = CredentialStore::open(&dir).expect("open");
        assert!(!store.status(1).unlocked);
        assert_eq!(store.list().expect("list").len(), 0);
        assert!(
            store.upsert(sample(), 1).is_err(),
            "a locked vault refuses writes"
        );
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn a_master_password_unlocks_and_round_trips_a_secret() {
        let dir = scratch_dir();
        let mut store = CredentialStore::open(&dir).expect("open");
        store.unlock(Some("correct horse"), 1_000).expect("unlock");

        let meta = store.upsert(sample(), 2_000).expect("create");
        assert_eq!(meta.origin, "https://pelda.hu");
        assert_eq!(store.list().expect("list").len(), 1);

        // The metadata list never carries a secret.
        assert!(!serde_json::to_string(&store.list().expect("list"))
            .expect("json")
            .contains("hunter2"));

        let plain = store.get(&meta.id, 3_000).expect("get").expect("known");
        assert_eq!(plain.username.as_deref(), Some("felhasznalo"));
        assert_eq!(plain.password.as_deref(), Some("hunter2"));

        // Locking drops the key: the same id no longer opens.
        store.lock();
        assert!(store.get(&meta.id, 4_000).is_err());
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn a_wrong_password_does_not_unlock() {
        let dir = scratch_dir();
        let mut store = CredentialStore::open(&dir).expect("open");
        store.unlock(Some("correct horse"), 1_000).expect("unlock");
        store.lock();
        assert!(store.unlock(Some("wrong horse"), 2_000).is_err());
        assert!(!store.status(2_000).unlocked);
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn the_vault_survives_a_reopen_with_the_same_master_password() {
        let dir = scratch_dir();
        {
            let mut store = CredentialStore::open(&dir).expect("open");
            store.unlock(Some("pw"), 1_000).expect("unlock");
            let meta = store.upsert(sample(), 2_000).expect("create");
            store.lock();
            let _ = meta;
        }
        let mut reopened = CredentialStore::open(&dir).expect("reopen");
        assert_eq!(reopened.list().expect("list").len(), 1, "rows survive");
        reopened.unlock(Some("pw"), 3_000).expect("unlock again");
        let plain = reopened
            .get(&reopened.list().expect("list")[0].id, 4_000)
            .expect("get")
            .expect("known");
        assert_eq!(plain.password.as_deref(), Some("hunter2"));
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn a_replayed_blob_is_refused_under_a_different_origin() {
        // The AAD binds the record to origin + id: moving the same ciphertext to another origin must not open.
        let dir = scratch_dir();
        let mut store = CredentialStore::open(&dir).expect("open");
        store.unlock(Some("pw"), 1_000).expect("unlock");
        let meta = store.upsert(sample(), 2_000).expect("create");
        let sealed = store.db.credential(&meta.id).expect("row").expect("known");
        let mut forged = sealed.clone();
        forged.id = "01FORGED0000000000000000".to_string();
        store
            .db
            .upsert_credential(&forged)
            .expect("write the forgery");
        let opened = store.get(&forged.id, 3_000);
        assert!(opened.is_err(), "the forged AAD must refuse: {opened:?}");
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn deleting_requires_an_open_vault_and_removes_the_row() {
        let dir = scratch_dir();
        let mut store = CredentialStore::open(&dir).expect("open");
        store.unlock(Some("pw"), 1_000).expect("unlock");
        let meta = store.upsert(sample(), 2_000).expect("create");
        assert!(store.delete(&meta.id, 2_500).expect("delete"));
        assert_eq!(store.list().expect("list").len(), 0);
        store.lock();
        let again = store.upsert(sample(), 3_000);
        assert!(again.is_err(), "a locked vault refuses deletes too");
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn idle_auto_lock_drops_the_key_after_five_minutes() {
        let dir = scratch_dir();
        let mut store = CredentialStore::open(&dir).expect("open");
        store.unlock(Some("pw"), 1_000).expect("unlock");
        let meta = store.upsert(sample(), 2_000).expect("create");

        // Use keeps it open; five idle minutes lock it and drop the cipher.
        store.get(&meta.id, 2_000 + 120_000).expect("in use");
        assert!(store.tick(2_000 + 120_000 + 300_000), "5 idle minutes lock");
        assert!(!store.status(2_000 + 120_000 + 300_000).unlocked);
        assert!(store.get(&meta.id, 2_000 + 120_000 + 300_001).is_err());
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn totp_code_for_reproduces_the_rfc_vector_through_the_store() {
        let dir = scratch_dir();
        let mut store = CredentialStore::open(&dir).expect("open");
        store.unlock(Some("pw"), 1_000).expect("unlock");

        let mut input = sample();
        input.kind = "totp".to_string();
        input.username = None;
        input.password = None;
        input.totp_secret = Some("GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ".to_string());
        input.totp_params = Some(serde_json::json!({ "digits": 8 }));
        let meta = store.upsert(input, 2_000).expect("create");

        // RFC 6238 appendix B, T = 59 s, 8 digits, SHA-1 → 94287082.
        let (code, remaining) = store
            .totp_code_for(&meta.id, 59_000)
            .expect("code")
            .expect("known");
        assert_eq!(code, "94287082");
        assert!(remaining <= 30, "the code rotates within one period");
        std::fs::remove_dir_all(&dir).ok();
    }
}
