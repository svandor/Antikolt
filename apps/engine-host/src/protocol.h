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
#include <mutex>
#include <string>

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
    std::string partition;
    int width = 1280;
    int height = 800;
    int protocol = 0;
};

/// Upper bound for one frame. Mirrors antikolt_ipc::MAX_FRAME_BYTES.
constexpr uint32_t kMaxFrameBytes = 16u * 1024u * 1024u;

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
