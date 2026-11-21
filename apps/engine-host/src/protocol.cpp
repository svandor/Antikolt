// SPDX-License-Identifier: MPL-2.0

#include "protocol.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

#include "include/cef_parser.h"
#include "include/internal/cef_types.h"

namespace antikolt {

namespace {

/// Monotonic envelope id for messages we originate (replies and events).
std::atomic<int64_t> g_next_envelope_id{1};

uint32_t DecodeLength(const unsigned char header[4]) {
    return static_cast<uint32_t>(header[0]) | (static_cast<uint32_t>(header[1]) << 8) |
           (static_cast<uint32_t>(header[2]) << 16) | (static_cast<uint32_t>(header[3]) << 24);
}

void EncodeLength(uint32_t length, char out[4]) {
    out[0] = static_cast<char>(length & 0xFF);
    out[1] = static_cast<char>((length >> 8) & 0xFF);
    out[2] = static_cast<char>((length >> 16) & 0xFF);
    out[3] = static_cast<char>((length >> 24) & 0xFF);
}

std::string GetString(CefRefPtr<CefDictionaryValue> dictionary, const char* key) {
    if (!dictionary) {
        return {};
    }
    return dictionary->GetString(key).ToString();
}

double GetNumber(CefRefPtr<CefDictionaryValue> dictionary, const char* key) {
    if (!dictionary) {
        return 0.0;
    }
    CefRefPtr<CefValue> value = dictionary->GetValue(key);
    if (!value) {
        return 0.0;
    }
    // Numbers arrive either as integers or as doubles; GetDouble alone returns 0 for an int.
    if (value->GetType() == VTYPE_INT) {
        return static_cast<double>(value->GetInt());
    }
    return value->GetDouble();
}

/// Integer JSON fields must be written as integers: the Rust side decodes `id` and `surface` as
/// `u64`, and `1.0` is not a `u64`.
constexpr cef_json_writer_options_t kWriterOptions = JSON_WRITER_OMIT_DOUBLE_TYPE_PRESERVATION;

CefRefPtr<CefDictionaryValue> GetDictionary(CefRefPtr<CefDictionaryValue> dictionary,
                                            const char* key) {
    if (!dictionary) {
        return nullptr;
    }
    CefRefPtr<CefValue> value = dictionary->GetValue(key);
    if (!value || value->GetType() != VTYPE_DICTIONARY) {
        return nullptr;
    }
    return value->GetDictionary();
}

}  // namespace

bool ParseCommand(const std::string& payload, Command* out) {
    CefRefPtr<CefValue> root = CefParseJSON(CefString(payload), JSON_PARSER_RFC);
    if (!root || root->GetType() != VTYPE_DICTIONARY) {
        return false;
    }
    CefRefPtr<CefDictionaryValue> envelope = root->GetDictionary();

    const int version = static_cast<int>(GetNumber(envelope, "version"));
    if (version != kProtocolVersion) {
        std::fprintf(stderr, "[engine-host] unsupported protocol version %d (this host speaks %d)\n",
                     version, kProtocolVersion);
        return false;
    }

    out->id = static_cast<int64_t>(GetNumber(envelope, "id"));

    CefRefPtr<CefDictionaryValue> message = GetDictionary(envelope, "message");
    if (!message) {
        return false;
    }
    out->op = GetString(message, "op");
    if (out->op.empty()) {
        return false;
    }

    out->surface = static_cast<int64_t>(GetNumber(message, "surface"));
    out->url = GetString(message, "url");
    out->script = GetString(message, "script");
    out->protocol = static_cast<int>(GetNumber(message, "protocol"));
    // Data control: the origin and the storage identifiers travel at the top level (`clear_origin_data`).
    out->origin = GetString(message, "origin");
    // The page's request/reply channel (`bridge_reply`, protocol 2).
    out->bridge_request = static_cast<int64_t>(GetNumber(message, "request"));
    out->json = GetString(message, "json");
    out->error = GetString(message, "error");
    // Site theme emulation (`set_media`): auto | dark | light.
    const std::string media = GetString(message, "media");
    if (!media.empty()) {
        out->media = media;
    }
    // Dark pages (`set_forced_dark`): the mode (`off`, `invert`, `dim`, `warm`), or the old boolean — the shell
    // used `enabled` before the modes existed, and an old binary talking to a new host must keep working.
    const std::string dark_mode = GetString(message, "mode");
    if (!dark_mode.empty()) {
        out->dark_mode = dark_mode;
    } else if (CefRefPtr<CefValue> forced = message->GetValue("enabled")) {
        if (forced->GetType() == VTYPE_BOOL) {
            out->dark_mode = forced->GetBool() ? "invert" : "off";
        }
    }
    // Scrolling (`scroll`): CSS pixels, and whether the movement is animated.
    out->scroll_x = static_cast<int>(GetNumber(message, "x"));
    out->scroll_y = static_cast<int>(GetNumber(message, "y"));
    if (CefRefPtr<CefValue> animate = message->GetValue("animate")) {
        out->animate = animate->GetType() == VTYPE_BOOL && animate->GetBool();
    }
    // Zoom (`set_zoom`): 100 is the default. Sent as a number by the Rust contract.
    if (const int factor = static_cast<int>(GetNumber(message, "factor_percent")); factor > 0) {
        out->factor_percent = factor;
    }
    if (CefRefPtr<CefValue> storages = message->GetValue("storages")) {
        if (storages->GetType() == VTYPE_LIST) {
            CefRefPtr<CefListValue> list = storages->GetList();
            if (list) {
                for (size_t index = 0; index < list->GetSize(); ++index) {
                    const std::string storage = list->GetString(index).ToString();
                    if (!storage.empty()) {
                        out->storages.push_back(storage);
                    }
                }
            }
        }
    }

    out->format = GetString(message, "format");
    if (out->format.empty()) {
        out->format = "png";
    }
    if (CefRefPtr<CefValue> quality = message->GetValue("quality")) {
        double requested = -1.0;
        if (quality->GetType() == VTYPE_INT) {
            requested = static_cast<double>(quality->GetInt());
        } else if (quality->GetType() == VTYPE_DOUBLE) {
            requested = quality->GetDouble();
        }
        // A quality outside 0..100 is rejected by CDP *after* the call was submitted, which turns a
        // bad argument into a slow failure; clamping keeps the answer an answer.
        if (requested >= 0.0) {
            out->quality = requested > 100.0 ? 100 : static_cast<int>(requested);
        }
    }

    CefRefPtr<CefDictionaryValue> config = GetDictionary(message, "config");
    if (config) {
        out->partition = GetString(config, "partition");
        // A 64-bit native handle travels as a JSON number; JSON numbers are doubles, so the value is
        // exact only up to 2^53. Windows handles fit comfortably below that, and a handle above it
        // is rejected rather than silently truncated into someone else's window.
        const double parent_window = GetNumber(config, "parent_window");
        if (parent_window < 0 || parent_window > 9007199254740992.0) {
            std::fprintf(stderr, "[engine-host] parent_window out of range\n");
            return false;
        }
        out->parent_window = static_cast<uint64_t>(parent_window);
        CefRefPtr<CefDictionaryValue> viewport = GetDictionary(config, "viewport");
        if (viewport) {
            const double width = GetNumber(viewport, "width");
            const double height = GetNumber(viewport, "height");
            if (width > 0) {
                out->width = static_cast<int>(width);
            }
            if (height > 0) {
                out->height = static_cast<int>(height);
            }
        }
        // Optional, additive field (ADR-041): an omitted `hidden` means "show it", so every older caller
        // keeps its behaviour without a protocol bump — the same rule `parent_window` follows.
        if (CefRefPtr<CefValue> hidden = config->GetValue("hidden")) {
            if (hidden->GetType() == VTYPE_BOOL) {
                out->hidden = hidden->GetBool();
            }
        }
    }

    // `set_bounds` replaces the placement of an embedded surface. The rectangle is physical pixels and
    // may start at a negative origin when the card is scrolled partly out of view.
    CefRefPtr<CefDictionaryValue> bounds = GetDictionary(message, "bounds");
    if (bounds) {
        out->bounds_x = static_cast<int>(GetNumber(bounds, "x"));
        out->bounds_y = static_cast<int>(GetNumber(bounds, "y"));
        out->bounds_width = static_cast<int>(GetNumber(bounds, "width"));
        out->bounds_height = static_cast<int>(GetNumber(bounds, "height"));
    }

    // Only `focus` sends this; `focused` defaults to true so an omitted key means "give it focus".
    if (CefRefPtr<CefValue> focused = message->GetValue("focused")) {
        if (focused->GetType() == VTYPE_BOOL) {
            out->focused = focused->GetBool();
        }
    }
    return true;
}

StdioLink& StdioLink::Instance() {
    static StdioLink instance;
    return instance;
}

void StdioLink::Start() {
    if (started_) {
        return;
    }
    started_ = true;
#ifdef _WIN32
    // The frame channel must be byte-exact. The 4-byte length header can legitimately contain a 0x0A byte
    // (the large `cards.upsert` event is 4362 = 0x110A bytes, low byte 0x0A), and the CRT's text-mode
    // stdout would translate that lone LF into CRLF — a 4-byte header becomes 5 bytes and every frame after
    // it is misaligned (measured on VECTOR: the shell then decoded a frame length of 1116685 instead of
    // 4362). Reading already goes through `_fdopen(..., "rb")`; writing must be binary too.
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::thread([this] { ReaderLoop(); }).detach();
}

void StdioLink::ReaderLoop() {
#ifdef _WIN32
    // Chromium closes the browser process's inherited stdin handle once it starts launching its child
    // processes (renderer/GPU). That surfaces as a premature EOF on the shell's pipe while the shell is
    // still writing: `fread(stdin)` returns 0 and the host shuts down mid-conversation (measured on
    // VECTOR — `stdin closed` right after the first page eval, while the shell kept writing frames).
    // Read from a private duplicate instead, so a CloseHandle inside Chromium cannot tear the link down;
    // the pipe itself stays alive as long as this duplicate and the shell's write end both exist.
    FILE* input = nullptr;
    const HANDLE inherited = GetStdHandle(STD_INPUT_HANDLE);
    if (inherited != nullptr && inherited != INVALID_HANDLE_VALUE) {
        HANDLE owned = INVALID_HANDLE_VALUE;
        if (DuplicateHandle(GetCurrentProcess(), inherited, GetCurrentProcess(), &owned, 0, FALSE,
                            DUPLICATE_SAME_ACCESS) &&
            owned != INVALID_HANDLE_VALUE) {
            const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(owned), _O_RDONLY | _O_BINARY);
            if (fd != -1) {
                input = _fdopen(fd, "rb");
                if (input == nullptr) {
                    _close(fd);
                }
            } else {
                CloseHandle(owned);
            }
        }
    }
    if (input == nullptr) {
        input = stdin;  // fallback: the standard stream, exactly as before
    }
#else
    FILE* input = stdin;
#endif

    for (;;) {
        unsigned char header[4] = {};
        if (std::fread(header, 1, sizeof(header), input) != sizeof(header)) {
            break;
        }
        const uint32_t length = DecodeLength(header);
        if (length == 0 || length > kMaxFrameBytes) {
            std::fprintf(stderr, "[engine-host] protocol violation: frame length %u\n", length);
            break;
        }
        std::string payload(length, '\0');
        if (std::fread(payload.data(), 1, length, input) != length) {
            break;
        }

        Command command;
        if (!ParseCommand(payload, &command)) {
            std::fprintf(stderr, "[engine-host] dropped a malformed frame (%u bytes)\n", length);
            continue;
        }
        Push(command);
    }

    active_ = false;
    std::fprintf(stderr, "[engine-host] stdin closed\n");
}

void StdioLink::Push(const Command& command) {
    std::lock_guard<std::mutex> guard(queue_mutex_);
    queue_.push_back(command);
}

bool StdioLink::TryPop(Command* out) {
    std::lock_guard<std::mutex> guard(queue_mutex_);
    if (queue_.empty()) {
        return false;
    }
    *out = queue_.front();
    queue_.pop_front();
    return true;
}

void StdioLink::WriteEnvelope(CefRefPtr<CefDictionaryValue> message) {
    CefRefPtr<CefDictionaryValue> envelope = CefDictionaryValue::Create();
    envelope->SetInt("version", kProtocolVersion);
    envelope->SetInt("id", static_cast<int>(g_next_envelope_id.fetch_add(1)));
    envelope->SetDictionary("message", message);

    CefRefPtr<CefValue> value = CefValue::Create();
    value->SetDictionary(envelope);
    const std::string json = CefWriteJSON(value, kWriterOptions).ToString();

    if (json.size() > kMaxFrameBytes) {
        std::fprintf(stderr, "[engine-host] refusing to write a %zu byte frame\n", json.size());
        return;
    }
    char header[4];
    EncodeLength(static_cast<uint32_t>(json.size()), header);

    std::lock_guard<std::mutex> guard(write_mutex_);
    std::fwrite(header, 1, sizeof(header), stdout);
    std::fwrite(json.data(), 1, json.size(), stdout);
    std::fflush(stdout);
    // One line per frame on stderr: the frame channel stays clean, and a stuck exchange is
    // diagnosable from the log alone.
    std::fprintf(stderr, "[engine-host] -> %zu byte frame\n", json.size());
    std::fflush(stderr);
}

void StdioLink::Reply(int64_t request_id, CefRefPtr<CefDictionaryValue> result) {
    CefRefPtr<CefDictionaryValue> outcome = CefDictionaryValue::Create();
    outcome->SetString("kind", "ok");
    if (result) {
        outcome->SetDictionary("json", result);
    }
    CefRefPtr<CefDictionaryValue> message = CefDictionaryValue::Create();
    message->SetString("op", "reply");
    message->SetInt("to", static_cast<int>(request_id));
    message->SetDictionary("outcome", outcome);
    WriteEnvelope(message);
}

void StdioLink::ReplyError(int64_t request_id, const char* code, const char* message_text) {
    CefRefPtr<CefDictionaryValue> outcome = CefDictionaryValue::Create();
    outcome->SetString("kind", "err");
    outcome->SetString("code", code);
    outcome->SetString("message", message_text);
    outcome->SetBool("retryable", false);
    CefRefPtr<CefDictionaryValue> message = CefDictionaryValue::Create();
    message->SetString("op", "reply");
    message->SetInt("to", static_cast<int>(request_id));
    message->SetDictionary("outcome", outcome);
    WriteEnvelope(message);
}

void StdioLink::SendMessage(CefRefPtr<CefDictionaryValue> message) {
    WriteEnvelope(message);
}

void StdioLink::Event(const char* type, CefRefPtr<CefDictionaryValue> payload) {
    CefRefPtr<CefDictionaryValue> event = CefDictionaryValue::Create();
    event->SetString("type", type);
    if (payload) {
        CefDictionaryValue::KeyList keys;
        payload->GetKeys(keys);
        for (const CefString& key : keys) {
            event->SetValue(key, payload->GetValue(key));
        }
    }
    CefRefPtr<CefDictionaryValue> message = CefDictionaryValue::Create();
    message->SetString("op", "event");
    message->SetDictionary("event", event);
    WriteEnvelope(message);
}

}  // namespace antikolt
