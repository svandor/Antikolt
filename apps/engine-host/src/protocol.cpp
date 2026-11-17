// SPDX-License-Identifier: MPL-2.0

#include "protocol.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

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
    if (version != 1) {
        std::fprintf(stderr, "[engine-host] unsupported protocol version %d\n", version);
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

    CefRefPtr<CefDictionaryValue> config = GetDictionary(message, "config");
    if (config) {
        out->partition = GetString(config, "partition");
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
    std::thread([this] { ReaderLoop(); }).detach();
}

void StdioLink::ReaderLoop() {
    for (;;) {
        unsigned char header[4] = {};
        if (std::fread(header, 1, sizeof(header), stdin) != sizeof(header)) {
            break;
        }
        const uint32_t length = DecodeLength(header);
        if (length == 0 || length > kMaxFrameBytes) {
            std::fprintf(stderr, "[engine-host] protocol violation: frame length %u\n", length);
            break;
        }
        std::string payload(length, '\0');
        if (std::fread(payload.data(), 1, length, stdin) != length) {
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
    envelope->SetInt("version", 1);
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
