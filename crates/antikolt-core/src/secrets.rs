// SPDX-License-Identifier: MPL-2.0

//! The vault's real ciphers.
//!
//! [`vault::SecretBox`] is the seam; this module is the first thing that implements it for real.
//! Two key paths exist, matching the ADR-031 decision "DPAPI + optional master password":
//!
//! * **Master password** — [`AesGcmSecrets::from_master_password`] derives the 256-bit key with
//!   Argon2id from the password and a stored salt. This path is portable (a profile exported with a
//!   master password opens on another machine) but asks the user for a password.
//! * **Windows DPAPI** — [`dpapi::protect`] / [`dpapi::unprotect`] seal a random data key under the
//!   current user's logon credentials. No password to forget, but the blob opens only for this user
//!   on this machine.
//!
//! Records themselves are always AES-256-GCM with the record's **origin + id** as associated data
//! (ADR-010), so a sealed blob can never be replayed onto another origin. A key is stored in
//! [`zeroize::Zeroizing`] and erased on drop; the [`Debug`] impl deliberately prints no secret.

use crate::vault::{SecretBox, VaultError};
use aes_gcm::{
    aead::{Aead, AeadCore, KeyInit, OsRng, Payload},
    Aes256Gcm, Key, Nonce,
};

/// The length of the random per-record nonce, in bytes. AES-GCM uses a 96-bit nonce; it is prepended
/// to the ciphertext so [`AesGcmSecrets::open`] can read it back.
const NONCE_LEN: usize = 12;

/// Derive a 256-bit key from a master password with Argon2id (v19, the recommended parameters).
///
/// `salt_b64` is the base64 salt string produced by [`generate_salt`]. A wrong salt or an unreadable
/// salt string is a corrupt record, never a guess.
fn derive_key(password: &str, salt_b64: &str) -> Result<zeroize::Zeroizing<[u8; 32]>, VaultError> {
    use argon2::password_hash::{PasswordHasher, SaltString};

    let salt = SaltString::from_b64(salt_b64).map_err(|_| VaultError::Corrupt)?;
    let hash = argon2::Argon2::default()
        .hash_password(password.as_bytes(), &salt)
        .map_err(|_| VaultError::Corrupt)?;
    let output = hash.hash.ok_or(VaultError::Corrupt)?;
    let mut key = zeroize::Zeroizing::new([0u8; 32]);
    key.copy_from_slice(output.as_bytes());
    Ok(key)
}

/// A fresh random salt, in the base64 form [`derive_key`] and the store can round-trip.
pub fn generate_salt() -> String {
    use argon2::password_hash::{rand_core::OsRng as ArgonOsRng, SaltString};

    let salt = SaltString::generate(&mut ArgonOsRng);
    salt.as_str().to_string()
}

/// A fresh random 256-bit data key (the DEK, ADR-010). Drawn from the OS CSPRNG, never from the
/// non-cryptographic identifier generator (`ids.rs` is explicit that it is not for secrets).
pub fn random_dek() -> [u8; 32] {
    use aes_gcm::aead::rand_core::RngCore;
    let mut key = [0u8; 32];
    OsRng.fill_bytes(&mut key);
    key
}

/// AES-256-GCM over a derived or injected 256-bit key.
pub struct AesGcmSecrets {
    key: zeroize::Zeroizing<[u8; 32]>,
}

impl AesGcmSecrets {
    /// Build the cipher from a raw 256-bit key (for example a data key recovered from DPAPI).
    pub fn new(key: [u8; 32]) -> Self {
        Self {
            key: zeroize::Zeroizing::new(key),
        }
    }

    /// Build the cipher by deriving the key from a master password and a stored salt.
    pub fn from_master_password(password: &str, salt_b64: &str) -> Result<Self, VaultError> {
        Ok(Self {
            key: derive_key(password, salt_b64)?,
        })
    }
}

/// The [`Debug`] impl never reveals the key: `Zeroizing` erases it on drop, so neither should the
/// formatter. The crate denies `missing_debug_implementations`, so this manual impl is required.
impl std::fmt::Debug for AesGcmSecrets {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str("AesGcmSecrets { .. }")
    }
}

impl SecretBox for AesGcmSecrets {
    fn seal(&self, aad: &[u8], plaintext: &[u8]) -> Result<Vec<u8>, VaultError> {
        let cipher = Aes256Gcm::new(Key::<Aes256Gcm>::from_slice(&*self.key));
        let nonce = Aes256Gcm::generate_nonce(&mut OsRng);
        let ciphertext = cipher
            .encrypt(
                &nonce,
                Payload {
                    msg: plaintext,
                    aad,
                },
            )
            .map_err(|_| VaultError::Corrupt)?;
        let mut sealed = Vec::with_capacity(NONCE_LEN + ciphertext.len());
        sealed.extend_from_slice(nonce.as_slice());
        sealed.extend_from_slice(&ciphertext);
        Ok(sealed)
    }

    fn open(&self, aad: &[u8], ciphertext: &[u8]) -> Result<Vec<u8>, VaultError> {
        if ciphertext.len() < NONCE_LEN {
            return Err(VaultError::Corrupt);
        }
        let (nonce_bytes, body) = ciphertext.split_at(NONCE_LEN);
        let cipher = Aes256Gcm::new(Key::<Aes256Gcm>::from_slice(&*self.key));
        let nonce = Nonce::from_slice(nonce_bytes);
        // A wrong key, a tampered body or a mismatched AAD all fail GCM tag verification here —
        // they are one error, `Corrupt`, never a plaintext guess.
        cipher
            .decrypt(nonce, Payload { msg: body, aad })
            .map_err(|_| VaultError::Corrupt)
    }

    fn available(&self) -> bool {
        true
    }
}

/// Windows Data Protection API: seal a byte blob under the current user's logon credentials and open
/// it again on the same machine + user.
///
/// This is the platform key store the "DPAPI + optional master password" decision refers to: the
/// vault's data key is protected with `CryptProtectData` by default, so there is no master password
/// to forget — and, symmetrically, the profile is **not** portable, because the blob opens only for
/// this user on this machine. The module is the only place in the crate that uses `unsafe`, confined
/// to the two documented Win32 calls and the `LocalFree` that releases the buffer Windows allocated.
#[allow(unsafe_code)]
#[cfg(windows)]
pub mod dpapi {
    use std::ptr;

    /// `CRYPTPROTECT_UI_FORBIDDEN`: never show a prompt, even in an interactive session.
    const CRYPTPROTECT_UI_FORBIDDEN: u32 = 0x1;

    #[repr(C)]
    struct DataBlob {
        cb_data: u32,
        pb_data: *mut u8,
    }

    #[link(name = "crypt32")]
    extern "system" {
        fn CryptProtectData(
            p_data_in: *const DataBlob,
            sz_data_descr: *const u16,
            p_optional_entropy: *const DataBlob,
            pv_reserved: *mut core::ffi::c_void,
            p_prompt_struct: *mut core::ffi::c_void,
            dw_flags: u32,
            p_data_out: *mut DataBlob,
        ) -> i32;

        fn CryptUnprotectData(
            p_data_in: *const DataBlob,
            ppsz_data_descr: *mut *mut u16,
            p_optional_entropy: *const DataBlob,
            pv_reserved: *mut core::ffi::c_void,
            p_prompt_struct: *mut core::ffi::c_void,
            dw_flags: u32,
            p_data_out: *mut DataBlob,
        ) -> i32;
    }

    #[link(name = "kernel32")]
    extern "system" {
        fn LocalFree(h_mem: *mut core::ffi::c_void) -> *mut core::ffi::c_void;
    }

    fn last_error() -> String {
        std::io::Error::last_os_error()
            .raw_os_error()
            .map(|code| code.to_string())
            .unwrap_or_else(|| "unknown".to_string())
    }

    /// Protect `plaintext` under the current user's credentials.
    pub fn protect(plaintext: &[u8]) -> Result<Vec<u8>, String> {
        // SAFETY: `input` points at `plaintext`, which is borrowed for the duration of the call and
        // therefore alive while `CryptProtectData` reads it. The remaining pointers are null, the
        // flags forbid any UI, and `output` is a zeroed out-parameter Windows fills.
        let input = DataBlob {
            cb_data: plaintext.len() as u32,
            pb_data: plaintext.as_ptr() as *mut u8,
        };
        let mut output = DataBlob {
            cb_data: 0,
            pb_data: ptr::null_mut(),
        };
        let ok = unsafe {
            CryptProtectData(
                &input,
                ptr::null(),
                ptr::null(),
                ptr::null_mut(),
                ptr::null_mut(),
                CRYPTPROTECT_UI_FORBIDDEN,
                &mut output,
            )
        };
        if ok == 0 {
            return Err(format!("CryptProtectData failed (error {})", last_error()));
        }
        Ok(take_blob(output))
    }

    /// Open a blob previously produced by [`protect`]. Fails (never panics) on foreign or corrupt input.
    pub fn unprotect(ciphertext: &[u8]) -> Result<Vec<u8>, String> {
        // SAFETY: see `protect`. `description` is an out-parameter we free with `LocalFree` when
        // non-null, so it is handled after the call rather than leaked.
        let input = DataBlob {
            cb_data: ciphertext.len() as u32,
            pb_data: ciphertext.as_ptr() as *mut u8,
        };
        let mut output = DataBlob {
            cb_data: 0,
            pb_data: ptr::null_mut(),
        };
        let mut description: *mut u16 = ptr::null_mut();
        let ok = unsafe {
            CryptUnprotectData(
                &input,
                &mut description,
                ptr::null(),
                ptr::null_mut(),
                ptr::null_mut(),
                CRYPTPROTECT_UI_FORBIDDEN,
                &mut output,
            )
        };
        // SAFETY: `description` is a `LocalAlloc` buffer when non-null.
        unsafe {
            if !description.is_null() {
                LocalFree(description as *mut core::ffi::c_void);
            }
        }
        if ok == 0 {
            return Err(format!(
                "CryptUnprotectData failed (error {})",
                last_error()
            ));
        }
        Ok(take_blob(output))
    }

    /// Copy a `DataBlob` Windows returned into an owned `Vec` and free the Windows buffer.
    ///
    /// Only called on the success path, where `pb_data` is a `LocalAlloc` buffer of `cb_data` bytes.
    fn take_blob(blob: DataBlob) -> Vec<u8> {
        let bytes = if blob.pb_data.is_null() || blob.cb_data == 0 {
            Vec::new()
        } else {
            // SAFETY: `pb_data` is a `LocalAlloc` buffer of `cb_data` bytes on the success path.
            unsafe { std::slice::from_raw_parts(blob.pb_data, blob.cb_data as usize).to_vec() }
        };
        if !blob.pb_data.is_null() {
            // SAFETY: `pb_data` was allocated by `LocalAlloc` inside the Crypt* call above.
            unsafe {
                LocalFree(blob.pb_data as *mut core::ffi::c_void);
            }
        }
        bytes
    }

    #[cfg(test)]
    mod tests {
        #[test]
        fn dpapi_round_trips() {
            let secret = b"hunter2";
            let sealed = super::protect(secret).expect("protect");
            assert_ne!(sealed, secret, "the sealed blob must not be the plaintext");
            let opened = super::unprotect(&sealed).expect("unprotect");
            assert_eq!(opened, secret);
        }

        #[test]
        fn dpapi_unprotect_rejects_foreign_input() {
            assert!(super::unprotect(b"not a dpapi blob").is_err());
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn seal_and_open_round_trip() {
        let secrets = AesGcmSecrets::new([7u8; 32]);
        let sealed = secrets
            .seal(b"https://example.com|id", b"hunter2")
            .expect("seal");
        assert_ne!(sealed, b"hunter2");
        let opened = secrets
            .open(b"https://example.com|id", &sealed)
            .expect("open");
        assert_eq!(opened, b"hunter2");
    }

    #[test]
    fn the_aad_binds_the_record_to_its_origin() {
        let secrets = AesGcmSecrets::new([7u8; 32]);
        let sealed = secrets
            .seal(b"https://a.example|id", b"secret")
            .expect("seal");
        // The same ciphertext presented under a different origin must be refused.
        assert_eq!(
            secrets.open(b"https://b.example|id", &sealed),
            Err(VaultError::Corrupt)
        );
    }

    #[test]
    fn a_tampered_record_is_refused() {
        let secrets = AesGcmSecrets::new([7u8; 32]);
        let mut sealed = secrets.seal(b"origin", b"secret").expect("seal");
        let last = sealed.len() - 1;
        sealed[last] ^= 0xff;
        assert_eq!(secrets.open(b"origin", &sealed), Err(VaultError::Corrupt));
    }

    #[test]
    fn a_wrong_password_does_not_open_a_record() {
        let salt = generate_salt();
        let writer = AesGcmSecrets::from_master_password("correct horse", &salt).expect("derive");
        let sealed = writer.seal(b"origin", b"secret").expect("seal");

        let wrong = AesGcmSecrets::from_master_password("wrong horse", &salt).expect("derive");
        assert_eq!(wrong.open(b"origin", &sealed), Err(VaultError::Corrupt));
    }

    #[test]
    fn the_same_password_and_salt_derive_the_same_key() {
        let salt = generate_salt();
        let first = AesGcmSecrets::from_master_password("pw", &salt).expect("derive");
        let second = AesGcmSecrets::from_master_password("pw", &salt).expect("derive");
        let sealed = first.seal(b"origin", b"secret").expect("seal");
        assert_eq!(second.open(b"origin", &sealed).expect("open"), b"secret");
    }

    #[test]
    fn salts_are_unique() {
        assert_ne!(generate_salt(), generate_salt());
    }

    #[test]
    fn a_short_blob_is_corrupt_not_a_crash() {
        let secrets = AesGcmSecrets::new([7u8; 32]);
        assert_eq!(secrets.open(b"origin", b"short"), Err(VaultError::Corrupt));
    }

    #[test]
    fn an_unreadable_salt_is_refused() {
        let err = AesGcmSecrets::from_master_password("pw", "not base64!!").unwrap_err();
        assert_eq!(err, VaultError::Corrupt);
    }
}
