// SPDX-License-Identifier: MPL-2.0

//! Snapshot manifests: what is stored on disk for a hibernated card (`docs/HU/02` §5).
//!
//! A snapshot is a manifest plus content-addressed blobs. The manifest is the authoritative
//! record of *what* the card looked like; the blobs hold the bytes. This module defines the
//! manifest, its validation rules and the digest type, so the storage layer and the migration
//! schema cannot drift apart.
//!
//! Note on hashing: [`Digest`] is final, but the SHA-256 implementation that fills it arrives with
//! the storage crate (`docs/HU/16-FEJLESZTESI-NAPLO.md` records why it is not here yet). Until
//! then callers construct digests from values they already have, and every digest is validated.

use serde::{Deserialize, Serialize};

use antikolt_ipc::Viewport;

use crate::ids::Id;

/// Length of a SHA-256 digest.
pub const DIGEST_LEN: usize = 32;

/// A 32-byte content digest, rendered as lowercase hex.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(transparent)]
pub struct Digest(#[serde(with = "hex_bytes")] [u8; DIGEST_LEN]);

impl std::fmt::Debug for Digest {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "Digest({})", self.to_hex())
    }
}

impl Digest {
    /// All zeroes; used as a placeholder before a blob is written. Never valid for a stored blob.
    pub const ZERO: Digest = Digest([0u8; DIGEST_LEN]);

    pub fn from_bytes(bytes: [u8; DIGEST_LEN]) -> Self {
        Self(bytes)
    }

    pub fn as_bytes(&self) -> &[u8; DIGEST_LEN] {
        &self.0
    }

    /// Parse 64 lowercase or uppercase hex characters.
    pub fn from_hex(input: &str) -> Result<Self, DigestError> {
        if input.len() != DIGEST_LEN * 2 {
            return Err(DigestError::WrongLength(input.len()));
        }
        let bytes = input.as_bytes();
        let mut out = [0u8; DIGEST_LEN];
        for (index, slot) in out.iter_mut().enumerate() {
            let hi = hex_value(bytes[index * 2]).ok_or(DigestError::BadCharacter(
                input.chars().nth(index * 2).unwrap_or('?'),
            ))?;
            let lo = hex_value(bytes[index * 2 + 1]).ok_or(DigestError::BadCharacter(
                input.chars().nth(index * 2 + 1).unwrap_or('?'),
            ))?;
            *slot = (hi << 4) | lo;
        }
        Ok(Self(out))
    }

    pub fn to_hex(&self) -> String {
        let mut out = String::with_capacity(DIGEST_LEN * 2);
        for byte in self.0 {
            out.push(char::from_digit((byte >> 4) as u32, 16).unwrap_or('0'));
            out.push(char::from_digit((byte & 0x0F) as u32, 16).unwrap_or('0'));
        }
        out
    }

    /// A digest of all zeroes means "no blob written yet", which must never reach storage.
    pub fn is_unset(&self) -> bool {
        self.0.iter().all(|b| *b == 0)
    }
}

fn hex_value(byte: u8) -> Option<u8> {
    match byte {
        b'0'..=b'9' => Some(byte - b'0'),
        b'a'..=b'f' => Some(byte - b'a' + 10),
        b'A'..=b'F' => Some(byte - b'A' + 10),
        _ => None,
    }
}

/// Errors from parsing a digest.
#[derive(Debug, Clone, Copy, PartialEq, Eq, thiserror::Error)]
pub enum DigestError {
    #[error("a sha256 digest is 64 hex characters, got {0}")]
    WrongLength(usize),
    #[error("invalid hex character {0:?}")]
    BadCharacter(char),
}

mod hex_bytes {
    use super::{DigestError, DIGEST_LEN};
    use serde::{Deserialize, Deserializer, Serializer};

    pub fn serialize<S: Serializer>(
        bytes: &[u8; DIGEST_LEN],
        serializer: S,
    ) -> Result<S::Ok, S::Error> {
        let mut out = String::with_capacity(DIGEST_LEN * 2);
        for byte in bytes {
            out.push(char::from_digit((byte >> 4) as u32, 16).unwrap_or('0'));
            out.push(char::from_digit((byte & 0x0F) as u32, 16).unwrap_or('0'));
        }
        serializer.serialize_str(&out)
    }

    pub fn deserialize<'de, D: Deserializer<'de>>(
        deserializer: D,
    ) -> Result<[u8; DIGEST_LEN], D::Error> {
        let text = String::deserialize(deserializer)?;
        let parsed = super::Digest::from_hex(&text).map_err(|error| match error {
            DigestError::WrongLength(len) => {
                serde::de::Error::invalid_length(len, &"64 hex characters")
            }
            DigestError::BadCharacter(c) => serde::de::Error::custom(format!("bad hex {c:?}")),
        })?;
        Ok(*parsed.as_bytes())
    }
}

/// Why a snapshot was taken; used for the audit trail and for the time-travel UI.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SnapshotReason {
    /// The user switched away from the card.
    Switch,
    /// The ten-minute idle timer fired.
    Idle,
    /// The app is closing.
    Close,
    /// The renderer died; this is a recovery snapshot.
    Crash,
    /// The user pressed "preserve now".
    Manual,
}

/// A scroll anchor: where the topmost visible element was, so a reload can find the place again
/// even when the page lazy-loads its content.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Anchor {
    pub selector: String,
    pub dx: i32,
    pub dy: i32,
}

/// The manifest stored for every hibernated card.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct SnapshotManifest {
    /// Manifest format version; bump on incompatible changes.
    pub version: u16,
    pub card_id: Id,
    pub created_at_ms: u64,
    pub reason: SnapshotReason,
    pub url: String,
    pub final_url: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub title: Option<String>,
    pub scroll_x: i64,
    pub scroll_y: i64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub anchor: Option<Anchor>,
    pub viewport: Viewport,
    /// The MHTML blob: the full DOM with its resources inlined.
    pub mhtml: Digest,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub text: Option<Digest>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub preview: Option<Digest>,
    /// Average relative luminance the preview showed, in permille (0–1000).
    ///
    /// This is how the dark-mode ladder verifies its own work (`02` §10.3).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub luminance_permille: Option<u16>,
    /// Total bytes of the blobs this manifest references.
    pub bytes: u64,
    /// Engine that produced the snapshot (`cef`, later others). A different engine cannot reuse it.
    pub engine: String,
}

/// The current manifest format.
pub const MANIFEST_VERSION: u16 = 1;

#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum ManifestError {
    #[error("unsupported manifest version {0} (this build understands {MANIFEST_VERSION})")]
    UnsupportedVersion(u16),
    #[error("url {0:?} is not a navigable page url")]
    InvalidUrl(String),
    #[error("url contains control characters")]
    ControlCharacterInUrl,
    #[error("negative scroll offset ({x}, {y})")]
    NegativeScroll { x: i64, y: i64 },
    #[error("luminance {0} is outside 0..=1000 permille")]
    LuminanceOutOfRange(u16),
    #[error("the MHTML blob digest is unset")]
    MissingMhtml,
    #[error("the snapshot claims zero bytes")]
    EmptySnapshot,
    #[error("engine name is empty")]
    EmptyEngine,
    #[error("title contains control characters")]
    ControlCharacterInTitle,
}

impl SnapshotManifest {
    /// Validate everything that would make the record unusable or dangerous to replay.
    pub fn validate(&self) -> Result<(), ManifestError> {
        if self.version != MANIFEST_VERSION {
            return Err(ManifestError::UnsupportedVersion(self.version));
        }
        if self.url.chars().any(|c| c.is_control())
            || self.final_url.chars().any(|c| c.is_control())
        {
            return Err(ManifestError::ControlCharacterInUrl);
        }
        if let Some(title) = &self.title {
            if title.chars().any(|c| c.is_control() && c != '\t') {
                return Err(ManifestError::ControlCharacterInTitle);
            }
        }
        if !is_navigable(&self.url) || !is_navigable(&self.final_url) {
            return Err(ManifestError::InvalidUrl(self.url.clone()));
        }
        if self.scroll_x < 0 || self.scroll_y < 0 {
            return Err(ManifestError::NegativeScroll {
                x: self.scroll_x,
                y: self.scroll_y,
            });
        }
        if let Some(luminance) = self.luminance_permille {
            if luminance > 1000 {
                return Err(ManifestError::LuminanceOutOfRange(luminance));
            }
        }
        if self.mhtml.is_unset() {
            return Err(ManifestError::MissingMhtml);
        }
        if self.bytes == 0 {
            return Err(ManifestError::EmptySnapshot);
        }
        if self.engine.trim().is_empty() {
            return Err(ManifestError::EmptyEngine);
        }
        Ok(())
    }

    /// The oldest manifest version this build can still read. Old manifests are never rewritten.
    pub fn is_readable(&self) -> bool {
        self.version == MANIFEST_VERSION
    }
}

/// Schemes a card may point at: the open web, plus the browser's own pages.
fn is_navigable(url: &str) -> bool {
    url.starts_with("https://")
        || url.starts_with("http://")
        || url.starts_with("antikolt://")
        || url.starts_with("file:///")
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample() -> SnapshotManifest {
        SnapshotManifest {
            version: MANIFEST_VERSION,
            card_id: Id::mint(1_700_000_000_000, 1, 7).unwrap(),
            created_at_ms: 1_700_000_100_000,
            reason: SnapshotReason::Switch,
            url: "https://example.com/article".into(),
            final_url: "https://example.com/article?page=1".into(),
            title: Some("Egy cikk".into()),
            scroll_x: 0,
            scroll_y: 4_200,
            anchor: Some(Anchor {
                selector: "#section-2".into(),
                dx: 0,
                dy: -12,
            }),
            viewport: Viewport::new(1440, 900, 125),
            mhtml: Digest::from_hex(&"ab".repeat(32)).unwrap(),
            text: Some(Digest::from_hex(&"cd".repeat(32)).unwrap()),
            preview: Some(Digest::from_hex(&"ef".repeat(32)).unwrap()),
            luminance_permille: Some(180),
            bytes: 2_400_000,
            engine: "cef".into(),
        }
    }

    #[test]
    fn a_complete_manifest_validates() {
        sample().validate().unwrap();
    }

    #[test]
    fn manifest_survives_json_roundtrip() {
        let manifest = sample();
        let json = serde_json::to_string(&manifest).unwrap();
        let back: SnapshotManifest = serde_json::from_str(&json).unwrap();
        assert_eq!(manifest, back);
        // Digests are hex strings on the wire, not arrays of numbers.
        assert!(json.contains(&"ab".repeat(32)));
    }

    #[test]
    fn digests_render_and_parse_as_hex() {
        let digest = Digest::from_hex(&"0123456789abcdef".repeat(4)).unwrap();
        assert_eq!(digest.to_hex(), "0123456789abcdef".repeat(4));
        assert!(!digest.is_unset());
        assert!(Digest::ZERO.is_unset());
    }

    #[test]
    fn digest_parsing_is_strict() {
        assert_eq!(Digest::from_hex("abc"), Err(DigestError::WrongLength(3)));
        assert!(matches!(
            Digest::from_hex(&"zz".repeat(32)),
            Err(DigestError::BadCharacter(_))
        ));
        // Uppercase hex is accepted and normalised on output.
        let upper = Digest::from_hex(&"AB".repeat(32)).unwrap();
        assert_eq!(upper.to_hex(), "ab".repeat(32));
    }

    #[test]
    fn unset_mhtml_is_rejected() {
        let mut manifest = sample();
        manifest.mhtml = Digest::ZERO;
        assert_eq!(manifest.validate(), Err(ManifestError::MissingMhtml));
    }

    #[test]
    fn non_navigable_urls_are_rejected() {
        let mut manifest = sample();
        manifest.url = "javascript:alert(1)".into();
        assert!(matches!(
            manifest.validate(),
            Err(ManifestError::InvalidUrl(_))
        ));

        manifest.url = "https://example.com".into();
        manifest.final_url = "data:text/html,<h1>x</h1>".into();
        assert!(matches!(
            manifest.validate(),
            Err(ManifestError::InvalidUrl(_))
        ));
    }

    #[test]
    fn control_characters_are_rejected() {
        let mut manifest = sample();
        manifest.url = "https://example.com/\u{0}".into();
        assert_eq!(
            manifest.validate(),
            Err(ManifestError::ControlCharacterInUrl)
        );

        manifest.url = "https://example.com".into();
        manifest.title = Some("bad\u{7}title".into());
        assert_eq!(
            manifest.validate(),
            Err(ManifestError::ControlCharacterInTitle)
        );
    }

    #[test]
    fn negative_scroll_is_rejected() {
        let mut manifest = sample();
        manifest.scroll_y = -1;
        assert!(matches!(
            manifest.validate(),
            Err(ManifestError::NegativeScroll { .. })
        ));
    }

    #[test]
    fn luminance_range_is_enforced() {
        let mut manifest = sample();
        manifest.luminance_permille = Some(1001);
        assert_eq!(
            manifest.validate(),
            Err(ManifestError::LuminanceOutOfRange(1001))
        );
        manifest.luminance_permille = Some(1000);
        manifest.validate().unwrap();
    }

    #[test]
    fn future_versions_are_refused_rather_than_misread() {
        let mut manifest = sample();
        manifest.version = MANIFEST_VERSION + 1;
        assert_eq!(
            manifest.validate(),
            Err(ManifestError::UnsupportedVersion(MANIFEST_VERSION + 1))
        );
        assert!(!manifest.is_readable());
    }

    #[test]
    fn empty_engine_and_zero_bytes_are_rejected() {
        let mut manifest = sample();
        manifest.engine = "   ".into();
        assert_eq!(manifest.validate(), Err(ManifestError::EmptyEngine));

        manifest.engine = "cef".into();
        manifest.bytes = 0;
        assert_eq!(manifest.validate(), Err(ManifestError::EmptySnapshot));
    }

    #[test]
    fn optional_blobs_may_be_absent() {
        let mut manifest = sample();
        manifest.text = None;
        manifest.preview = None;
        manifest.anchor = None;
        manifest.title = None;
        manifest.luminance_permille = None;
        manifest.validate().unwrap();
        let json = serde_json::to_string(&manifest).unwrap();
        assert!(!json.contains("preview"));
    }
}
