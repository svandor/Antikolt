// SPDX-License-Identifier: MPL-2.0

//! Message types shared by the core and the engine host.

use serde::{Deserialize, Serialize};

/// Identifier of a web surface (a view of one card or of the home page).
///
/// Surfaces are handed out by the engine host and are only meaningful while it is alive.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(transparent)]
pub struct SurfaceId(pub u64);

/// Which JavaScript world a script runs in.
///
/// `Isolated` is where the password watcher and the dark-mode engine live: the page cannot see or
/// tamper with it. `Main` is the page's own world and must never receive secrets.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum World {
    Isolated,
    Main,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ColorScheme {
    Light,
    Dark,
    /// The "grey" reading mode: desaturated and slightly dimmed.
    Grey,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ImageFormat {
    Webp,
    Png,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PopupDisposition {
    /// A real, temporary window that shares the container partition (OAuth/SSO flows).
    AuthenticatedWindow,
    /// A plain window that does not share credentials.
    PlainWindow,
    /// Refused (e.g. a popup from an untrusted origin while no gesture is active).
    Blocked,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PermissionKind {
    Notifications,
    Camera,
    Microphone,
    Geolocation,
    ClipboardRead,
    ClipboardWrite,
    Midi,
    MidiSysex,
    FontAccess,
    Fullscreen,
    PointerLock,
    Sensors,
    Unknown,
}

/// A capability the engine host reports during [`Message::Hello`].
///
/// The core must degrade gracefully when a capability is missing; it must never assume one.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Capability {
    /// Full DOM snapshot (`Page.captureSnapshot`) for disk hibernation.
    Snapshot,
    /// Screenshot capture for card previews.
    Capture,
    /// Per-surface `prefers-color-scheme` / `forced-colors` emulation.
    MediaEmulation,
    /// Real isolated JavaScript worlds, required for safe autofill.
    IsolatedWorld,
    /// Clearing one origin's data inside a partition, without touching other origins.
    ClearOriginData,
    /// A virtual WebAuthn authenticator (software passkeys).
    VirtualAuthenticator,
    /// Offscreen rendering (required on Wayland).
    OffscreenSurface,
    /// Per-surface proxying.
    SurfaceProxy,
    /// Streaming frame channel instead of a native child window.
    FrameChannel,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct Viewport {
    pub width: u32,
    pub height: u32,
    /// Device pixel ratio in hundredths (`125` = 1.25), to stay exact across platforms.
    pub device_scale_percent: u16,
}

impl Viewport {
    pub fn new(width: u32, height: u32, device_scale_percent: u16) -> Self {
        Self {
            width,
            height,
            device_scale_percent,
        }
    }

    pub fn device_scale(&self) -> f32 {
        f32::from(self.device_scale_percent) / 100.0
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct SurfaceConfig {
    /// Engine partition key, e.g. `persist:c_01h...` for a container or `persist:home`.
    ///
    /// The engine host never receives filesystem paths: it only ever sees partition keys.
    pub partition: String,
    pub viewport: Viewport,
    /// Per-surface proxy specification, already validated by the core.
    pub proxy: Option<String>,
    /// Whether this surface is the user-facing card (only one may exist at a time).
    pub is_primary: bool,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct NavigateOpts {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub referrer: Option<String>,
    #[serde(default)]
    pub bypass_cache: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub timeout_ms: Option<u32>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct EngineInfo {
    /// `cef`, `webkitgtk`, `webview2`, …
    pub name: String,
    pub version: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub chromium_major: Option<u16>,
    #[serde(default)]
    pub capabilities: Vec<Capability>,
}

impl EngineInfo {
    pub fn supports(&self, capability: Capability) -> bool {
        self.capabilities.contains(&capability)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct SurfaceMetrics {
    pub dom_nodes: u32,
    pub js_heap_bytes: u64,
    /// Working set of the engine process this surface belongs to, in bytes.
    pub engine_working_set_bytes: u64,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum ReplyOutcome {
    Ok {
        #[serde(default)]
        json: serde_json::Value,
    },
    Err {
        code: ErrorCode,
        message: String,
        #[serde(default)]
        retryable: bool,
    },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ErrorCode {
    /// The operation exceeded its deadline; the surface may still be usable.
    Timeout,
    /// The engine does not implement this operation (missing capability).
    NotSupported,
    InvalidArgument,
    SurfaceNotFound,
    /// The engine host is already serving its one primary surface.
    EngineBusy,
    /// A secret operation was attempted while the vault was locked.
    VaultLocked,
    /// The operation was cancelled by the user or by the core.
    Cancelled,
    Internal,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum EngineEvent {
    LoadStarted {
        surface: SurfaceId,
        url: String,
    },
    LoadFinished {
        surface: SurfaceId,
        url: String,
        http_status: Option<u16>,
    },
    LoadFailed {
        surface: SurfaceId,
        url: String,
        reason: String,
    },
    TitleChanged {
        surface: SurfaceId,
        title: String,
    },
    FaviconChanged {
        surface: SurfaceId,
        url: String,
    },
    /// Audible/playing state drives the hibernation exemption in `docs/HU/02` §4.2.
    AudibleChanged {
        surface: SurfaceId,
        audible: bool,
        playing: bool,
    },
    /// A form was submitted; the core decides whether to offer saving credentials.
    FormSubmitted {
        surface: SurfaceId,
        origin: String,
    },
    PopupRequested {
        surface: SurfaceId,
        url: String,
        disposition: PopupDisposition,
    },
    PermissionRequested {
        surface: SurfaceId,
        origin: String,
        kind: PermissionKind,
    },
    DownloadStarted {
        surface: SurfaceId,
        url: String,
        suggested_name: String,
    },
    RendererGone {
        surface: SurfaceId,
        reason: String,
    },
    /// Engine host is about to exit; the core must treat all surfaces as gone.
    ShuttingDown {
        reason: String,
    },
}

/// Every frame is an envelope: version, correlation id and the message itself.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Envelope {
    pub version: u16,
    /// Request id (monotonic per peer) or, for replies/events, any unique id.
    pub id: u64,
    pub message: Message,
}

impl Envelope {
    pub fn new(id: u64, message: Message) -> Self {
        Self {
            version: crate::PROTOCOL_VERSION,
            id,
            message,
        }
    }
}

/// The closed set of control messages. Adding a variant is a protocol change: bump
/// [`crate::PROTOCOL_VERSION`] and describe the migration in `docs/HU/02-ARCHITEKTURA.md`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "op", rename_all = "snake_case")]
pub enum Message {
    // ---- handshake ------------------------------------------------------------------------
    Hello {
        protocol: u16,
        engine: String,
    },
    Welcome {
        protocol: u16,
        engine: EngineInfo,
    },
    Ping,

    // ---- surface lifecycle ----------------------------------------------------------------
    CreateSurface {
        config: SurfaceConfig,
    },
    DestroySurface {
        surface: SurfaceId,
    },
    Navigate {
        surface: SurfaceId,
        url: String,
        #[serde(default)]
        opts: NavigateOpts,
    },

    // ---- page interaction -----------------------------------------------------------------
    Eval {
        surface: SurfaceId,
        script: String,
        world: World,
    },
    Scroll {
        surface: SurfaceId,
        x: i32,
        y: i32,
        #[serde(default)]
        animate: bool,
    },
    SetZoom {
        surface: SurfaceId,
        factor_percent: u16,
    },
    SetMedia {
        surface: SurfaceId,
        scheme: ColorScheme,
        #[serde(default)]
        forced_colors: bool,
    },

    // ---- capture / metrics ----------------------------------------------------------------
    Capture {
        surface: SurfaceId,
        format: ImageFormat,
        #[serde(default = "default_quality")]
        quality: u8,
    },
    /// Capture the full document as MHTML for disk hibernation (see `02` §5).
    Snapshot {
        surface: SurfaceId,
    },
    Metrics {
        surface: SurfaceId,
    },

    // ---- data control ---------------------------------------------------------------------
    /// Clear one origin's data inside a partition (`cookies`, `local_storage`, …).
    ClearOriginData {
        partition: String,
        origin: String,
        #[serde(default)]
        storages: Vec<String>,
    },

    // ---- replies and events ---------------------------------------------------------------
    Reply {
        to: u64,
        outcome: ReplyOutcome,
    },
    Event {
        event: EngineEvent,
    },
}

fn default_quality() -> u8 {
    80
}

impl Message {
    /// Requests expect exactly one [`Message::Reply`] with `to == envelope id`.
    pub fn expects_reply(&self) -> bool {
        !matches!(
            self,
            Message::Reply { .. } | Message::Event { .. } | Message::Welcome { .. }
        )
    }
}

/// Baseline: 1280×800 at 100% scale. Kept here so tests and the shell agree on one default.
pub const DEFAULT_VIEWPORT: Viewport = Viewport {
    width: 1280,
    height: 800,
    device_scale_percent: 100,
};
