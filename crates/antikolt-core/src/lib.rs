// SPDX-License-Identifier: MPL-2.0

//! Antikolt domain core.
//!
//! This crate owns everything that must be correct regardless of the UI or the engine:
//! identifiers, the card lifecycle state machine, the resource profiles, the session policy, the
//! snapshot manifest and the schema migrations. It has **no** dependency on Qt, CEF or any engine,
//! and it is deterministic: every function that needs the current time or entropy takes it as a
//! parameter, so the whole crate is testable without a clock or a network.
//!
//! Design notes live in `docs/HU/02-ARCHITEKTURA.md`; the decisions encoded here are recorded in
//! `docs/HU/05-DONTESEK.md` (ADR-001 single live view, ADR-003 disk hibernation, ADR-009 session
//! policy, ADR-019 session ledger, ADR-020 resource profiles, ADR-025 disk quota, ADR-031 the vault's
//! fail-closed crypto seam).

#![deny(missing_debug_implementations)]
#![deny(unsafe_code)]

pub mod clock;
pub mod ids;
pub mod lifecycle;
pub mod migrations;
pub mod profile;
pub mod session;
pub mod snapshot;
pub mod vault;

/// Crate version, surfaced in the About dialog and in diagnostics.
pub const VERSION: &str = env!("CARGO_PKG_VERSION");

/// Default global blob quota (ADR-025): 50 GB, overridable per device.
pub const DEFAULT_QUOTA_BYTES: u64 = 50 * 1024 * 1024 * 1024;

/// Per-card blob quota (ADR-025): 100 MB.
pub const DEFAULT_CARD_QUOTA_BYTES: u64 = 100 * 1024 * 1024;
