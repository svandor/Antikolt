// SPDX-License-Identifier: MPL-2.0
//
// One DevTools method call, and the answer that comes back for it.
//
// CEF's DevTools transport is **asymmetric**: `CefBrowserHost::ExecuteDevToolsMethod` only submits a
// request, and the answer arrives later on `CefDevToolsMessageObserver::OnDevToolsMethodResult` with
// no way to tell which caller it belongs to beyond the message id. Every DevTools feature in this
// host needs the same four things around that asymmetry — a unique id, a param dictionary, a parse
// of the JSON envelope, and a handler — so they all go through this one type instead of each
// keeping a near-copy of the observer.
//
// The handlers are where the *sequences* live: `Page.enable` → `Page.captureSnapshot` →
// `Runtime.evaluate` is three calls, and each handler starts the next one. Keeping the sequence in a
// handler (rather than in a state enum here) keeps this file free of any knowledge of MHTML,
// screenshots or WebAuthn.
//
// Rules that keep this safe:
//   * the observer is registered for the *whole* browser, so it sees every result: a call must
//     ignore ids it did not issue, otherwise two parallel calls would steal each other's answers;
//   * the registration is released only after the handler returned, so removing the observer can
//     never free the object while it is inside its own callback;
//   * nothing here touches the wire protocol or stdout: replies are the caller's business.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "include/cef_browser.h"
#include "include/cef_devtools_message_observer.h"
#include "include/cef_registration.h"
#include "include/cef_values.h"

namespace antikolt {

/// A DevTools answer, parsed exactly once.
struct DevToolsReply {
    /// The transport delivered a result and it was usable.
    bool success = false;
    /// Why the call failed, taken from the envelope's `error.message` when there is one. Never empty
    /// when `success` is false.
    std::string error;
    /// True when `error` came from the DevTools *protocol* (the method rejected the arguments)
    /// rather than from the transport or from a payload this host could not parse. Callers map this
    /// to `invalid_argument` instead of `internal`, so a bad script is not reported as a broken engine.
    bool from_protocol = false;
    /// The envelope's `result` object, or the root object when no wrapper was present.
    CefRefPtr<CefDictionaryValue> result;
    /// The parsed JSON document the dictionaries above point into.
    ///
    /// **Must stay alive as long as `result` is used.** CEF's inner dictionaries live inside the
    /// parsed value: once that value is released, reading the dictionary comes back *empty* rather
    /// than failing — which is a silent, convincing-looking bug. It cost a debugging round when the
    /// observer classes read the parsed value directly, and a second one when this type took the
    /// document's lifetime away from its callers.
    CefRefPtr<CefValue> document;
    /// The response JSON exactly as received: worth logging, and some callers need the bytes.
    std::string raw;

    bool ok() const { return success && result; }
    /// A string field of `result` ("" when absent).
    std::string String(const char* key) const;
    /// An integer field of `result` (`fallback` when absent or of another type).
    int Int(const char* key, int fallback = 0) const;
    /// A dictionary field of `result` (null when absent or not a dictionary).
    CefRefPtr<CefDictionaryValue> Dictionary(const char* key) const;
};

/// Decode standard base64. CDP returns binary payloads (`Page.captureSnapshot`,
/// `Page.captureScreenshot`) base64-encoded over the JSON transport; the caller decides whether the
/// payload is still base64 by looking at its first bytes.
std::string Base64Decode(const std::string& input);

/// One DevTools method call: a method name, its parameters, and the handler for its answer.
class DevToolsCall : public CefDevToolsMessageObserver {
public:
    using Handler = std::function<void(CefRefPtr<CefBrowser> browser, const DevToolsReply& reply)>;

    /// `method` is a CDP name such as `Page.captureSnapshot`; `params` may be null (empty object).
    DevToolsCall(std::string method, CefRefPtr<CefDictionaryValue> params, Handler handler);
    ~DevToolsCall() override;

    /// Attach the observer and submit the call. Returns false when DevTools is not usable at all, so
    /// the caller can answer `not_supported` (or fall back) instead of waiting for an answer that
    /// will never arrive. Must be called on the CEF UI thread.
    bool Start(CefRefPtr<CefBrowser> browser);

    void OnDevToolsMethodResult(CefRefPtr<CefBrowser> browser,
                               int message_id,
                               bool success,
                               const void* result,
                               size_t result_size) override;

    /// The CDP method name, for logs.
    const std::string& method() const { return method_; }

private:
    /// Forget the registration. Called once the answer for this call has been handled.
    void Detach();

    std::string method_;
    CefRefPtr<CefDictionaryValue> params_;
    Handler handler_;
    int message_id_ = 0;
    CefRefPtr<CefRegistration> registration_;

    IMPLEMENT_REFCOUNTING(DevToolsCall);
    DISALLOW_COPY_AND_ASSIGN(DevToolsCall);
};

}  // namespace antikolt
