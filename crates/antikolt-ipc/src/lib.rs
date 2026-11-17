// SPDX-License-Identifier: MPL-2.0

//! The contract between the Antikolt core and an engine host.
//!
//! This crate is deliberately dependency-light and has **no** engine-specific code: it is the
//! language in which the shell/core and any engine (`cef`, later `webkitgtk`, `webview2`) talk.
//! See `docs/HU/02-ARCHITEKTURA.md` §3 for the design rationale.
//!
//! Wire format: a 4-byte little-endian length prefix followed by a JSON-encoded [`Envelope`].
//! The transport is a local socket; frames are rarely on the hot path (pixels travel through the
//! separate frame channel), so JSON is chosen for debuggability over compactness.
//!
//! Rules that callers must respect:
//! - A frame larger than [`MAX_FRAME_BYTES`] is a protocol violation, never a hint to buffer more.
//! - Unknown message kinds, unknown enum values and wrong versions are **errors**, never guesses.
//! - Every request carries an `id`; every reply echoes it in `to`.

#![deny(missing_debug_implementations)]
#![deny(unsafe_code)]

pub mod frame;
pub mod message;
pub mod transport;

/** Bumped only for breaking changes; the host and engine host negotiate in [`message::Message::Hello`].*/
pub const PROTOCOL_VERSION: u16 = 1;

/// Upper bound for a single control frame. Snapshots and pixels do **not** travel in frames.
pub const MAX_FRAME_BYTES: usize = 16 * 1024 * 1024;

pub use frame::{decode_frame, encode_frame, FrameError};
pub use message::{
    Capability, ColorScheme, EngineEvent, EngineInfo, Envelope, ErrorCode, ImageFormat, Message,
    NavigateOpts, PermissionKind, PopupDisposition, ReplyOutcome, SurfaceConfig, SurfaceId,
    SurfaceMetrics, Viewport, World,
};
pub use transport::{EngineLink, LinkError, DEFAULT_TIMEOUT};
