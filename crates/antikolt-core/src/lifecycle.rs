// SPDX-License-Identifier: MPL-2.0

//! The card lifecycle state machine (`docs/HU/02-ARCHITEKTURA.md` §4).
//!
//! There is no "sleeping tab". A card is either **live** — and then it is the one live surface —
//! or it is **on disk** and costs no RAM. Everything between those two states is a short, timed
//! transition. This module encodes that machine as a pure function (`reduce`) so it can be tested
//! exhaustively without an engine, a clock or a UI.
//!
//! ```text
//! Saved ──Open──▶ Warming ──Opened──▶ Live
//!   ▲                                  │
//!   │                          Leave / 10 min idle
//!   │                                  ▼
//!   └────FreezeFinished── Freezing ◀────┘
//! Saved ──PurgeRequested──▶ Purging ──PurgeFinished──▶ Saved
//! Saved ──Archive──▶ Archived ──Unarchive──▶ Saved
//! ```

use serde::{Deserialize, Serialize};

use crate::clock::{elapsed_ms, is_due};
use crate::profile::ProfileParams;

/// How long a freeze may take before the core accepts a degraded save (`02` §5.4).
pub const FREEZE_DEADLINE_MS: u64 = 2_500;

/// Where a card is in its life.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum CardState {
    /// On the home page; a snapshot exists; no engine surface.
    Saved,
    /// The engine is starting and the page is loading; the preview/skeleton is shown.
    Warming,
    /// The single live surface.
    Live,
    /// A snapshot is being written; the surface is destroyed when it finishes.
    Freezing,
    /// The session policy is clearing data for this card's origin.
    Purging,
    /// Hidden from the grid, snapshot kept, no preview refresh.
    Archived,
}

impl CardState {
    /// States in which this card occupies the one available engine surface.
    pub fn occupies_surface(self) -> bool {
        matches!(
            self,
            CardState::Warming | CardState::Live | CardState::Freezing
        )
    }

    /// States in which the card cannot be opened again until the transition finishes.
    pub fn is_busy(self) -> bool {
        matches!(self, CardState::Freezing | CardState::Purging)
    }
}

/// Conditions that suspend the automatic hibernation timer (`02` §4.2).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
pub struct Exemptions {
    /// The user pinned this card: never hibernate automatically.
    pub pinned: bool,
    /// The page is producing sound.
    pub audible: bool,
    /// Media is playing (covers silent video).
    pub playing: bool,
    /// A download is in flight; destroying the surface would abort it.
    pub downloading: bool,
    /// An authentication popup is open; freezing would break the login flow.
    pub popup_open: bool,
}

impl Exemptions {
    /// Any exemption that suspends the idle timer.
    pub fn suspends_idle_timer(&self) -> bool {
        self.pinned || self.audible || self.playing || self.downloading || self.popup_open
    }

    /// Exemptions that postpone leaving the card as well, because freezing would lose real work.
    pub fn blocks_leaving(&self) -> bool {
        self.downloading || self.popup_open
    }
}

/// The mutable state of one card.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct CardRuntime {
    pub state: CardState,
    /// When the card was opened live (0 if never).
    pub opened_at_ms: u64,
    /// Last user input while live; the idle timer counts from here.
    pub last_input_at_ms: u64,
    pub exemptions: Exemptions,
    /// When the current freeze started, if one is in flight.
    pub freeze_started_at_ms: Option<u64>,
}

impl CardRuntime {
    /// A freshly imported or restored card.
    pub fn saved() -> Self {
        Self {
            state: CardState::Saved,
            opened_at_ms: 0,
            last_input_at_ms: 0,
            exemptions: Exemptions::default(),
            freeze_started_at_ms: None,
        }
    }

    /// A card that was saved at an earlier point in time.
    pub fn saved_at(now_ms: u64) -> Self {
        Self {
            last_input_at_ms: now_ms,
            ..Self::saved()
        }
    }

    pub fn idle_for_ms(&self, now_ms: u64) -> u64 {
        elapsed_ms(now_ms, self.last_input_at_ms)
    }

    pub fn is_idle_beyond(&self, now_ms: u64, params: &ProfileParams) -> bool {
        is_due(now_ms, self.last_input_at_ms, params.idle_hibernate_ms)
    }

    pub fn freeze_is_overdue(&self, now_ms: u64) -> bool {
        match self.freeze_started_at_ms {
            Some(started) => is_due(now_ms, started, FREEZE_DEADLINE_MS),
            None => false,
        }
    }
}

/// Inputs the core feeds into the machine.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Event {
    /// The user asked to open the card from the home page.
    Open,
    /// The engine reported the page as loaded and painted.
    Opened,
    /// Any user interaction (key, pointer, wheel, focus).
    Input,
    /// Audible/playing/download/popup state changed.
    Exemptions(Exemptions),
    /// The user left the card (went back to the home page or to another card).
    Leave,
    /// The snapshot writer started.
    FreezeStarted,
    /// The snapshot completed successfully.
    FreezeFinished,
    /// The snapshot exceeded [`FREEZE_DEADLINE_MS`]; accept a degraded save.
    FreezeTimedOut,
    /// The renderer process died.
    RendererGone,
    /// The session policy asked for this card's origin data to be cleared.
    PurgeRequested,
    /// The clear operation finished.
    PurgeFinished,
    /// Hide the card from the grid (keep the snapshot).
    Archive,
    /// Bring an archived card back.
    Unarchive,
    /// Periodic timer tick; drives the idle and freeze-deadline rules.
    Tick,
}

/// Side effects the caller must perform. The machine itself touches nothing.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Action {
    /// Ask the engine for a full-state snapshot (MHTML + metadata) **before** destroying it.
    PrepareSnapshot,
    /// Tear down the engine surface; this is what actually gives the RAM back.
    DestroySurface,
    /// Clear this card's origin data inside its container partition.
    WipeOriginData,
    /// Warn the user that the card will be saved shortly (the discreet bar in `02` §4.2).
    WarnSoonHibernating,
    /// The save was degraded (preview + scroll only); tell the user.
    MarkDegradedSave,
    /// Write an audit trail entry; never contains page content or secrets.
    Record,
}

/// Informational outcomes that are not side effects.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Notice {
    /// Leaving did not freeze the card because something would have been lost.
    FreezeDeferred,
    /// The event does not apply in the current state; nothing happened.
    Ignored,
}

/// Why an event was rejected (kept separate from [`Notice`] so callers can surface real errors).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RejectReason {
    /// The card is mid-freeze or mid-purge.
    Busy,
    /// The event only makes sense in another state.
    WrongState,
}

/// The result of feeding one event into the machine.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Transition {
    pub next: CardRuntime,
    pub actions: Vec<Action>,
    pub notices: Vec<Notice>,
    pub rejected: Option<RejectReason>,
}

impl Transition {
    fn ignored(rt: CardRuntime, notice: Notice) -> Self {
        Self {
            next: rt,
            actions: Vec::new(),
            notices: vec![notice],
            rejected: None,
        }
    }

    fn rejected(rt: CardRuntime, reason: RejectReason) -> Self {
        Self {
            next: rt,
            actions: Vec::new(),
            notices: Vec::new(),
            rejected: Some(reason),
        }
    }

    fn to(next: CardRuntime, actions: &[Action]) -> Self {
        Self {
            next,
            actions: actions.to_vec(),
            notices: Vec::new(),
            rejected: None,
        }
    }
}

/// Feed one event into the machine.
///
/// `now_ms` and `params` are supplied by the caller, which keeps the function pure and testable.
pub fn reduce(rt: &CardRuntime, event: Event, now_ms: u64, params: &ProfileParams) -> Transition {
    match (rt.state, event) {
        // ---- opening ---------------------------------------------------------------------
        (CardState::Saved | CardState::Archived, Event::Open) => {
            let next = CardRuntime {
                state: CardState::Warming,
                opened_at_ms: now_ms,
                last_input_at_ms: now_ms,
                freeze_started_at_ms: None,
                ..*rt
            };
            Transition::to(next, &[Action::Record])
        }
        (CardState::Warming, Event::Open) => Transition::ignored(*rt, Notice::Ignored),
        (CardState::Freezing | CardState::Purging, Event::Open) => {
            Transition::rejected(*rt, RejectReason::Busy)
        }

        (CardState::Warming, Event::Opened) => {
            let next = CardRuntime {
                state: CardState::Live,
                opened_at_ms: now_ms,
                last_input_at_ms: now_ms,
                ..*rt
            };
            Transition::to(next, &[])
        }

        // ---- interaction -----------------------------------------------------------------
        (CardState::Live, Event::Input) => {
            let next = CardRuntime {
                last_input_at_ms: now_ms,
                ..*rt
            };
            Transition::to(next, &[])
        }
        (CardState::Live, Event::Exemptions(exemptions)) => {
            let next = CardRuntime { exemptions, ..*rt };
            Transition::to(next, &[])
        }
        (_, Event::Input) | (_, Event::Exemptions(_)) => Transition::ignored(*rt, Notice::Ignored),

        // ---- leaving ---------------------------------------------------------------------
        (CardState::Live, Event::Leave) => {
            if rt.exemptions.blocks_leaving() {
                Transition::ignored(*rt, Notice::FreezeDeferred)
            } else {
                let next = CardRuntime {
                    state: CardState::Freezing,
                    freeze_started_at_ms: Some(now_ms),
                    ..*rt
                };
                Transition::to(next, &[Action::PrepareSnapshot])
            }
        }
        (CardState::Saved | CardState::Archived, Event::Leave) => {
            Transition::ignored(*rt, Notice::Ignored)
        }
        // Leaving before the page even finished loading cancels the load; there is nothing to save.
        (CardState::Warming, Event::Leave) => {
            let next = CardRuntime {
                state: CardState::Saved,
                freeze_started_at_ms: None,
                ..*rt
            };
            Transition::to(next, &[Action::DestroySurface])
        }

        // ---- freezing --------------------------------------------------------------------
        (CardState::Live, Event::FreezeStarted) => {
            let next = CardRuntime {
                state: CardState::Freezing,
                freeze_started_at_ms: Some(now_ms),
                ..*rt
            };
            Transition::to(next, &[Action::PrepareSnapshot])
        }
        (CardState::Freezing, Event::FreezeStarted) => Transition::ignored(*rt, Notice::Ignored),
        (CardState::Freezing, Event::FreezeFinished) => {
            let next = CardRuntime {
                state: CardState::Saved,
                freeze_started_at_ms: None,
                ..*rt
            };
            Transition::to(next, &[Action::DestroySurface, Action::Record])
        }
        (CardState::Freezing, Event::FreezeTimedOut) => {
            let next = CardRuntime {
                state: CardState::Saved,
                freeze_started_at_ms: None,
                ..*rt
            };
            Transition::to(
                next,
                &[
                    Action::MarkDegradedSave,
                    Action::DestroySurface,
                    Action::Record,
                ],
            )
        }
        (CardState::Live, Event::FreezeTimedOut) => Transition::ignored(*rt, Notice::Ignored),
        (_, Event::FreezeStarted | Event::FreezeFinished | Event::FreezeTimedOut) => {
            Transition::rejected(*rt, RejectReason::WrongState)
        }

        // ---- renderer crash --------------------------------------------------------------
        (CardState::Warming | CardState::Live | CardState::Freezing, Event::RendererGone) => {
            let next = CardRuntime {
                state: CardState::Saved,
                freeze_started_at_ms: None,
                ..*rt
            };
            Transition::to(next, &[Action::DestroySurface, Action::Record])
        }
        (_, Event::RendererGone) => Transition::ignored(*rt, Notice::Ignored),

        // ---- session policy --------------------------------------------------------------
        (CardState::Saved | CardState::Archived, Event::PurgeRequested) => {
            let next = CardRuntime {
                state: CardState::Purging,
                ..*rt
            };
            Transition::to(next, &[Action::WipeOriginData, Action::Record])
        }
        (CardState::Purging, Event::PurgeFinished) => {
            let next = CardRuntime {
                state: CardState::Saved,
                ..*rt
            };
            Transition::to(next, &[Action::Record])
        }
        (_, Event::PurgeRequested | Event::PurgeFinished) => {
            Transition::rejected(*rt, RejectReason::WrongState)
        }

        // ---- archive ---------------------------------------------------------------------
        (CardState::Saved, Event::Archive) => {
            let next = CardRuntime {
                state: CardState::Archived,
                ..*rt
            };
            Transition::to(next, &[Action::Record])
        }
        (CardState::Archived, Event::Unarchive) => {
            let next = CardRuntime {
                state: CardState::Saved,
                ..*rt
            };
            Transition::to(next, &[Action::Record])
        }
        (_, Event::Archive | Event::Unarchive) => {
            Transition::rejected(*rt, RejectReason::WrongState)
        }

        // ---- timer -----------------------------------------------------------------------
        (CardState::Live, Event::Tick) => {
            if rt.exemptions.suspends_idle_timer() {
                return Transition::ignored(*rt, Notice::Ignored);
            }
            let idle = rt.idle_for_ms(now_ms);
            if !is_due(now_ms, rt.last_input_at_ms, params.idle_hibernate_ms) {
                // Five seconds before the deadline, tell the UI so it can show the saving bar.
                let warning_lead_ms = 5_000;
                if idle + warning_lead_ms >= params.idle_hibernate_ms {
                    return Transition::to(*rt, &[Action::WarnSoonHibernating]);
                }
                return Transition::ignored(*rt, Notice::Ignored);
            }
            let next = CardRuntime {
                state: CardState::Freezing,
                freeze_started_at_ms: Some(now_ms),
                ..*rt
            };
            Transition::to(next, &[Action::PrepareSnapshot, Action::Record])
        }
        (CardState::Freezing, Event::Tick) => {
            if rt.freeze_is_overdue(now_ms) {
                reduce(rt, Event::FreezeTimedOut, now_ms, params)
            } else {
                Transition::ignored(*rt, Notice::Ignored)
            }
        }
        (_, Event::Tick) => Transition::ignored(*rt, Notice::Ignored),

        // Anything else is an event that does not apply in this state. Refusing is safer than
        // guessing: a wrong guess here would destroy a surface that still holds the user's work.
        _ => Transition::rejected(*rt, RejectReason::WrongState),
    }
}

/// A violated global invariant: more than one card occupies the engine surface.
#[derive(Debug, Clone, Copy, PartialEq, Eq, thiserror::Error)]
#[error("invariant I1 violated: {occupying} cards occupy the single engine surface")]
pub struct InvariantViolation {
    pub occupying: usize,
}

/// Check invariant I1 (`docs/HU/02` §1): at most one card may occupy the engine surface.
///
/// The login popup is the one documented exception and is not a card, so it is not counted here.
pub fn ensure_single_live(states: &[CardState]) -> Result<(), InvariantViolation> {
    let occupying = states.iter().filter(|s| s.occupies_surface()).count();
    if occupying > 1 {
        Err(InvariantViolation { occupying })
    } else {
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::profile::ProfileKind;

    fn balanced() -> ProfileParams {
        ProfileKind::Balanced.params().unwrap()
    }

    fn live_at(now: u64) -> CardRuntime {
        CardRuntime {
            state: CardState::Live,
            opened_at_ms: now,
            last_input_at_ms: now,
            ..CardRuntime::saved()
        }
    }

    fn apply(rt: &CardRuntime, event: Event, now: u64, params: &ProfileParams) -> CardRuntime {
        reduce(rt, event, now, params).next
    }

    #[test]
    fn happy_path_reaches_saved_again() {
        let params = balanced();
        let saved = CardRuntime::saved_at(1_000);

        let warming = apply(&saved, Event::Open, 2_000, &params);
        assert_eq!(warming.state, CardState::Warming);

        let live = apply(&warming, Event::Opened, 2_500, &params);
        assert_eq!(live.state, CardState::Live);
        assert_eq!(live.opened_at_ms, 2_500);

        let freezing = apply(&live, Event::Leave, 3_000, &params);
        assert_eq!(freezing.state, CardState::Freezing);
        assert!(freezing.freeze_started_at_ms.is_some());

        let done = apply(&freezing, Event::FreezeFinished, 3_400, &params);
        assert_eq!(done.state, CardState::Saved);
        assert_eq!(done.freeze_started_at_ms, None);
    }

    #[test]
    fn leaving_prepares_a_snapshot_then_destroys_the_surface() {
        let params = balanced();
        let leaving = reduce(&live_at(1_000), Event::Leave, 2_000, &params);
        assert_eq!(leaving.next.state, CardState::Freezing);
        assert_eq!(leaving.actions, vec![Action::PrepareSnapshot]);

        let finished = reduce(&leaving.next, Event::FreezeFinished, 2_100, &params);
        assert!(finished.actions.contains(&Action::DestroySurface));
    }

    #[test]
    fn idle_hibernation_happens_at_ten_minutes_not_before() {
        let params = balanced();
        let rt = live_at(0);

        let early = reduce(&rt, Event::Tick, 10 * 60_000 - 1, &params);
        assert_eq!(
            early.next.state,
            CardState::Live,
            "must not hibernate early"
        );

        let late = reduce(&rt, Event::Tick, 10 * 60_000, &params);
        assert_eq!(late.next.state, CardState::Freezing);
        assert!(late.actions.contains(&Action::PrepareSnapshot));
    }

    #[test]
    fn input_resets_the_idle_timer() {
        let params = balanced();
        let rt = live_at(0);
        let touched = apply(&rt, Event::Input, 9 * 60_000, &params);
        assert_eq!(touched.last_input_at_ms, 9 * 60_000);

        let check = reduce(&touched, Event::Tick, 10 * 60_000, &params);
        assert_eq!(check.next.state, CardState::Live);
    }

    #[test]
    fn a_warning_appears_five_seconds_before_hibernation() {
        let params = balanced();
        let rt = live_at(0);
        let warn = reduce(&rt, Event::Tick, 10 * 60_000 - 5_000, &params);
        assert_eq!(warn.next.state, CardState::Live);
        assert_eq!(warn.actions, vec![Action::WarnSoonHibernating]);
    }

    #[test]
    fn every_exemption_suspends_the_idle_timer() {
        let params = balanced();
        let cases = [
            Exemptions {
                pinned: true,
                ..Default::default()
            },
            Exemptions {
                audible: true,
                ..Default::default()
            },
            Exemptions {
                playing: true,
                ..Default::default()
            },
            Exemptions {
                downloading: true,
                ..Default::default()
            },
            Exemptions {
                popup_open: true,
                ..Default::default()
            },
        ];
        for exemptions in cases {
            let rt = CardRuntime {
                exemptions,
                ..live_at(0)
            };
            let late = reduce(&rt, Event::Tick, 60 * 60_000, &params);
            assert_eq!(late.next.state, CardState::Live, "exemption {exemptions:?}");
        }
    }

    #[test]
    fn leaving_defers_when_it_would_lose_real_work() {
        let params = balanced();
        for exemptions in [
            Exemptions {
                downloading: true,
                ..Default::default()
            },
            Exemptions {
                popup_open: true,
                ..Default::default()
            },
        ] {
            let rt = CardRuntime {
                exemptions,
                ..live_at(0)
            };
            let transition = reduce(&rt, Event::Leave, 5_000, &params);
            assert_eq!(transition.next.state, CardState::Live);
            assert_eq!(transition.notices, vec![Notice::FreezeDeferred]);
            assert!(!transition.actions.contains(&Action::PrepareSnapshot));
        }
        // An audible page is fine to hibernate when the user leaves: sound is not work.
        let playing = CardRuntime {
            exemptions: Exemptions {
                playing: true,
                ..Default::default()
            },
            ..live_at(0)
        };
        assert_eq!(
            reduce(&playing, Event::Leave, 5_000, &params).next.state,
            CardState::Freezing
        );
    }

    #[test]
    fn freeze_timeout_degrades_instead_of_hanging() {
        let params = balanced();
        let freezing = apply(&live_at(0), Event::Leave, 0, &params);
        let early = reduce(&freezing, Event::Tick, FREEZE_DEADLINE_MS - 1, &params);
        assert_eq!(early.next.state, CardState::Freezing);

        let late = reduce(&freezing, Event::Tick, FREEZE_DEADLINE_MS, &params);
        assert_eq!(late.next.state, CardState::Saved);
        assert!(late.actions.contains(&Action::MarkDegradedSave));
        assert!(late.actions.contains(&Action::DestroySurface));
    }

    #[test]
    fn renderer_crash_returns_the_card_to_disk() {
        let params = balanced();
        let transition = reduce(&live_at(0), Event::RendererGone, 100, &params);
        assert_eq!(transition.next.state, CardState::Saved);
        assert!(transition.actions.contains(&Action::DestroySurface));
    }

    #[test]
    fn purge_round_trip() {
        let params = balanced();
        let rt = CardRuntime::saved_at(0);
        let purging = reduce(&rt, Event::PurgeRequested, 10, &params);
        assert_eq!(purging.next.state, CardState::Purging);
        assert_eq!(purging.actions[0], Action::WipeOriginData);

        let done = reduce(&purging.next, Event::PurgeFinished, 20, &params);
        assert_eq!(done.next.state, CardState::Saved);
    }

    #[test]
    fn archive_round_trip() {
        let params = balanced();
        let rt = CardRuntime::saved_at(0);
        let archived = apply(&rt, Event::Archive, 10, &params);
        assert_eq!(archived.state, CardState::Archived);
        let back = apply(&archived, Event::Unarchive, 20, &params);
        assert_eq!(back.state, CardState::Saved);
    }

    #[test]
    fn busy_cards_refuse_to_open() {
        let params = balanced();
        let rt = CardRuntime {
            state: CardState::Freezing,
            ..CardRuntime::saved_at(0)
        };
        let transition = reduce(&rt, Event::Open, 10, &params);
        assert_eq!(transition.rejected, Some(RejectReason::Busy));
        assert_eq!(transition.next.state, CardState::Freezing);
    }

    #[test]
    fn impossible_transitions_are_rejected_not_guessed() {
        let params = balanced();
        let saved = CardRuntime::saved_at(0);
        // Finishing a freeze that never started.
        assert_eq!(
            reduce(&saved, Event::FreezeFinished, 10, &params).rejected,
            Some(RejectReason::WrongState)
        );
        // Purging a live card: the caller must hibernate it first.
        assert_eq!(
            reduce(&live_at(0), Event::PurgeRequested, 10, &params).rejected,
            Some(RejectReason::WrongState)
        );
    }

    #[test]
    fn only_one_card_may_occupy_the_surface() {
        assert!(ensure_single_live(&[CardState::Live, CardState::Saved]).is_ok());
        assert!(ensure_single_live(&[CardState::Live, CardState::Warming]).is_err());
        assert!(ensure_single_live(&[CardState::Freezing]).is_ok());
        assert_eq!(
            ensure_single_live(&[
                CardState::Live,
                CardState::Saved,
                CardState::Freezing,
                CardState::Archived
            ])
            .unwrap_err()
            .occupying,
            2
        );
    }

    #[test]
    fn tick_is_harmless_in_every_non_live_state() {
        let params = balanced();
        for state in [
            CardState::Saved,
            CardState::Warming,
            CardState::Purging,
            CardState::Archived,
        ] {
            let rt = CardRuntime {
                state,
                ..CardRuntime::saved_at(0)
            };
            let transition = reduce(&rt, Event::Tick, 10_000_000, &params);
            assert_eq!(transition.next.state, state);
            assert!(transition.actions.is_empty());
        }
    }
}
