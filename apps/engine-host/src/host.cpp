// SPDX-License-Identifier: MPL-2.0

#include "host.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#include "include/cef_command_line.h"
#include "include/cef_devtools_message_observer.h"
#include "include/cef_parser.h"
#include "include/cef_registration.h"
#include "include/cef_string_visitor.h"
#include "include/cef_version.h"
#include "include/wrapper/cef_helpers.h"
#include "protocol.h"

namespace antikolt {

namespace {

/// A page that never finishes loading must not hang the harness.
constexpr int kLoadTimeoutSeconds = 30;

constexpr wchar_t kWindowClass[] = L"AntikoltEngineHostWindow";

/// Never show a crash dialog: a failed run must exit with a code that CI can read.
constexpr int kPumpIntervalMs = 2;

/// The single host instance of this process. CEF is a process-wide singleton, so one host per
/// process is the honest model; the window procedure needs it to translate WM_CLOSE into a request
/// that the pump loop can act on.
HostState* g_state = nullptr;

/// True when the host runs as the shell's protocol server (stdout carries frames only).
bool g_server_mode = false;

int64_t g_next_surface_id = 1;

void Log(const char* format, ...) {
    // Diagnostics never go to stdout in server mode: stdout is the frame channel.
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fflush(stderr);
}

#ifdef _WIN32
LRESULT CALLBACK HostWndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CLOSE:
            // Do not shut down from inside the window procedure: record the intent and let the pump
            // loop close the browser first (CefShutdown with a live browser crashes).
            if (g_state != nullptr) {
                g_state->quit_requested = true;
            }
            return 0;
        case WM_SIZE: {
            // Keep the CEF child window filling the client area (CEF creates it as a child of ours).
            RECT client = {};
            GetClientRect(window, &client);
            HWND child = FindWindowExW(window, nullptr, L"Chrome_WidgetWin_0", nullptr);
            if (child != nullptr) {
                MoveWindow(child, 0, 0, client.right - client.left, client.bottom - client.top, TRUE);
            }
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
#endif

/// Initialise CEF once per process. Returns false on failure.
bool InitializeCef(const HostConfig& config, CefRefPtr<EngineApp> app) {
#ifdef _WIN32
    CefMainArgs main_args(GetModuleHandleW(nullptr));
#else
    CefMainArgs main_args(0, nullptr);
#endif
    CefSettings settings;
    CefString(&settings.browser_subprocess_path).FromString(config.exe_path);
    // CEF 158 has no `user_data_path`: the per-profile cache directory is `cache_path`, and
    // `root_cache_path` is the fallback root for request contexts that do not set their own.
    CefString(&settings.cache_path).FromString(config.user_data_path);
    CefString(&settings.root_cache_path).FromString(config.user_data_path);
    CefString(&settings.log_file).FromString(config.user_data_path + "\\cef.log");
    settings.log_severity = LOGSEVERITY_WARNING;
    // We drive CEF from our own pump instead of letting it own a blocking loop. This is the same
    // integration shape the Qt shell needs, and it avoids the lost-quit race that CefQuitMessageLoop
    // produced when a page finished loading before the loop started.
    settings.external_message_pump = true;
    settings.multi_threaded_message_loop = false;
    settings.windowless_rendering_enabled = false;

    const bool initialised = CefInitialize(main_args, settings, app.get(), nullptr);
#ifdef _WIN32
    if (initialised) {
        // Embedding lives or dies on units: log what this process believes about DPI so a size
        // mismatch in the shell is diagnosable from a log line instead of by guesswork.
        const int awareness = static_cast<int>(
            GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()));
        Log("[engine-host] dpi_awareness=%d system_dpi=%u desktop_dpi=%u\n", awareness,
            GetDpiForSystem(), GetDpiForWindow(GetDesktopWindow()));
    }
#endif
    return initialised;
}

/// Create one native window with one browser surface. Returns false on failure.
///
/// With `config.parent_window` set the surface is embedded into a window owned by another process
/// (the shell) instead of getting a top-level window of its own: `SetAsChild` makes Chromium create
/// its `Chrome_WidgetWin_0` as a child of that handle, in *that* process's window tree. Input and
/// painting then flow through the OS, so pixels never cross our protocol.
bool CreateSurface(const HostConfig& config, HostState* state, const std::string& url) {
    HWND window = nullptr;
    if (config.parent_window != nullptr) {
        window = static_cast<HWND>(config.parent_window);
        if (IsWindow(window) == FALSE) {
            Log("[engine-host] parent window handle is not a window\n");
            return false;
        }
        state->window_owned = false;
    } else {
        window = CreateHostWindow(config);
        if (window == nullptr) {
            return false;
        }
        state->window_owned = true;
    }
    state->window = window;
    g_state = state;

    int origin_x = 0;
    int origin_y = 0;
    int width = 0;
    int height = 0;
    if (state->window_owned) {
        RECT client = {};
        GetClientRect(window, &client);
        width = client.right - client.left;
        height = client.bottom - client.top;
    } else {
        origin_x = config.origin_x;
        origin_y = config.origin_y;
        width = config.width;
        height = config.height;
    }

    CefWindowInfo window_info;
    window_info.SetAsChild(window, CefRect(origin_x, origin_y, width, height));

    CefRefPtr<EngineClient> client_handler(new EngineClient(state, config.quit_on_first_load));
    CefBrowserSettings browser_settings;
    if (!CefBrowserHost::CreateBrowser(window_info, client_handler, url, browser_settings, nullptr,
                                       nullptr)) {
        Log("[engine-host] CreateBrowser failed\n");
        return false;
    }

    if (state->window_owned && !config.hidden) {
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);
    }
    return true;
}

#ifdef _WIN32
/// The Chromium window that `SetAsChild` created inside our parent handle.
///
/// The class name is `Chrome_WidgetWin_1` (the `_0` variant is for top-level windows), and it must be
/// searched for by prefix: assuming `_0` made the lookup fail silently and left the embedded surface
/// parked at a negative coordinate, invisible, while every size still looked right.
HWND FindSurfaceChild(HWND parent) {
    struct Search {
        HWND found = nullptr;
    } search;
    EnumChildWindows(
        parent,
        [](HWND child, LPARAM param) -> BOOL {
            wchar_t class_name[64] = {};
            if (GetClassNameW(child, class_name, 64) > 0 &&
                wcsncmp(class_name, L"Chrome_WidgetWin_", 17) == 0) {
                reinterpret_cast<Search*>(param)->found = child;
                return FALSE;  // the first one is the browser's own window
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

/// Move and resize an embedded surface.
///
/// Units matter here and they are not the obvious ones: the protocol carries **device independent
/// pixels** (what the page calls CSS pixels), because that is what CEF and Chromium lay out in — a
/// rect of 900×640 on a 175 % monitor becomes a 1575×1120 window. Passing physical pixels instead
/// makes Chromium scale them a second time: the page then sees a 1575 px viewport and a window taller
/// than the screen, which the window manager clamps. That was a real bug, measured, not a guess.
///
/// `SetWindowPos` with `SWP_NOZORDER` keeps the surface below any Qt popup the shell draws over it.
bool ResizeEmbeddedSurface(HostState* state, int x, int y, int width, int height) {
    if (state->window == nullptr) {
        return false;
    }
    HWND parent = static_cast<HWND>(state->window);
    HWND child = FindSurfaceChild(parent);
    if (child == nullptr) {
        Log("[engine-host] set_bounds: no Chromium child under the parent window yet\n");
        return false;
    }
    const UINT dpi = GetDpiForWindow(parent);
    const double scale = (dpi > 0 ? static_cast<double>(dpi) : 96.0) / 96.0;
    const int physical_x = static_cast<int>(std::lround(x * scale));
    const int physical_y = static_cast<int>(std::lround(y * scale));
    const int physical_width = static_cast<int>(std::lround(width * scale));
    const int physical_height = static_cast<int>(std::lround(height * scale));
    if (SetWindowPos(child, nullptr, physical_x, physical_y, physical_width, physical_height,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER) == FALSE) {
        Log("[engine-host] set_bounds: SetWindowPos failed error=%lu\n", GetLastError());
        return false;
    }
    if (state->browser) {
        state->browser->GetHost()->NotifyScreenInfoChanged();
        state->browser->GetHost()->WasResized();
    }
    return true;
}

/// Apply the pending rectangle once the surface's child window exists. The pump loop calls this, so a
/// `set_bounds` that arrives before Chromium created its window is remembered instead of lost.
bool ApplyPendingEmbeddedBounds(HostState* state) {
    if (!state->bounds_pending) {
        return true;
    }
    if (state->window_owned || state->window == nullptr) {
        return false;
    }
    if (!ResizeEmbeddedSurface(state, state->bounds_x, state->bounds_y, state->bounds_width,
                               state->bounds_height)) {
        return false;
    }
    state->bounds_pending = false;
    return true;
}
#endif

/// Escape hatch: the surface may have been created for a stopped browser.
void DrainAndCloseBrowser(HostState* state) {
    for (int i = 0; i < 50; ++i) {
        CefDoMessageLoopWork();
        std::this_thread::sleep_for(std::chrono::milliseconds(kPumpIntervalMs));
    }
    if (state->browser) {
        state->browser->GetHost()->CloseBrowser(true);
        for (int i = 0; i < 500 && state->browser; ++i) {
            CefDoMessageLoopWork();
            std::this_thread::sleep_for(std::chrono::milliseconds(kPumpIntervalMs));
        }
    }
}

#ifdef _WIN32
void PumpWindowsMessages() {
    MSG message = {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

/// Destroy the native window that hosted a surface — but only if this process created it.
///
/// An embedded surface lives in the shell's window; destroying that from here would kill the UI. We
/// only forget the handle and let the shell's own widget teardown do the real work.
void DestroyHostWindow(HostState* state) {
    if (state->window != nullptr) {
        if (state->window_owned) {
            DestroyWindow(static_cast<HWND>(state->window));
        }
        state->window = nullptr;
        state->window_owned = false;
    }
}
#else
void PumpWindowsMessages() {}
void DestroyHostWindow(HostState* state) { (void)state; }
#endif

CefRefPtr<CefDictionaryValue> MakeEngineInfo() {
    CefRefPtr<CefDictionaryValue> engine = CefDictionaryValue::Create();
    engine->SetString("name", "cef");
    engine->SetString("version", CEF_VERSION);
    engine->SetInt("chromium_major", CHROME_VERSION_MAJOR);
    CefRefPtr<CefListValue> capabilities = CefListValue::Create();
    capabilities->SetString(0, "capture");
    capabilities->SetString(1, "media_emulation");
    capabilities->SetString(2, "isolated_world");
    capabilities->SetString(3, "offscreen_surface");
    // Claimed from this milestone on: the DOM snapshot that makes disk hibernation possible.
    capabilities->SetString(4, "snapshot");
    engine->SetList("capabilities", capabilities);
    return engine;
}

/// Writes a frame's serialised DOM to disk and replies to the pending `snapshot` request.
///
/// This is the first half of the freeze mechanism (docs/HU/02 §5): capture the page state, *then*
/// destroy the surface. `CefFrame::GetSource` is asynchronous and delivers the serialised DOM, which
/// is the piece a hibernated card must not lose. MHTML with inlined resources, scroll position and
/// storage come next; this proves the seam and the memory release.
class SnapshotVisitor : public CefStringVisitor {
public:
    SnapshotVisitor(int64_t request_id, std::string path, std::string url, std::string title)
        : request_id_(request_id),
          path_(std::move(path)),
          url_(std::move(url)),
          title_(std::move(title)) {}

    void Visit(const CefString& content) override {
        const std::string html = content.ToString();
        bool ok = false;
        {
            std::ofstream out(path_, std::ios::binary | std::ios::trunc);
            if (out) {
                out.write(html.data(), static_cast<std::streamsize>(html.size()));
                ok = out.good();
            }
        }
        if (!ok) {
            StdioLink::Instance().ReplyError(request_id_, "internal", "could not write the snapshot");
            return;
        }
        CefRefPtr<CefDictionaryValue> result = CefDictionaryValue::Create();
        result->SetBool("ok", true);
        result->SetString("path", path_);
        result->SetInt("bytes", static_cast<int>(html.size()));
        result->SetString("url", url_);
        result->SetString("title", title_);
        StdioLink::Instance().Reply(request_id_, result);
        Log("[engine-host] snapshot written: %s (%zu bytes)\n", path_.c_str(), html.size());
    }

private:
    int64_t request_id_;
    std::string path_;
    std::string url_;
    std::string title_;

    IMPLEMENT_REFCOUNTING(SnapshotVisitor);
    DISALLOW_COPY_AND_ASSIGN(SnapshotVisitor);
};

/// Monotonic id for snapshot files, so two freezes never overwrite each other.
int64_t s_snapshot_counter = 0;

/// Monotonic id for DevTools method calls.
///
/// The id must be **unique per request**: reusing one silently drops the second reply, which looks
/// exactly like a hung engine. Every capture object takes its own ids from here.
int NextDevToolsId() {
    static int next = 100;
    return next++;
}

/// Decode standard base64. CDP's `Page.captureSnapshot` returns `data` as a `binary` type, which is
/// base64 on the wire; older/other builds hand back the MHTML as-is, and the caller decides by
/// looking at the first bytes.
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

/// The page state a card must not lose, gathered in the page and returned as one JSON string.
///
/// Rules encoded here (docs/HU/02 §5.1):
///   * password fields are **never** captured — a snapshot is not a credential store;
///   * values are truncated, so one pathological textarea cannot blow up the snapshot;
///   * the anchor is captured because a reload of an infinite feed cannot find the position by number.
constexpr const char* kStateExpression = R"JS(
(function () {
  function selectorOf(el) {
    if (!el || !el.tagName) return '';
    var parts = [];
    while (el && el.nodeType === 1 && parts.length < 8) {
      var part = el.tagName.toLowerCase();
      var parent = el.parentElement;
      if (parent) {
        var index = 1;
        for (var sib = el.previousElementSibling; sib; sib = sib.previousElementSibling) {
          if (sib.tagName === el.tagName) index++;
        }
        part += ':nth-of-type(' + index + ')';
      }
      parts.unshift(part);
      el = parent;
    }
    return parts.join(' > ');
  }
  function anchorOf() {
    var nodes = document.querySelectorAll('body *');
    for (var i = 0; i < nodes.length; i++) {
      var r = nodes[i].getBoundingClientRect();
      if (r.top >= -2 && r.height > 4) { return { selector: selectorOf(nodes[i]), dy: Math.round(r.top) }; }
    }
    return null;
  }
  function sessionDump() {
    var out = {};
    try {
      for (var i = 0; i < sessionStorage.length; i++) {
        var k = sessionStorage.key(i);
        out[k] = String(sessionStorage.getItem(k)).slice(0, 2000);
      }
    } catch (e) { }
    return out;
  }
  var forms = [];
  var fields = document.querySelectorAll('input, textarea, select');
  for (var i = 0; i < fields.length; i++) {
    var el = fields[i];
    var type = (el.getAttribute('type') || el.tagName).toLowerCase();
    if (type === 'password' || type === 'hidden') continue;
    var value = null;
    try { value = String(el.value).slice(0, 2000); } catch (e) { }
    forms.push({ selector: selectorOf(el), type: type, value: value, checked: !!el.checked });
  }
  var media = [];
  var players = document.querySelectorAll('video, audio');
  for (var i = 0; i < players.length; i++) {
    media.push({ currentTime: players[i].currentTime || 0, paused: !!players[i].paused });
  }
  return JSON.stringify({
    url: location.href,
    title: document.title,
    scrollX: window.scrollX || 0,
    scrollY: window.scrollY || 0,
    anchor: anchorOf(),
    forms: forms.slice(0, 200),
    session: sessionDump(),
    media: media,
    viewport: { width: window.innerWidth, height: window.innerHeight, dpr: window.devicePixelRatio }
  });
})()
)JS";

/// Captures the page as MHTML plus its runtime state through the DevTools protocol, then replies to
/// the `snapshot` request.
///
/// This is the full hibernation artifact: the DOM with its resources inlined (readable offline) and a
/// small JSON file with everything the DOM does not carry (scroll, anchor, form values, session
/// storage, media position). The sequence is a small state machine because DevTools answers
/// asynchronously: enable -> capture -> evaluate -> write.
class MhtmlCapture : public CefDevToolsMessageObserver {
public:
    MhtmlCapture(int64_t request_id,
                 std::string base_path,
                 int64_t surface_id,
                 std::string url,
                 std::string title)
        : request_id_(request_id),
          base_path_(std::move(base_path)),
          surface_id_(surface_id),
          url_(std::move(url)),
          title_(std::move(title)),
          enable_id_(NextDevToolsId()),
          capture_id_(NextDevToolsId()),
          evaluate_id_(NextDevToolsId()) {}

    /// Attach the observer and start the sequence. Returns false when DevTools cannot be used at all,
    /// so the caller can fall back to the DOM-only capture.
    bool Start(CefRefPtr<CefBrowser> browser) {
        registration_ = browser->GetHost()->AddDevToolsMessageObserver(this);
        CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
        const int submitted =
            browser->GetHost()->ExecuteDevToolsMethod(enable_id_, "Page.enable", params);
        if (submitted == 0) {
            registration_ = nullptr;
            return false;
        }
        return true;
    }

    void OnDevToolsMethodResult(CefRefPtr<CefBrowser> browser,
                                int message_id,
                                bool success,
                                const void* result,
                                size_t result_size) override {
        const std::string raw =
            (result != nullptr && result_size > 0)
                ? std::string(static_cast<const char*>(result), result_size)
                : std::string();
        Log("[engine-host] devtools id=%d success=%d bytes=%zu\n", message_id, success ? 1 : 0,
            result_size);

        if (message_id == enable_id_) {
            SubmitCapture(browser);
        } else if (message_id == capture_id_) {
            OnCaptured(browser, success, raw);
        } else if (message_id == evaluate_id_) {
            OnEvaluated(success, raw);
        }
    }

private:
    int enable_id_;
    int capture_id_;
    int evaluate_id_;

    void SubmitCapture(CefRefPtr<CefBrowser> browser) {
        CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
        params->SetString("format", "mhtml");
        if (browser->GetHost()->ExecuteDevToolsMethod(capture_id_, "Page.captureSnapshot",
                                                      params) == 0) {
            Finish("could not submit Page.captureSnapshot");
        }
    }

    void OnCaptured(CefRefPtr<CefBrowser> browser, bool success, const std::string& raw) {
        if (!success || raw.empty()) {
            Finish("Page.captureSnapshot failed");
            return;
        }
        // The parsed CefValue owns its dictionaries: keep it alive while reading them, otherwise the
        // inner dictionary comes back empty (a real bug that cost a debugging round).
        CefRefPtr<CefValue> parsed = CefParseJSON(CefString(raw), JSON_PARSER_RFC);
        if (!parsed || parsed->GetType() != VTYPE_DICTIONARY) {
            Finish("unparseable DevTools result");
            return;
        }
        CefRefPtr<CefDictionaryValue> root = parsed->GetDictionary();
        if (CefRefPtr<CefValue> error = root->GetValue("error")) {
            if (error->GetType() == VTYPE_DICTIONARY) {
                Finish(error->GetDictionary()->GetString("message").ToString());
                return;
            }
        }
        std::string data;
        if (CefRefPtr<CefValue> result_value = root->GetValue("result")) {
            if (result_value->GetType() == VTYPE_DICTIONARY) {
                data = result_value->GetDictionary()->GetString("data").ToString();
            }
        }
        if (data.empty()) {
            data = root->GetString("data").ToString();
        }
        if (data.empty()) {
            Finish("DevTools result has no data");
            return;
        }
        // CEF hands over the finished MHTML; other DevTools transports base64-encode it. "From:" at
        // the start is the reliable discriminator.
        mhtml_ = data.rfind("From:", 0) == 0 ? data : Base64Decode(data);
        if (!WriteFileBytes(base_path_ + ".mhtml", mhtml_)) {
            Finish("could not write the snapshot");
            return;
        }

        // Second half: the runtime state the MHTML cannot carry.
        CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
        params->SetString("expression", kStateExpression);
        params->SetBool("returnByValue", true);
        if (browser->GetHost()->ExecuteDevToolsMethod(evaluate_id_, "Runtime.evaluate",
                                                      params) == 0) {
            FinishOk();  // MHTML is on disk; a missing state file is degraded, not fatal.
        }
    }

    void OnEvaluated(bool success, const std::string& raw) {
        Log("[engine-host] state evaluate success=%d bytes=%zu\n", success ? 1 : 0, raw.size());
        if (success) {
            CefRefPtr<CefValue> parsed = CefParseJSON(CefString(raw), JSON_PARSER_RFC);
            if (parsed && parsed->GetType() == VTYPE_DICTIONARY) {
                CefRefPtr<CefDictionaryValue> root = parsed->GetDictionary();
                CefRefPtr<CefDictionaryValue> value_holder = root;
                if (CefRefPtr<CefValue> result_value = root->GetValue("result")) {
                    if (result_value->GetType() == VTYPE_DICTIONARY) {
                        value_holder = result_value->GetDictionary();
                    }
                }
                const std::string json = value_holder->GetString("value").ToString();
                const std::string state_path = base_path_ + ".state.json";
                if (!json.empty() && WriteFileBytes(state_path, json)) {
                    state_bytes_ = json.size();
                    Log("[engine-host] state: %zu bytes written\n", json.size());
                } else {
                    Log("[engine-host] state: no usable value\n");
                }
            }
        }
        FinishOk();
    }

    void FinishOk() {
        registration_ = nullptr;
        CefRefPtr<CefDictionaryValue> reply = CefDictionaryValue::Create();
        reply->SetBool("ok", true);
        reply->SetBool("mhtml", !mhtml_.empty());
        reply->SetString("path", base_path_ + (mhtml_.empty() ? ".html" : ".mhtml"));
        reply->SetString("state_path", base_path_ + ".state.json");
        reply->SetInt("bytes", static_cast<int>(mhtml_.size()));
        reply->SetInt("state_bytes", static_cast<int>(state_bytes_));
        reply->SetString("url", url_);
        reply->SetString("title", title_);
        reply->SetInt("surface", static_cast<int>(surface_id_));
        StdioLink::Instance().Reply(request_id_, reply);
        Log("[engine-host] snapshot written: %zu bytes MHTML, %zu bytes state\n", mhtml_.size(),
            state_bytes_);
    }

    void Finish(const std::string& error) {
        registration_ = nullptr;
        StdioLink::Instance().ReplyError(request_id_, "internal", error.c_str());
    }

    static bool WriteFileBytes(const std::string& path, const std::string& bytes) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return out.good();
    }

    int64_t request_id_;
    std::string base_path_;
    int64_t surface_id_;
    std::string url_;
    std::string title_;
    std::string mhtml_;
    size_t state_bytes_ = 0;
    CefRefPtr<CefRegistration> registration_;

    IMPLEMENT_REFCOUNTING(MhtmlCapture);
    DISALLOW_COPY_AND_ASSIGN(MhtmlCapture);
};

/// Evaluates JavaScript in the page through DevTools and replies with the resulting value.
///
/// The main world is used deliberately: this is the shell's own script (restore, measurement), not a
/// credential or a page-visible secret. Anything secret must go through an isolated world instead.
class EvalCapture : public CefDevToolsMessageObserver {
public:
    EvalCapture(int64_t request_id, std::string script)
        : request_id_(request_id), script_(std::move(script)), message_id_(NextDevToolsId()) {}

    bool Start(CefRefPtr<CefBrowser> browser) {
        registration_ = browser->GetHost()->AddDevToolsMessageObserver(this);
        CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
        params->SetString("expression", script_);
        params->SetBool("returnByValue", true);
        if (browser->GetHost()->ExecuteDevToolsMethod(message_id_, "Runtime.evaluate", params) ==
            0) {
            registration_ = nullptr;
            return false;
        }
        return true;
    }

    void OnDevToolsMethodResult(CefRefPtr<CefBrowser> browser,
                                int message_id,
                                bool success,
                                const void* result,
                                size_t result_size) override {
        if (message_id != message_id_) {
            return;
        }
        registration_ = nullptr;
        if (!success || result == nullptr || result_size == 0) {
            StdioLink::Instance().ReplyError(request_id_, "internal", "Runtime.evaluate failed");
            return;
        }
        const std::string raw(static_cast<const char*>(result), result_size);
        CefRefPtr<CefValue> parsed = CefParseJSON(CefString(raw), JSON_PARSER_RFC);
        if (!parsed || parsed->GetType() != VTYPE_DICTIONARY) {
            StdioLink::Instance().ReplyError(request_id_, "internal", "unparseable evaluate result");
            return;
        }
        CefRefPtr<CefDictionaryValue> root = parsed->GetDictionary();
        if (CefRefPtr<CefValue> error = root->GetValue("error")) {
            if (error->GetType() == VTYPE_DICTIONARY) {
                const std::string message =
                    error->GetDictionary()->GetString("message").ToString();
                StdioLink::Instance().ReplyError(request_id_, "invalid_argument", message.c_str());
                return;
            }
        }
        CefRefPtr<CefDictionaryValue> inner = root;
        if (CefRefPtr<CefValue> result_value = root->GetValue("result")) {
            if (result_value->GetType() == VTYPE_DICTIONARY) {
                inner = result_value->GetDictionary();
            }
        }
        CefRefPtr<CefDictionaryValue> reply = CefDictionaryValue::Create();
        reply->SetBool("ok", true);
        reply->SetString("type", inner->GetString("type").ToString());
        // `value` is always a string on the wire, whatever the script returned. Reading it with
        // GetString made every boolean and number come back empty — `document.hasFocus()` answered
        // nothing at all — so primitives are rendered to their JSON text here.
        CefRefPtr<CefValue> value = inner->GetValue("value");
        if (!value) {
            reply->SetString("value", "");
        } else {
            switch (value->GetType()) {
                case VTYPE_BOOL:
                    reply->SetString("value", value->GetBool() ? "true" : "false");
                    break;
                case VTYPE_INT:
                    reply->SetString("value", std::to_string(value->GetInt()));
                    break;
                case VTYPE_DOUBLE: {
                    std::ostringstream text;
                    text << value->GetDouble();
                    reply->SetString("value", text.str());
                    break;
                }
                case VTYPE_STRING:
                    reply->SetString("value", value->GetString().ToString());
                    break;
                default:
                    reply->SetString("value", "");
                    break;
            }
        }
        StdioLink::Instance().Reply(request_id_, reply);
    }

private:
    int64_t request_id_;
    int message_id_;
    std::string script_;
    CefRefPtr<CefRegistration> registration_;

    IMPLEMENT_REFCOUNTING(EvalCapture);
    DISALLOW_COPY_AND_ASSIGN(EvalCapture);
};

}  // namespace

void EngineApp::OnBeforeCommandLineProcessing(const CefString& process_type,
                                              CefRefPtr<CefCommandLine> command_line) {
    if (!process_type.empty()) {
        return;  // subprocesses inherit the switches from the browser process
    }

    // The CEF sandbox is not wired up yet; without this switch CEF refuses to start. Turning the
    // sandbox on is a release requirement (docs/HU/02-ARCHITEKTURA.md §14), tracked in the dev log.
    command_line->AppendSwitch("no-sandbox");

    // Keep the host boring and the measurement meaningful: nothing that could start background work
    // before the baseline is measured.
    command_line->AppendSwitch("disable-extensions");
    command_line->AppendSwitch("disable-background-networking");
    command_line->AppendSwitch("disable-component-update");
    command_line->AppendSwitch("disable-default-apps");
    command_line->AppendSwitch("no-first-run");
    command_line->AppendSwitch("disable-pdf-extension");

    // Some environments (virtualised GPUs, locked-down sessions, CI) cannot initialise Chromium's GPU
    // process: it aborts with a CHECK failure, CEF retries, and every retry costs a process. Setting
    // ANTIKOLT_ENGINE_DISABLE_GPU=1 makes the host fall back to software compositing on purpose, so a
    // measurement or a test can run deterministically. It is an environment switch, not a product
    // setting, and it must never be used for a release measurement (see docs/HU/18-F0-JELENTES.md).
    if (const char* disable_gpu = std::getenv("ANTIKOLT_ENGINE_DISABLE_GPU");
        disable_gpu != nullptr && *disable_gpu != '\0' && std::strcmp(disable_gpu, "0") != 0) {
        command_line->AppendSwitch("disable-gpu");
        command_line->AppendSwitch("disable-gpu-compositing");
    }
}

void EngineClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();
    state_->browser = browser;
    state_->browser_created = true;
    if (g_server_mode) {
        CefRefPtr<CefDictionaryValue> payload = CefDictionaryValue::Create();
        payload->SetInt("surface", static_cast<int>(state_->surface_id));
        StdioLink::Instance().Event("surface_created", payload);
    }
}

void EngineClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();
    state_->browser = nullptr;
    if (g_server_mode) {
        // The next command may create another surface, so "created" must go back to false.
        state_->browser_created = false;
        CefRefPtr<CefDictionaryValue> payload = CefDictionaryValue::Create();
        payload->SetInt("surface", static_cast<int>(state_->surface_id));
        StdioLink::Instance().Event("surface_closed", payload);
    }
    if (!quit_on_first_load_) {
        // One-shot interactive mode: closing the browser ends the process. In server mode the next
        // command may create another surface, so the loop keeps running.
        if (!g_server_mode) {
            state_->quit_requested = true;
        }
    }
}

void EngineClient::OnLoadEnd(CefRefPtr<CefBrowser> browser,
                             CefRefPtr<CefFrame> frame,
                             int httpStatusCode) {
    CEF_REQUIRE_UI_THREAD();
    if (!frame->IsMain()) {
        return;
    }
    state_->http_status = httpStatusCode;
    state_->final_url = frame->GetURL().ToString();
    state_->load_finished = true;

    if (g_server_mode) {
        CefRefPtr<CefDictionaryValue> payload = CefDictionaryValue::Create();
        payload->SetInt("surface", static_cast<int>(state_->surface_id));
        payload->SetString("url", state_->final_url);
        payload->SetInt("http_status", httpStatusCode);
        StdioLink::Instance().Event("load_finished", payload);
    } else {
        Log("[engine-host] load_end url=%s status=%d\n", state_->final_url.c_str(), httpStatusCode);
    }
}

void EngineClient::OnLoadError(CefRefPtr<CefBrowser> browser,
                               CefRefPtr<CefFrame> frame,
                               ErrorCode errorCode,
                               const CefString& errorText,
                               const CefString& failedUrl) {
    CEF_REQUIRE_UI_THREAD();
    if (!frame->IsMain()) {
        return;
    }
    state_->final_url = failedUrl.ToString();
    state_->load_failed = true;

    if (g_server_mode) {
        CefRefPtr<CefDictionaryValue> payload = CefDictionaryValue::Create();
        payload->SetInt("surface", static_cast<int>(state_->surface_id));
        payload->SetString("url", state_->final_url);
        payload->SetString("reason", errorText.ToString());
        StdioLink::Instance().Event("load_failed", payload);
    } else {
        Log("[engine-host] load_error code=%d text=%s url=%s\n", static_cast<int>(errorCode),
            errorText.ToString().c_str(), state_->final_url.c_str());
    }
}

void EngineClient::OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) {
    CEF_REQUIRE_UI_THREAD();
    state_->title = title.ToString();
    if (g_server_mode) {
        CefRefPtr<CefDictionaryValue> payload = CefDictionaryValue::Create();
        payload->SetInt("surface", static_cast<int>(state_->surface_id));
        payload->SetString("title", state_->title);
        StdioLink::Instance().Event("title_changed", payload);
    }
}

#ifdef _WIN32
HWND CreateHostWindow(const HostConfig& config) {
    HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = HostWndProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kWindowClass;
    if (RegisterClassExW(&window_class) == 0) {
        const DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS) {  // harmless: an earlier run in the same process
            Log("[engine-host] RegisterClassEx failed: %lu\n", error);
            return nullptr;
        }
    }

    // A window that is never shown still needs normal window styles: a WS_POPUP surface with no DWM
    // presence makes Chromium's GPU process fail to create a command buffer and it aborts.
    HWND window = CreateWindowExW(0, kWindowClass, L"Antikolt", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                  CW_USEDEFAULT, config.width, config.height, nullptr, nullptr,
                                  instance, nullptr);
    if (window == nullptr) {
        Log("[engine-host] CreateWindowEx failed: %lu\n", GetLastError());
    }
    return window;
}
#endif

int RunCefSubprocess() {
#ifdef _WIN32
    CefMainArgs main_args(GetModuleHandleW(nullptr));
#else
    CefMainArgs main_args(0, nullptr);
#endif
    CefRefPtr<EngineApp> app(new EngineApp());
    return CefExecuteProcess(main_args, app.get(), nullptr);
}

int RunEngineHost(const HostConfig& config) {
#ifdef _WIN32
    CefMainArgs main_args(GetModuleHandleW(nullptr));
#else
    CefMainArgs main_args(0, nullptr);
#endif
    CefRefPtr<EngineApp> app(new EngineApp());
    const int subprocess_code = CefExecuteProcess(main_args, app.get(), nullptr);
    if (subprocess_code >= 0) {
        return subprocess_code;
    }

    if (!InitializeCef(config, app)) {
        Log("[engine-host] CefInitialize failed\n");
        return 1;
    }

    HostState state;
    state.surface_id = g_next_surface_id++;
    if (!CreateSurface(config, &state, config.start_url)) {
        CefShutdown();
        return 1;
    }

    if (config.quit_on_first_load) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(kLoadTimeoutSeconds);
        while (!state.load_finished.load() && !state.load_failed.load() &&
               !state.quit_requested.load()) {
            PumpWindowsMessages();
            CefDoMessageLoopWork();
            std::this_thread::sleep_for(std::chrono::milliseconds(kPumpIntervalMs));
            if (std::chrono::steady_clock::now() > deadline) {
                state.timed_out = true;
                break;
            }
        }
    } else {
        while (!state.quit_requested.load()) {
            PumpWindowsMessages();
            CefDoMessageLoopWork();
            std::this_thread::sleep_for(std::chrono::milliseconds(kPumpIntervalMs));
            if (state.browser_created.load() && !state.browser) {
                break;  // the browser closed itself
            }
        }
    }

    DrainAndCloseBrowser(&state);

    const bool timed_out = state.timed_out.load();
    const bool failed = state.load_failed.load();
    const bool loaded = state.load_finished.load();
    // "ok" must mean a page really loaded: an interactive run closed before any load reports "none".
    Log("load=%s\n", timed_out ? "timeout" : (failed ? "error" : (loaded ? "ok" : "none")));
    Log("url=%s\n", state.final_url.c_str());
    Log("status=%d\n", state.http_status);
    Log("title=%s\n", state.title.c_str());

    CefShutdown();
    if (timed_out || failed) {
        return 1;
    }
    return (config.quit_on_first_load && !loaded) ? 1 : 0;
}

int RunEngineHostServer(const HostConfig& config) {
#ifdef _WIN32
    CefMainArgs main_args(GetModuleHandleW(nullptr));
#else
    CefMainArgs main_args(0, nullptr);
#endif
    CefRefPtr<EngineApp> app(new EngineApp());
    const int subprocess_code = CefExecuteProcess(main_args, app.get(), nullptr);
    if (subprocess_code >= 0) {
        return subprocess_code;
    }

    g_server_mode = true;
    if (!InitializeCef(config, app)) {
        Log("[engine-host] CefInitialize failed\n");
        return 1;
    }

    StdioLink& link = StdioLink::Instance();
    link.Start();
    Log("[engine-host] protocol server ready on stdio\n");
#ifdef _WIN32
    {
        // Embedding lives or dies on units. Log what this process believes about DPI so a size
        // mismatch in the shell is diagnosable from the log instead of by guesswork.
        DPI_AWARENESS_CONTEXT context = GetThreadDpiAwarenessContext();
        const int awareness = static_cast<int>(GetAwarenessFromDpiAwarenessContext(context));
        Log("[engine-host] dpi_awareness=%d system_dpi=%u\n", awareness, GetDpiForSystem());
    }
#endif

    HostState state;
    bool shutdown_requested = false;

    while (!shutdown_requested && link.active()) {
        PumpWindowsMessages();
        CefDoMessageLoopWork();
#ifdef _WIN32
        ApplyPendingEmbeddedBounds(&state);
#endif

        Command command;
        while (link.TryPop(&command)) {
            Log("[engine-host] op=%s id=%lld surface=%lld\n", command.op.c_str(),
                static_cast<long long>(command.id), static_cast<long long>(command.surface));
            if (command.op == "hello") {
                // The handshake answer is a `welcome` message, not a reply (proto/README.md).
                CefRefPtr<CefDictionaryValue> welcome = CefDictionaryValue::Create();
                welcome->SetString("op", "welcome");
                welcome->SetInt("protocol", 1);
                welcome->SetDictionary("engine", MakeEngineInfo());
                link.SendMessage(welcome);
            } else if (command.op == "ping") {
                link.Reply(command.id);
            } else if (command.op == "create_surface") {
                if (state.browser || state.browser_created) {
                    link.ReplyError(command.id, "engine_busy", "only one surface at a time");
                    continue;
                }
                HostConfig surface_config = config;
                if (command.width > 0) {
                    surface_config.width = command.width;
                }
                if (command.height > 0) {
                    surface_config.height = command.height;
                }
                surface_config.hidden = config.hidden;
#ifdef _WIN32
                // Embedding is a window-handle feature: without a parent the host owns the window.
                surface_config.parent_window =
                    command.parent_window != 0
                        ? reinterpret_cast<void*>(static_cast<uintptr_t>(command.parent_window))
                        : nullptr;
#endif
                state.surface_id = g_next_surface_id++;
                // A fresh surface starts at the origin of its parent, filling the rectangle the shell
                // asked for; Chromium creates its window from these units (device independent px).
                state.bounds_x = 0;
                state.bounds_y = 0;
                state.bounds_width = surface_config.width;
                state.bounds_height = surface_config.height;
                state.bounds_pending = surface_config.parent_window != nullptr;
                if (!CreateSurface(surface_config, &state, command.url.empty() ? "about:blank"
                                                                              : command.url)) {
                    link.ReplyError(command.id, "internal", "could not create the surface");
                    continue;
                }
                CefRefPtr<CefDictionaryValue> result = CefDictionaryValue::Create();
                result->SetInt("surface", static_cast<int>(state.surface_id));
                link.Reply(command.id, result);
            } else if (command.op == "navigate") {
                if (!state.browser) {
                    link.ReplyError(command.id, "surface_not_found", "no live surface");
                    continue;
                }
                state.browser->GetMainFrame()->LoadURL(command.url);
                link.Reply(command.id);
            } else if (command.op == "snapshot") {
                if (!state.browser || !state.browser->GetMainFrame()) {
                    link.ReplyError(command.id, "surface_not_found", "no live surface");
                    continue;
                }
                // `<user_data>/snapshots/surface-N-M.mhtml`: a real file the shell can verify, and
                // later move into the content-addressed blob store.
                std::error_code dir_error;
                std::filesystem::create_directories(config.user_data_path + "\\snapshots",
                                                    dir_error);
                const std::string base = config.user_data_path + "\\snapshots\\surface-" +
                                         std::to_string(state.surface_id) + "-" +
                                         std::to_string(s_snapshot_counter++);
                // Preferred: full MHTML plus the runtime state, through the DevTools protocol.
                CefRefPtr<MhtmlCapture> capture(new MhtmlCapture(
                    command.id, base, state.surface_id, state.final_url, state.title));
                if (!capture->Start(state.browser)) {
                    // Fallback: DOM only. Less useful offline, but never a missing snapshot.
                    Log("[engine-host] DevTools unavailable, falling back to a DOM-only snapshot\n");
                    CefRefPtr<SnapshotVisitor> visitor(new SnapshotVisitor(
                        command.id, base + ".html", state.final_url, state.title));
                    state.browser->GetMainFrame()->GetSource(visitor);
                }
                // The reply is sent by the capture object when the payload arrives.
            } else if (command.op == "set_bounds") {
                if (!state.browser) {
                    link.ReplyError(command.id, "surface_not_found", "no live surface");
                    continue;
                }
#ifdef _WIN32
                if (state.window_owned) {
                    link.ReplyError(command.id, "not_supported",
                                    "this surface is not embedded in a parent window");
                    continue;
                }
                state.bounds_x = command.bounds_x;
                state.bounds_y = command.bounds_y;
                state.bounds_width = command.bounds_width;
                state.bounds_height = command.bounds_height;
                state.bounds_pending = true;
                // Applied now if Chromium's child window already exists, otherwise by the pump loop.
                const bool applied = ApplyPendingEmbeddedBounds(&state);
                CefRefPtr<CefDictionaryValue> placed = CefDictionaryValue::Create();
                placed->SetBool("applied", applied);
                link.Reply(command.id, placed);
#else
                link.ReplyError(command.id, "not_supported", "embedding needs a window system");
#endif
            } else if (command.op == "focus") {
                if (!state.browser) {
                    link.ReplyError(command.id, "surface_not_found", "no live surface");
                    continue;
                }
                state.browser->GetHost()->SetFocus(command.focused);
                link.Reply(command.id);
            } else if (command.op == "destroy_surface") {
                DrainAndCloseBrowser(&state);
                DestroyHostWindow(&state);
                link.Reply(command.id);
            } else if (command.op == "shutdown") {
                shutdown_requested = true;
                link.Reply(command.id);
            } else if (command.op == "eval") {
                if (!state.browser) {
                    link.ReplyError(command.id, "surface_not_found", "no live surface");
                    continue;
                }
                if (command.script.empty()) {
                    link.ReplyError(command.id, "invalid_argument", "empty script");
                    continue;
                }
                CefRefPtr<EvalCapture> capture(new EvalCapture(command.id, command.script));
                if (!capture->Start(state.browser)) {
                    link.ReplyError(command.id, "not_supported",
                                    "DevTools is unavailable, cannot evaluate");
                }
                // The reply is sent by EvalCapture when the value arrives.
            } else {
                link.ReplyError(command.id, "not_supported",
                                "this milestone implements hello, ping, create_surface, navigate, "
                                "eval, set_bounds, focus, destroy_surface and shutdown");
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(kPumpIntervalMs));
    }

    DrainAndCloseBrowser(&state);
    DestroyHostWindow(&state);
    Log("[engine-host] shutting down\n");
    CefShutdown();
    return 0;
}

}  // namespace antikolt
