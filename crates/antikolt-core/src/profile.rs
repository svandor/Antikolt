// SPDX-License-Identifier: MPL-2.0

//! Resource profiles (ADR-020).
//!
//! A profile is a *bundle* of tuning values, not a single switch: how long a card may stay live,
//! whether the engine host is kept warm, whether previews are refreshed in the background, what
//! video display cap applies, and where the memory governor's warning and hard limits sit.
//!
//! The numbers are engineering estimates that `docs/HU/03-MEMORIA.md` turns into measured budgets;
//! they are deliberately in one place so that changing them is a one-line diff.

use serde::{Deserialize, Serialize};

/// The five shipped profiles. `Manual` carries no built-in values: it means "use what the user set".
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ProfileKind {
    /// Desktop machine on mains power: stay warm, full quality.
    Fast,
    /// Default: warm engine, 10-minute hibernation, 1080p cap.
    Balanced,
    /// Laptop on battery: engine sleeps after 2 minutes, no preview refresh, 720p cap.
    Saver,
    /// Memory pressure: engine killed immediately when idle, 480p cap.
    Ultra,
    /// Everything explicit; no automatic switching.
    Manual,
}

impl ProfileKind {
    /// Every kind except `Manual`, i.e. the ones that carry built-in values.
    pub const BUILT_IN: [ProfileKind; 4] = [
        ProfileKind::Fast,
        ProfileKind::Balanced,
        ProfileKind::Saver,
        ProfileKind::Ultra,
    ];

    pub fn params(self) -> Option<ProfileParams> {
        match self {
            ProfileKind::Fast => Some(ProfileParams {
                kind: self,
                idle_hibernate_ms: 15 * 60_000,
                engine_warm_idle_shutdown_ms: None,
                preview_refresh: true,
                video_max_height: 2160,
                memory_warn_bytes: 1_000 * 1024 * 1024,
                memory_hard_bytes: 1_300 * 1024 * 1024,
            }),
            ProfileKind::Balanced => Some(ProfileParams {
                kind: self,
                idle_hibernate_ms: 10 * 60_000,
                engine_warm_idle_shutdown_ms: None,
                preview_refresh: true,
                video_max_height: 1080,
                memory_warn_bytes: 850 * 1024 * 1024,
                memory_hard_bytes: 1_000 * 1024 * 1024,
            }),
            ProfileKind::Saver => Some(ProfileParams {
                kind: self,
                idle_hibernate_ms: 5 * 60_000,
                engine_warm_idle_shutdown_ms: Some(2 * 60_000),
                preview_refresh: false,
                video_max_height: 720,
                memory_warn_bytes: 650 * 1024 * 1024,
                memory_hard_bytes: 800 * 1024 * 1024,
            }),
            ProfileKind::Ultra => Some(ProfileParams {
                kind: self,
                idle_hibernate_ms: 2 * 60_000,
                engine_warm_idle_shutdown_ms: Some(0),
                preview_refresh: false,
                video_max_height: 480,
                memory_warn_bytes: 450 * 1024 * 1024,
                memory_hard_bytes: 600 * 1024 * 1024,
            }),
            ProfileKind::Manual => None,
        }
    }

    pub fn default_for_power(on_ac_power: bool) -> ProfileKind {
        if on_ac_power {
            ProfileKind::Balanced
        } else {
            ProfileKind::Saver
        }
    }

    /// Automatic profile for the current conditions.
    ///
    /// Precedence: memory pressure beats everything (a warning on battery drops to `Saver`, a hard
    /// limit drops to `Ultra` even on mains power); otherwise mains power means `Balanced`,
    /// battery means `Saver`.
    pub fn auto(on_ac_power: bool, pressure: MemoryPressure) -> ProfileKind {
        match pressure {
            MemoryPressure::Hard => ProfileKind::Ultra,
            MemoryPressure::Warn if !on_ac_power => ProfileKind::Saver,
            MemoryPressure::Warn => ProfileKind::Balanced,
            MemoryPressure::Normal if on_ac_power => ProfileKind::Balanced,
            MemoryPressure::Normal => ProfileKind::Saver,
        }
    }
}

/// The concrete values behind a profile.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct ProfileParams {
    pub kind: ProfileKind,
    /// How long a live card may be idle before it is hibernated (`docs/HU/02` §4.2).
    pub idle_hibernate_ms: u64,
    /// When the engine host is shut down after being idle. `None` keeps it warm (T1),
    /// `Some(0)` shuts it down as soon as no surface is live (T2).
    pub engine_warm_idle_shutdown_ms: Option<u64>,
    /// Whether the idle preview refresher may run (`02` §12).
    pub preview_refresh: bool,
    /// Cap on the displayed height of top-level videos (ADR-020/ADR-076). This is a *display* cap, not a
    /// decode-resolution cap: CEF exposes no per-page video decode-resolution limit, so the cap limits the
    /// rendered box rather than the decoded stream, and it reaches only top-level `video` elements (a
    /// cross-origin iframe keeps its own document).
    pub video_max_height: u32,
    /// Governor band boundary: above this the core starts freeing background resources.
    pub memory_warn_bytes: u64,
    /// Governor band boundary: above this the core takes aggressive action.
    pub memory_hard_bytes: u64,
}

/// Memory pressure class, derived from the measured private working set of the process tree.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum MemoryPressure {
    Normal,
    Warn,
    Hard,
}

impl ProfileParams {
    pub fn pressure(&self, used_bytes: u64) -> MemoryPressure {
        if used_bytes >= self.memory_hard_bytes {
            MemoryPressure::Hard
        } else if used_bytes >= self.memory_warn_bytes {
            MemoryPressure::Warn
        } else {
            MemoryPressure::Normal
        }
    }

    /// Whether the engine host may be shut down now, since no surface is live.
    pub fn engine_shutdown_due(&self, idle_for_ms: u64) -> bool {
        match self.engine_warm_idle_shutdown_ms {
            None => false,
            Some(threshold) => idle_for_ms >= threshold,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn balanced_profile_matches_the_documented_ten_minutes() {
        let balanced = ProfileKind::Balanced.params().unwrap();
        assert_eq!(balanced.idle_hibernate_ms, 10 * 60_000);
        assert_eq!(balanced.engine_warm_idle_shutdown_ms, None);
        assert_eq!(balanced.video_max_height, 1080);
    }

    #[test]
    fn profiles_get_gradually_more_economical() {
        let fast = ProfileKind::Fast.params().unwrap();
        let balanced = ProfileKind::Balanced.params().unwrap();
        let saver = ProfileKind::Saver.params().unwrap();
        let ultra = ProfileKind::Ultra.params().unwrap();

        assert!(fast.idle_hibernate_ms >= balanced.idle_hibernate_ms);
        assert!(balanced.idle_hibernate_ms >= saver.idle_hibernate_ms);
        assert!(saver.idle_hibernate_ms >= ultra.idle_hibernate_ms);

        assert!(fast.video_max_height >= balanced.video_max_height);
        assert!(balanced.video_max_height >= saver.video_max_height);
        assert!(saver.video_max_height >= ultra.video_max_height);

        assert!(fast.memory_hard_bytes >= balanced.memory_hard_bytes);
        assert!(balanced.memory_hard_bytes >= saver.memory_hard_bytes);
        assert!(saver.memory_hard_bytes >= ultra.memory_hard_bytes);
    }

    #[test]
    fn manual_has_no_builtin_values() {
        assert!(ProfileKind::Manual.params().is_none());
    }

    #[test]
    fn battery_selects_saver_and_mains_selects_balanced() {
        assert_eq!(ProfileKind::default_for_power(true), ProfileKind::Balanced);
        assert_eq!(ProfileKind::default_for_power(false), ProfileKind::Saver);
    }

    #[test]
    fn pressure_overrides_power_source() {
        assert_eq!(
            ProfileKind::auto(true, MemoryPressure::Hard),
            ProfileKind::Ultra
        );
        assert_eq!(
            ProfileKind::auto(false, MemoryPressure::Warn),
            ProfileKind::Saver
        );
        assert_eq!(
            ProfileKind::auto(true, MemoryPressure::Warn),
            ProfileKind::Balanced
        );
        assert_eq!(
            ProfileKind::auto(false, MemoryPressure::Normal),
            ProfileKind::Saver
        );
    }

    #[test]
    fn pressure_bands_are_inclusive_at_the_boundary() {
        let params = ProfileKind::Balanced.params().unwrap();
        assert_eq!(
            params.pressure(params.memory_warn_bytes - 1),
            MemoryPressure::Normal
        );
        assert_eq!(
            params.pressure(params.memory_warn_bytes),
            MemoryPressure::Warn
        );
        assert_eq!(
            params.pressure(params.memory_hard_bytes),
            MemoryPressure::Hard
        );
    }

    #[test]
    fn ultra_shuts_the_engine_down_immediately() {
        let ultra = ProfileKind::Ultra.params().unwrap();
        assert!(ultra.engine_shutdown_due(0));
    }

    #[test]
    fn balanced_keeps_the_engine_warm() {
        let balanced = ProfileKind::Balanced.params().unwrap();
        assert!(!balanced.engine_shutdown_due(10 * 60_000 * 100));
    }

    #[test]
    fn saver_shuts_down_after_two_minutes() {
        let saver = ProfileKind::Saver.params().unwrap();
        assert!(!saver.engine_shutdown_due(119_999));
        assert!(saver.engine_shutdown_due(120_000));
    }
}
