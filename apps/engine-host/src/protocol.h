// SPDX-License-Identifier: MPL-2.0
//
// The engine host's side of the wire protocol (proto/README.md, crates/antikolt-ipc).
//
// The Rust crate is the contract; this file implements exactly that JSON, not a cousin of it:
//
//   {"version":1,"id":42,"message":{"op":"navigate","surface":7,"url":"https://…"}}
//   {"version":1,"id":43,"message":{"op":"reply","to":42,"outcome":{"kind":"ok","json":{…}}}}
//   {"version":1,"id":0,"message":{"op":"event","event":{"type":"load_finished",…}}}
//
// Transport for this milestone: the child process's standard input/output with the same
// length-prefixed frames the Rust side already tests. Standard I/O is chosen because the engine host
// is a child process with exactly one client, which makes it simpler *and* sturdier than a named
// pipe; a local socket only becomes useful when a second client appears.
//
// Rules that keep this safe:
//   * stdout carries **only** frames; every human-readable diagnostic goes to stderr;
//   * a frame above kMaxFrameBytes kills the link, it is never buffered "just in case";
//   * JSON is parsed with CEF's own parser, never with hand-rolled code;
//   * the reader thread only parses and queues; all engine work happens on the UI thread.

#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "include/cef_values.h"

namespace antikolt {

/// A parsed request. Only the fields the host understands are extracted.
struct Command {
    std::string op;
    int64_t id = 0;
    int64_t surface = 0;
    std::string url;
    /// JavaScript to evaluate (`eval`), executed in the page's main world.
    std::string script;
    /// Autofill (`fill_credential`): the credential to write into the page's login form, in the
    /// isolated world (ADR-053). The page's main world never sees these as JS values.
    std::string username;
    std::string password;
    /// WebAuthn (`webauthn_credentials`): the virtual authenticator's id, as returned by `webauthn_setup`.
    std::string authenticator_id;
    std::string partition;
    /// The container the surface belongs to (ADR-071) and the policy that comes with it: the proxy the container
    /// reaches the network through, and the permissions its pages may ask for. The engine keeps the partition as the
    /// storage key and the container as the policy key — two cards of the same container still get their own cache
    /// directory (ADR-009), but they share the proxy and the permission rules.
    std::string container;
    std::string proxy;
    std::string proxy_bypass;
    std::map<std::string, std::string> permissions;
    /// Whether the surface must stay unseen (`create_surface` config `hidden`, ADR-041).
    ///
    /// The background preview worker photographs a page and never shows a window for it: the user is on
    /// the grid and a preview must not steal focus or appear over what they are reading. Same effect the
    /// process-wide `--hidden` flag has, but per surface, so the card and the worker can differ.
    bool hidden = false;
    /// Data control (`clear_origin_data`): the origin whose storage is cleared, and which storages —
    /// CDP `Storage.clearDataForOrigin` identifiers such as `cookies` or `local_storage`. The Rust
    /// contract (crates/antikolt-ipc `Message::ClearOriginData`) sends the list as an array.
    std::string origin;
    std::vector<std::string> storages;
    /// Screenshot encoding (`capture`): `png`, `jpeg` or `webp`. Defaults match the Rust contract
    /// (crates/antikolt-ipc `Message::Capture`), where `format` is `webp`/`png` and `quality` is 80.
    std::string format = "png";
    /// Quality for the lossy encodings, 0..100. Ignored by PNG.
    int quality = 80;
    /// Site theme emulation (`set_media`, ADR-037): `auto` (no override), `dark` or `light` for
    /// `prefers-color-scheme`. Applied through DevTools, so pages that support the query follow it.
    std::string media = "auto";
    /// Dark pages (`set_forced_dark`, ADR-044/ADR-045): the user asked for the page itself to be darkened, and
    /// for *how* — `off`, `invert` (near black), `dim` (soft dark grey) or `warm` (warm dark grey). The engine
    /// injects one named style element, so a mode change is a replacement and `off` removes exactly what this
    /// host added. An empty value means "leave the page alone".
    std::string dark_mode;
    /// Video display cap (`set_video_cap`, ADR-020/ADR-076): the profile's `video_max_height`, in CSS pixels.
    /// 0 means "no cap" — the host removes exactly the style element it added. This is a *display* cap on
    /// top-level `video` elements: CEF exposes no per-page decode-resolution limit, and a cross-origin iframe
    /// keeps its own document (a browser security boundary), so the cap never reaches inside it.
    int video_max_height = 0;
    /// Scrolling (`scroll`): the distance in CSS pixels, and whether the move is animated.
    int scroll_x = 0;
    int scroll_y = 0;
    bool animate = false;
    /// Zoom (`set_zoom`): percent, 100 is the default. The viewport reflows, the pixels do not scale.
    int factor_percent = 100;
    int width = 1280;
    int height = 800;
    int protocol = 0;
    /// Native window handle the shell wants the surface embedded into (`create_surface`); 0 means
    /// the host creates its own top-level window.
    uint64_t parent_window = 0;
    /// Placement inside the parent window, in physical pixels (`create_surface` and `set_bounds`).
    int bounds_x = 0;
    int bounds_y = 0;
    int bounds_width = 0;
    int bounds_height = 0;
    /// Whether the surface should hold keyboard focus (`focus`).
    bool focused = true;
    /// Answer to a held page request (`bridge_reply`, protocol 2): which request, and its payload —
    /// `json` on success or `error` on failure (ADR-034).
    int64_t bridge_request = 0;
    std::string json;
    std::string error;
    /// Output file path (`print_pdf`): where the PDF is written (F7 "nyomtatás").
    std::string path;
    /// Find-in-page (`find`, F7 "Ctrl+F"): the text to search for, whether this call advances to the next
    /// match instead of starting a new search, and the search direction.
    std::string find_text;
    bool find_next = false;
    bool find_forward = true;
};

/// Upper bound for one frame. Mirrors antikolt_ipc::MAX_FRAME_BYTES.
constexpr uint32_t kMaxFrameBytes = 16u * 1024u * 1024u;

/// The engine-link protocol this host speaks (crates/antikolt-ipc `PROTOCOL_VERSION`).
///
/// Version 2 adds `bridge_reply`: a page call marked `request` is answered by the shell instead of the
/// engine, which is what lets the page ask instead of only announce (ADR-034). It is declared here so the
/// parser, the `welcome` handshake and the envelope writer cannot drift apart.
constexpr int kProtocolVersion = 2;

/// A parsed envelope that is not a request we serve (e.g. a reply from the peer).
std::string ParseEnvelopeVersion(const std::string& payload);

/// Parse one frame payload into a command. Returns false on malformed input.
bool ParseCommand(const std::string& payload, Command* out);

/// The stdio link: a reader thread, a thread-safe command queue and a single writer.
class StdioLink {
public:
    static StdioLink& Instance();

    /// Start the reader thread. Safe to call once.
    void Start();

    /// Pop one command, if any. Called from the UI thread.
    bool TryPop(Command* out);

    /// Reply with an optional JSON object result (`outcome.kind = "ok"`).
    void Reply(int64_t request_id, CefRefPtr<CefDictionaryValue> result = nullptr);

    /// Reply with a typed error (`outcome.kind = "err"`).
    void ReplyError(int64_t request_id, const char* code, const char* message);

    /// Emit an engine event: `event.type` plus the given fields.
    void Event(const char* type, CefRefPtr<CefDictionaryValue> payload);

    /// Send an arbitrary message envelope (used for the `welcome` handshake, which is its own
    /// message kind rather than a reply).
    void SendMessage(CefRefPtr<CefDictionaryValue> message);

    /// True while stdin is open.
    bool active() const { return active_; }

private:
    StdioLink() = default;

    void ReaderLoop();
    void Push(const Command& command);
    void WriteEnvelope(CefRefPtr<CefDictionaryValue> message);

    std::mutex queue_mutex_;
    std::deque<Command> queue_;
    std::mutex write_mutex_;
    bool started_ = false;
    bool active_ = true;
};

}  // namespace antikolt
