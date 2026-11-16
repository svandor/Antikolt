// SPDX-License-Identifier: MPL-2.0

//! Time as an injected dependency.
//!
//! Every timer in Antikolt (the 10-minute hibernation, the 24-hour session policy, the preview
//! refresh scheduler) is expressed as "now minus a stored timestamp". Nothing reads the wall clock
//! directly, so the whole core is testable by moving a [`ManualClock`] forward.

use std::fmt;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

/// Source of "now", in milliseconds since the Unix epoch.
pub trait Clock: fmt::Debug + Send + Sync {
    fn now_ms(&self) -> u64;
}

/// The real clock.
#[derive(Debug, Default, Clone, Copy)]
pub struct SystemClock;

impl Clock for SystemClock {
    fn now_ms(&self) -> u64 {
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis() as u64
    }
}

/// A clock the caller moves by hand. Used by tests and by deterministic replays.
#[derive(Debug, Default)]
pub struct ManualClock {
    now_ms: AtomicU64,
}

impl ManualClock {
    pub fn new(start_ms: u64) -> Self {
        Self {
            now_ms: AtomicU64::new(start_ms),
        }
    }

    pub fn set(&self, ms: u64) {
        self.now_ms.store(ms, Ordering::SeqCst);
    }

    pub fn advance(&self, delta_ms: u64) {
        self.now_ms.fetch_add(delta_ms, Ordering::SeqCst);
    }
}

impl Clock for ManualClock {
    fn now_ms(&self) -> u64 {
        self.now_ms.load(Ordering::SeqCst)
    }
}

/// `now - earlier`, saturating at zero so a clock that steps backwards cannot resurrect a timer.
pub fn elapsed_ms(now_ms: u64, earlier_ms: u64) -> u64 {
    now_ms.saturating_sub(earlier_ms)
}

/// True when at least `threshold_ms` has passed since `earlier_ms`.
pub fn is_due(now_ms: u64, earlier_ms: u64, threshold_ms: u64) -> bool {
    elapsed_ms(now_ms, earlier_ms) >= threshold_ms
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn manual_clock_is_controllable() {
        let clock = ManualClock::new(1_000);
        assert_eq!(clock.now_ms(), 1_000);
        clock.advance(500);
        assert_eq!(clock.now_ms(), 1_500);
        clock.set(42);
        assert_eq!(clock.now_ms(), 42);
    }

    #[test]
    fn elapsed_saturates_instead_of_underflowing() {
        assert_eq!(elapsed_ms(100, 250), 0);
        assert_eq!(elapsed_ms(250, 100), 150);
    }

    #[test]
    fn due_boundary_is_inclusive() {
        assert!(!is_due(1_099, 1_000, 100));
        assert!(is_due(1_100, 1_000, 100));
    }

    #[test]
    fn system_clock_is_plausible() {
        let now = SystemClock.now_ms();
        // Later than 2020-01-01 and earlier than 2200-01-01: a sanity check, not a precision test.
        assert!(now > 1_577_836_800_000, "clock is set before 2020: {now}");
        assert!(now < 7_258_118_400_000, "clock is set after 2200: {now}");
    }
}
