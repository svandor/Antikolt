// SPDX-License-Identifier: MPL-2.0

//! Identifiers.
//!
//! Antikolt uses time-ordered 128-bit identifiers rendered in Crockford base32 (the ULID layout:
//! 10 characters of millisecond timestamp, then 16 characters of entropy). That is what lets the
//! database keep insertion order for free and lets "recent cards" be a plain index scan.
//!
//! **IDs are not secrets.** The entropy here is a fast, non-cryptographic generator: it exists so
//! that two IDs minted in the same millisecond differ, not to defend against prediction. Anything
//! secret (vault keys, tokens) must come from the OS CSPRNG instead.

use std::fmt;

/// Crockford base32: no `I`, `L`, `O`, `U`, to survive being read aloud or typed by hand.
const ALPHABET: &[u8; 32] = b"0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/// Length of a rendered identifier.
pub const ID_LEN: usize = 26;

const TIME_LEN: usize = 10;
const ENTROPY_LEN: usize = 16;

/// Errors from parsing an identifier.
#[derive(Debug, Clone, Copy, PartialEq, Eq, thiserror::Error)]
pub enum IdError {
    #[error("an id must be {ID_LEN} characters, got {0}")]
    WrongLength(usize),
    #[error("invalid base32 character {0:?} at position {1}")]
    BadCharacter(char, usize),
    #[error("the timestamp part of this id is outside the supported range")]
    TimestampOverflow,
}

/// A time-ordered, rendered identifier.
///
/// The string form is canonical (uppercase Crockford base32); parsing accepts lowercase and the
/// commonly confused characters (`I`, `L` → `1`, `O` → `0`) because humans type them.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Id(String);

impl Id {
    /// Build an id from an explicit millisecond timestamp and 80 bits of entropy.
    ///
    /// Callers normally use [`Id::mint`]; this constructor exists so tests and importers can
    /// produce stable ids.
    pub fn from_parts(timestamp_ms: u64, entropy: [u8; ENTROPY_LEN]) -> Result<Self, IdError> {
        if timestamp_ms >> 48 != 0 {
            return Err(IdError::TimestampOverflow);
        }
        let mut out = String::with_capacity(ID_LEN);

        // 48-bit timestamp, most significant first: 10 characters × 5 bits.
        let mut ts = timestamp_ms;
        let mut time_chars = [0u8; TIME_LEN];
        for slot in time_chars.iter_mut().rev() {
            *slot = (ts & 0x1F) as u8;
            ts >>= 5;
        }
        for c in time_chars {
            out.push(char::from(ALPHABET[c as usize]));
        }

        // 80 bits of entropy: 16 characters × 5 bits.
        for byte in entropy {
            // Each byte contributes 8 bits; pack them two bytes at a time is overkill here, so we
            // emit one character per 5-bit group of the whole entropy buffer.
            let _ = byte;
        }
        let mut acc: u128 = 0;
        for byte in entropy {
            acc = (acc << 8) | u128::from(byte);
        }
        for _ in 0..ENTROPY_LEN {
            let idx = (acc & 0x1F) as u8;
            out.push(char::from(ALPHABET[idx as usize]));
            acc >>= 5;
        }
        // The loop above emits least-significant groups first; reverse them for canonical order.
        let time_part = out[..TIME_LEN].to_string();
        let mut entropy_part: Vec<char> = out[TIME_LEN..].chars().collect();
        entropy_part.reverse();
        let entropy_part: String = entropy_part.into_iter().collect();

        Ok(Self(format!("{time_part}{entropy_part}")))
    }

    /// Mint a fresh id for `timestamp_ms`, deriving entropy from a cheap counter mixed with the
    /// process-specific salt.
    pub fn mint(timestamp_ms: u64, counter: u64, salt: u64) -> Result<Self, IdError> {
        let mut state = salt
            .wrapping_mul(0x9E37_79B9_7F4A_7C15)
            .wrapping_add(counter)
            .wrapping_add(timestamp_ms.rotate_left(17));
        let mut entropy = [0u8; ENTROPY_LEN];
        for slot in entropy.iter_mut() {
            // splitmix64 step: cheap, well-distributed, not cryptographic.
            state = state.wrapping_add(0x9E37_79B9_7F4A_7C15);
            let mut z = state;
            z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
            z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
            *slot = (z ^ (z >> 31)) as u8;
        }
        Self::from_parts(timestamp_ms, entropy)
    }

    /// Parse and canonicalise an identifier.
    pub fn parse(input: &str) -> Result<Self, IdError> {
        if input.len() != ID_LEN {
            return Err(IdError::WrongLength(input.len()));
        }
        let mut canonical = String::with_capacity(ID_LEN);
        for (position, raw) in input.chars().enumerate() {
            let upper = raw.to_ascii_uppercase();
            let normalised = match upper {
                'I' | 'L' => '1',
                'O' => '0',
                other => other,
            };
            if ALPHABET.contains(&(normalised as u8)) {
                canonical.push(normalised);
            } else {
                return Err(IdError::BadCharacter(raw, position));
            }
        }
        Ok(Self(canonical))
    }

    /// The rendered identifier.
    pub fn as_str(&self) -> &str {
        &self.0
    }

    /// Milliseconds since the Unix epoch encoded in the id.
    pub fn timestamp_ms(&self) -> u64 {
        let mut value: u64 = 0;
        for byte in self.0.as_bytes()[..TIME_LEN].iter() {
            value = (value << 5) | decode_base32(*byte).unwrap_or(0) as u64;
        }
        value
    }

    /// True when both ids were minted within the same millisecond.
    pub fn same_instant(&self, other: &Self) -> bool {
        self.timestamp_ms() == other.timestamp_ms()
    }
}

fn decode_base32(byte: u8) -> Option<u8> {
    ALPHABET.iter().position(|c| *c == byte).map(|p| p as u8)
}

impl fmt::Display for Id {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::str::FromStr for Id {
    type Err = IdError;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        Self::parse(s)
    }
}

// Serialization goes through the canonical string form, and deserialization **validates**:
// an id that arrives from disk or from the wire is parsed, never trusted as a raw string. This is
// why the derive is not used here.
impl serde::Serialize for Id {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        serializer.serialize_str(self.as_str())
    }
}

impl<'de> serde::Deserialize<'de> for Id {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        let text = <String as serde::Deserialize>::deserialize(deserializer)?;
        Id::parse(&text).map_err(serde::de::Error::custom)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rendered_length_and_alphabet_are_canonical() {
        let id = Id::mint(1_700_000_000_000, 1, 42).unwrap();
        assert_eq!(id.as_str().len(), ID_LEN);
        // Every byte is in the canonical alphabet: no ambiguous characters can appear.
        assert!(id.as_str().bytes().all(|b| ALPHABET.contains(&b)));
        // No ambiguous characters ever appear in canonical form.
        for forbidden in ['I', 'L', 'O', 'U'] {
            assert!(!id.as_str().contains(forbidden), "{forbidden} in {}", id);
        }
    }

    #[test]
    fn timestamp_roundtrips() {
        for ms in [0u64, 1, 1_700_000_000_000, (1 << 48) - 1] {
            let id = Id::from_parts(ms, [7; ENTROPY_LEN]).unwrap();
            assert_eq!(id.timestamp_ms(), ms);
        }
    }

    #[test]
    fn timestamp_overflow_is_rejected() {
        assert_eq!(
            Id::from_parts(1 << 48, [0; ENTROPY_LEN]),
            Err(IdError::TimestampOverflow)
        );
    }

    #[test]
    fn generation_is_ordered_by_time() {
        let earlier = Id::mint(1_000, 1, 9).unwrap();
        let later = Id::mint(2_000, 1, 9).unwrap();
        assert!(earlier < later, "{earlier} should sort before {later}");
    }

    #[test]
    fn ids_minted_in_the_same_millisecond_differ() {
        let a = Id::mint(1_000, 1, 9).unwrap();
        let b = Id::mint(1_000, 2, 9).unwrap();
        assert_ne!(a, b);
        assert!(a.same_instant(&b));
    }

    #[test]
    fn parsing_normalises_human_confusions() {
        let canonical = "01ARZ3NDEKTSV4RRFFQ69G5FAV";
        let sloppy = canonical.replace('1', "I").replace('0', "O");
        assert_eq!(Id::parse(&sloppy).unwrap().as_str(), canonical);
        assert_eq!(
            Id::parse(canonical.to_lowercase().as_str())
                .unwrap()
                .as_str(),
            canonical
        );
    }

    #[test]
    fn parsing_rejects_bad_input() {
        assert_eq!(Id::parse("short"), Err(IdError::WrongLength(5)));
        assert!(matches!(
            Id::parse("01ARZ3NDEKTSV4RRFFQ69G5FA!"),
            Err(IdError::BadCharacter('!', 25))
        ));
    }

    #[test]
    fn ten_thousand_ids_are_unique() {
        let mut seen = std::collections::HashSet::new();
        let salt = 0xDEAD_BEEF_u64;
        for counter in 0..10_000u64 {
            let id = Id::mint(1_700_000_000_000 + (counter / 100), counter, salt).unwrap();
            assert!(seen.insert(id.clone()), "duplicate id {id}");
        }
    }
}
