// SPDX-License-Identifier: MPL-2.0

#include "devtools.h"

#include <cstdio>
#include <utility>

#include "include/cef_parser.h"

namespace antikolt {

namespace {

/// Monotonic id for DevTools method calls.
///
/// The id must be **unique per request**: reusing one silently drops the second reply, which looks
/// exactly like a hung engine. Ids come from one process-wide counter, so two calls that overlap in
/// time (a snapshot while a screenshot is running) can never mistake each other's answer.
int NextDevToolsId() {
    static int next = 100;
    return next++;
}

/// Turn one transport answer into a `DevToolsReply`: parse the envelope once, here, so no caller has
/// to repeat the "is it a dictionary, is there an error, is there a result wrapper" dance.
DevToolsReply ParseReply(bool success, const void* payload, size_t payload_size) {
    DevToolsReply reply;
    reply.success = success;
    if (payload == nullptr || payload_size == 0) {
        if (!success) {
            reply.error = "DevTools call failed";
        }
        return reply;
    }
    reply.raw.assign(static_cast<const char*>(payload), payload_size);

    CefRefPtr<CefValue> parsed = CefParseJSON(CefString(reply.raw), JSON_PARSER_RFC);
    if (!parsed || parsed->GetType() != VTYPE_DICTIONARY) {
        reply.success = false;
        reply.error = "unparseable DevTools result";
        return reply;
    }
    CefRefPtr<CefDictionaryValue> root = parsed->GetDictionary();

    if (CefRefPtr<CefValue> error = root->GetValue("error")) {
        if (error->GetType() == VTYPE_DICTIONARY) {
            const std::string message = error->GetDictionary()->GetString("message").ToString();
            reply.success = false;
            reply.from_protocol = true;
            reply.error = message.empty() ? "DevTools rejected the call" : message;
            return reply;
        }
    }

    CefRefPtr<CefDictionaryValue> result = root;
    if (CefRefPtr<CefValue> wrapper = root->GetValue("result")) {
        if (wrapper->GetType() == VTYPE_DICTIONARY) {
            result = wrapper->GetDictionary();
        }
    }
    reply.document = parsed;
    reply.result = result;
    reply.success = success;
    if (!success && reply.error.empty()) {
        reply.error = "DevTools call failed";
    }
    return reply;
}

}  // namespace

std::string DevToolsReply::String(const char* key) const {
    if (!result) {
        return {};
    }
    return result->GetString(key).ToString();
}

int DevToolsReply::Int(const char* key, int fallback) const {
    if (!result) {
        return fallback;
    }
    CefRefPtr<CefValue> value = result->GetValue(key);
    if (!value) {
        return fallback;
    }
    switch (value->GetType()) {
        case VTYPE_INT:
            return value->GetInt();
        case VTYPE_DOUBLE:
            return static_cast<int>(value->GetDouble());
        default:
            return fallback;
    }
}

CefRefPtr<CefDictionaryValue> DevToolsReply::Dictionary(const char* key) const {
    if (!result) {
        return nullptr;
    }
    CefRefPtr<CefValue> value = result->GetValue(key);
    if (!value || value->GetType() != VTYPE_DICTIONARY) {
        return nullptr;
    }
    return value->GetDictionary();
}

std::string Base64Decode(const std::string& input) {
    auto value_of = [](unsigned char character) -> int {
        if (character >= 'A' && character <= 'Z') {
            return character - 'A';
        }
        if (character >= 'a' && character <= 'z') {
            return character - 'a' + 26;
        }
        if (character >= '0' && character <= '9') {
            return character - '0' + 52;
        }
        if (character == '+') {
            return 62;
        }
        if (character == '/') {
            return 63;
        }
        return -1;
    };

    std::string out;
    out.reserve(input.size() * 3 / 4);
    int accumulator = 0;
    int bits = -8;
    for (unsigned char character : input) {
        const int value = value_of(character);
        if (value < 0) {
            continue;  // padding and whitespace
        }
        accumulator = (accumulator << 6) | value;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((accumulator >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

DevToolsCall::DevToolsCall(std::string method,
                           CefRefPtr<CefDictionaryValue> params,
                           Handler handler)
    : method_(std::move(method)), params_(std::move(params)), handler_(std::move(handler)) {}

DevToolsCall::~DevToolsCall() = default;

bool DevToolsCall::Start(CefRefPtr<CefBrowser> browser) {
    if (!browser || !browser->GetHost()) {
        return false;
    }
    registration_ = browser->GetHost()->AddDevToolsMessageObserver(this);
    if (!registration_) {
        return false;
    }
    message_id_ = NextDevToolsId();
    if (browser->GetHost()->ExecuteDevToolsMethod(message_id_, method_, params_) == 0) {
        Detach();
        return false;
    }
    return true;
}

void DevToolsCall::OnDevToolsMethodResult(CefRefPtr<CefBrowser> browser,
                                          int message_id,
                                          bool success,
                                          const void* result,
                                          size_t result_size) {
    // The observer sees *every* result on this browser, including the ones other calls issued.
    // Ignoring foreign ids is what makes parallel calls possible at all.
    if (message_id != message_id_) {
        return;
    }

    // Keep the object and its registration alive across the handler: the handler may start the next
    // call of a sequence, and this one is finished the moment the handler returns.
    CefRefPtr<DevToolsCall> self(this);
    CefRefPtr<CefRegistration> registration = registration_;
    registration_ = nullptr;

    const DevToolsReply reply = ParseReply(success, result, result_size);
    fprintf(stderr, "[engine-host] devtools %s id=%d success=%d bytes=%zu\n", method_.c_str(),
            message_id, reply.success ? 1 : 0, reply.raw.size());
    fflush(stderr);

    if (handler_) {
        handler_(browser, reply);
    }
}

void DevToolsCall::Detach() {
    registration_ = nullptr;
}

}  // namespace antikolt
