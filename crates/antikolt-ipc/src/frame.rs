// SPDX-License-Identifier: MPL-2.0

//! Length-prefixed framing for [`Envelope`]s.
//!
//! A frame is `[u32 little-endian payload length][JSON payload]`. The decoder is total: it never
//! panics on hostile input, it rejects anything above [`crate::MAX_FRAME_BYTES`], and it reports
//! how many bytes a complete frame consumed so back-to-back frames in one buffer work.

use crate::message::Envelope;
use crate::MAX_FRAME_BYTES;

#[derive(Debug, thiserror::Error)]
pub enum FrameError {
    #[error("frame of {len} bytes exceeds the {max} byte limit")]
    TooLarge { len: usize, max: usize },
    #[error("incomplete frame: need {need} bytes, have {have}")]
    Incomplete { need: usize, have: usize },
    #[error("protocol version {got} is not supported (this build speaks {expected})")]
    VersionMismatch { got: u16, expected: u16 },
    #[error("malformed frame payload: {source}")]
    Malformed {
        #[from]
        source: serde_json::Error,
    },
}

/// Serialize and prefix an envelope.
pub fn encode_frame(envelope: &Envelope) -> Result<Vec<u8>, FrameError> {
    let payload = serde_json::to_vec(envelope)?;
    if payload.len() > MAX_FRAME_BYTES {
        return Err(FrameError::TooLarge {
            len: payload.len(),
            max: MAX_FRAME_BYTES,
        });
    }
    let mut out = Vec::with_capacity(4 + payload.len());
    out.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    out.extend_from_slice(&payload);
    Ok(out)
}

/// Decode one frame from the front of `bytes`.
///
/// Returns the envelope and the number of bytes it occupied, so callers can loop over a buffer
/// that holds several frames. Truncated input yields [`FrameError::Incomplete`] — the caller
/// reads more and retries; it is never an error to have only part of a frame yet.
pub fn decode_frame(bytes: &[u8]) -> Result<(Envelope, usize), FrameError> {
    if bytes.len() < 4 {
        return Err(FrameError::Incomplete {
            need: 4,
            have: bytes.len(),
        });
    }
    let mut header = [0u8; 4];
    header.copy_from_slice(&bytes[..4]);
    let len = u32::from_le_bytes(header) as usize;

    if len > MAX_FRAME_BYTES {
        return Err(FrameError::TooLarge {
            len,
            max: MAX_FRAME_BYTES,
        });
    }

    let need = 4 + len;
    if bytes.len() < need {
        return Err(FrameError::Incomplete {
            need,
            have: bytes.len(),
        });
    }

    let envelope: Envelope = serde_json::from_slice(&bytes[4..need])?;
    if envelope.version != crate::PROTOCOL_VERSION {
        return Err(FrameError::VersionMismatch {
            got: envelope.version,
            expected: crate::PROTOCOL_VERSION,
        });
    }

    Ok((envelope, need))
}
