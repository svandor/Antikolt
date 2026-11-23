// SPDX-License-Identifier: MPL-2.0

//! TOTP (RFC 6238): the time-based one-time codes the vault turns a stored secret into.
//!
//! Pure and time-parameterised like the rest of the crate: `now_ms` is an argument, so the code is
//! testable against the RFC's fixed vectors without waiting for a wall clock. The secret is the
//! standard base32 enrolment string (the form Google Authenticator and every importer use).

use data_encoding::BASE32_NOPAD;
use hmac::{Hmac, Mac};
use sha1::Sha1;
use sha2::{Sha256, Sha512};

/// The algorithm a TOTP secret was enrolled with. RFC 6238 allows SHA-1 (the default), SHA-256 and
/// SHA-512; the choice travels in the credential's `totp_params_json`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TotpAlgorithm {
    Sha1,
    Sha256,
    Sha512,
}

impl TotpAlgorithm {
    pub fn parse(value: &str) -> Option<Self> {
        match value.to_ascii_lowercase().as_str() {
            "sha1" | "sha-1" => Some(Self::Sha1),
            "sha256" | "sha-256" => Some(Self::Sha256),
            "sha512" | "sha-512" => Some(Self::Sha512),
            _ => None,
        }
    }
}

/// Everything that can go wrong computing a code.
#[derive(Debug, Clone, Copy, PartialEq, Eq, thiserror::Error)]
pub enum TotpError {
    #[error("the secret is empty")]
    EmptySecret,
    #[error("the secret is not valid base32")]
    BadSecret,
    #[error("the code length must be 6 or 8 digits, got {0}")]
    BadDigits(u32),
}

/// Compute the TOTP code for `now_ms`.
///
/// `digits` is 6 or 8, `period_seconds` the step (usually 30), `algorithm` the enrolled hash.
pub fn totp_code(
    secret_base32: &str,
    digits: u32,
    period_seconds: u64,
    algorithm: TotpAlgorithm,
    now_ms: u64,
) -> Result<String, TotpError> {
    let secret = secret_base32.trim();
    if secret.is_empty() {
        return Err(TotpError::EmptySecret);
    }
    if digits != 6 && digits != 8 {
        return Err(TotpError::BadDigits(digits));
    }
    let key = BASE32_NOPAD
        .decode(secret.as_bytes())
        .map_err(|_| TotpError::BadSecret)?;
    // The time counter is the number of whole periods elapsed (RFC 6238 §4.2).
    let counter = now_ms / (period_seconds * 1000);
    let counter_bytes = counter.to_be_bytes();
    let truncated = truncate(&hotp(&key, &counter_bytes, algorithm));
    Ok(format!(
        "{:0width$}",
        truncated % 10u32.pow(digits),
        width = digits as usize
    ))
}

/// `HOTP(K, C) = HMAC-SHA(K, C)` over the selected hash, RFC 4226 §5.
fn hotp(key: &[u8], counter_bytes: &[u8], algorithm: TotpAlgorithm) -> Vec<u8> {
    match algorithm {
        TotpAlgorithm::Sha1 => {
            let mut mac =
                <Hmac<Sha1> as Mac>::new_from_slice(key).expect("HMAC accepts any key length");
            mac.update(counter_bytes);
            mac.finalize().into_bytes().to_vec()
        }
        TotpAlgorithm::Sha256 => {
            let mut mac =
                <Hmac<Sha256> as Mac>::new_from_slice(key).expect("HMAC accepts any key length");
            mac.update(counter_bytes);
            mac.finalize().into_bytes().to_vec()
        }
        TotpAlgorithm::Sha512 => {
            let mut mac =
                <Hmac<Sha512> as Mac>::new_from_slice(key).expect("HMAC accepts any key length");
            mac.update(counter_bytes);
            mac.finalize().into_bytes().to_vec()
        }
    }
}

/// Dynamic truncation (RFC 4226 §5.3): take the last nibble as an offset, read four bytes from there.
fn truncate(digest: &[u8]) -> u32 {
    let offset = (digest[digest.len() - 1] & 0x0f) as usize;
    ((u32::from(digest[offset]) & 0x7f) << 24)
        | (u32::from(digest[offset + 1]) << 16)
        | (u32::from(digest[offset + 2]) << 8)
        | u32::from(digest[offset + 3])
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The RFC 6238 test secret: ASCII "12345678901234567890", base32-encoded without padding.
    const SECRET: &str = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";

    #[test]
    fn rfc_6238_sha1_vectors() {
        // Appendix B, SHA-1, 8 digits, T0 = 0, X = 30.
        let cases = [
            (59u64, "94287082"),
            (1_111_111_109, "07081804"),
            (1_111_111_111, "14050471"),
            (1_234_567_890, "89005924"),
            (2_000_000_000, "69279037"),
            (20_000_000_000, "65353130"),
        ];
        for (seconds, expected) in cases {
            let code = totp_code(SECRET, 8, 30, TotpAlgorithm::Sha1, seconds * 1000).expect("code");
            assert_eq!(code, expected, "T = {seconds}");
        }
    }

    #[test]
    fn rfc_6238_sha256_and_sha512_vectors() {
        // The "classic trap": RFC 6238 Appendix B uses a DIFFERENT secret per algorithm — the ASCII
        // seed repeated to 20, 32 and 64 bytes — so running one secret down all three columns yields
        // "vectors that do not match". SHA-1's 20-byte seed is `SECRET`; the other two are longer.
        let sha256_secret = BASE32_NOPAD.encode(b"12345678901234567890123456789012");
        let sha512_secret = BASE32_NOPAD
            .encode(b"1234567890123456789012345678901234567890123456789012345678901234");
        assert_eq!(
            totp_code(&sha256_secret, 8, 30, TotpAlgorithm::Sha256, 59 * 1000).expect("code"),
            "46119246"
        );
        assert_eq!(
            totp_code(&sha512_secret, 8, 30, TotpAlgorithm::Sha512, 59 * 1000).expect("code"),
            "90693936"
        );
    }

    #[test]
    fn six_digits_are_the_default_shape() {
        let code = totp_code(SECRET, 6, 30, TotpAlgorithm::Sha1, 59 * 1000).expect("code");
        assert_eq!(code.len(), 6);
        assert!(code.chars().all(|c| c.is_ascii_digit()));
    }

    #[test]
    fn the_same_instant_gives_the_same_code() {
        let a = totp_code(SECRET, 6, 30, TotpAlgorithm::Sha1, 5_000).expect("code");
        let b = totp_code(SECRET, 6, 30, TotpAlgorithm::Sha1, 25_000).expect("code");
        assert_eq!(a, b, "within one period the code is stable");
    }

    #[test]
    fn bad_input_is_refused() {
        assert_eq!(
            totp_code("", 6, 30, TotpAlgorithm::Sha1, 0),
            Err(TotpError::EmptySecret)
        );
        assert_eq!(
            totp_code("!!!!", 6, 30, TotpAlgorithm::Sha1, 0),
            Err(TotpError::BadSecret)
        );
        assert_eq!(
            totp_code(SECRET, 7, 30, TotpAlgorithm::Sha1, 0),
            Err(TotpError::BadDigits(7))
        );
    }
}
