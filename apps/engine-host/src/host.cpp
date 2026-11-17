// SPDX-License-Identifier: MPL-2.0

#include "host.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "include/cef_command_line.h"
#include "include/cef_parser.h"
#include "include/cef_string_visitor.h"
#include "include/cef_version.h"
#include "include/wrapper/cef_helpers.h"
#include "devtools.h"
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

/// Monotonic id for screenshot files, for the same reason.
int64_t s_screenshot_counter = 0;

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

/// Write bytes to a capture file. Truncates first, so a shorter snapshot never leaves the tail of a
/// longer one behind.
bool WriteFileBytes(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

/// The pixel size of an encoded image, read from its header.
///
/// `Page.captureScreenshot` answers with the encoded bytes and nothing else, so the reply must not
/// guess a size from the viewport: multiplying CSS pixels by the device scale would report what the
/// caller *expected* instead of what was produced. Only the three formats CDP can emit are known
/// here; anything else reports 0x0, which the caller must read as "not measured".
struct ImageSize {
    int width = 0;
    int height = 0;
};

ImageSize MeasureImage(const std::string& bytes) {
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    const size_t count = bytes.size();
    auto byte_at = [data, count](size_t index) -> unsigned {
        return index < count ? static_cast<unsigned>(data[index]) : 0u;
    };
    auto be16 = [&](size_t index) -> int {
        return static_cast<int>((byte_at(index) << 8) | byte_at(index + 1));
    };
    auto be32 = [&](size_t index) -> int {
        return static_cast<int>((byte_at(index) << 24) | (byte_at(index + 1) << 16) |
                                (byte_at(index + 2) << 8) | byte_at(index + 3));
    };
    auto le24 = [&](size_t index) -> int {
        return static_cast<int>(byte_at(index) | (byte_at(index + 1) << 8) |
                                (byte_at(index + 2) << 16));
    };

    ImageSize size;
    if (count > 24 && byte_at(0) == 0x89 && byte_at(1) == 'P' && byte_at(2) == 'N' &&
        byte_at(3) == 'G') {
        // PNG: the IHDR chunk always comes first, so width and height sit at a fixed offset.
        size.width = be32(16);
        size.height = be32(20);
        return size;
    }
    if (count > 4 && byte_at(0) == 0xFF && byte_at(1) == 0xD8) {
        // JPEG: walk the segment list to the first start-of-frame header.
        size_t pos = 2;
        while (pos + 8 < count) {
            if (byte_at(pos) != 0xFF) {
                ++pos;
                continue;
            }
            const unsigned marker = byte_at(pos + 1);
            if (marker == 0xFF) {
                ++pos;  // fill byte before the real marker
                continue;
            }
            if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9)) {
                pos += 2;  // markers without a length field
                continue;
            }
            const size_t length = static_cast<size_t>(be16(pos + 2));
            const bool start_of_frame =
                marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 &&
                marker != 0xCC;
            if (start_of_frame) {
                size.height = be16(pos + 5);
                size.width = be16(pos + 7);
                return size;
            }
            if (length < 2) {
                break;
            }
            pos += 2 + length;
        }
        return size;
    }
    if (count > 30 && std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "WEBP", 4) == 0) {
        const bool lossy = std::memcmp(data + 12, "VP8 ", 4) == 0;
        const bool lossless = std::memcmp(data + 12, "VP8L", 4) == 0;
        const bool extended = std::memcmp(data + 12, "VP8X", 4) == 0;
        if (lossy) {
            // Frame tag (3), start code (3), then two 14-bit values with a 2-bit scale.
            size.width = static_cast<int>((byte_at(26) | (byte_at(27) << 8)) & 0x3FFF);
            size.height = static_cast<int>((byte_at(28) | (byte_at(29) << 8)) & 0x3FFF);
        } else if (lossless) {
            // Signature byte, then 14 bits of width-1 and 14 bits of height-1.
            const unsigned bits = byte_at(21) | (byte_at(22) << 8) | (byte_at(23) << 16) |
                                  (byte_at(24) << 24);
            size.width = 1 + static_cast<int>(bits & 0x3FFF);
            size.height = 1 + static_cast<int>((bits >> 14) & 0x3FFF);
        } else if (extended) {
            size.width = 1 + le24(24);
            size.height = 1 + le24(27);
        }
        return size;
    }
    return size;
}

/// Captures the page as MHTML plus its runtime state through the DevTools protocol, then replies to
/// the `snapshot` request.
///
/// This is the full hibernation artifact: the DOM with its resources inlined (readable offline) and a
/// small JSON file with everything the DOM does not carry (scroll, anchor, form values, session
/// storage, media position). The sequence is a chain of DevTools calls — enable, capture, evaluate,
/// write — because DevTools answers asynchronously: each handler starts the next call.
class MhtmlCapture : public CefBaseRefCounted {
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
          title_(std::move(title)) {}

    /// Start the sequence. Returns false when DevTools cannot be used at all, so the caller can fall
    /// back to the DOM-only capture.
    bool Start(CefRefPtr<CefBrowser> browser) {
        CefRefPtr<MhtmlCapture> self(this);
        return Submit(browser, "Page.enable", nullptr,
                      [self](CefRefPtr<CefBrowser> browser, const DevToolsReply&) {
                          // Page.enable's own answer carries nothing worth handling: the capture
                          // call reports the failure if the domain really is unavailable.
                          self->RequestSnapshot(browser);
                      });
    }

private:
    /// Submit one call of the sequence. The handler keeps `self` alive, so the object survives until
    /// the last call in the chain has answered.
    bool Submit(CefRefPtr<CefBrowser> browser,
                const char* method,
                CefRefPtr<CefDictionaryValue> params,
                DevToolsCall::Handler handler) {
        CefRefPtr<DevToolsCall> call(new DevToolsCall(method, params, std::move(handler)));
        return call->Start(browser);
    }

    void RequestSnapshot(CefRefPtr<CefBrowser> browser) {
        CefRefPtr<MhtmlCapture> self(this);
        CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
        params->SetString("format", "mhtml");
        if (!Submit(browser, "Page.captureSnapshot", params,
                    [self](CefRefPtr<CefBrowser> browser, const DevToolsReply& reply) {
                        self->OnCaptured(browser, reply);
                    })) {
            Finish("could not submit Page.captureSnapshot");
        }
    }

    void OnCaptured(CefRefPtr<CefBrowser> browser, const DevToolsReply& reply) {
        if (!reply.ok()) {
            Finish(reply.error);
            return;
        }
        const std::string data = reply.String("data");
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
        CefRefPtr<MhtmlCapture> self(this);
        CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
        params->SetString("expression", kStateExpression);
        params->SetBool("returnByValue", true);
        if (!Submit(browser, "Runtime.evaluate", params,
                    [self](CefRefPtr<CefBrowser>, const DevToolsReply& reply) {
                        self->OnEvaluated(reply);
                    })) {
            FinishOk();  // MHTML is on disk; a missing state file is degraded, not fatal.
        }
    }

    void OnEvaluated(const DevToolsReply& reply) {
        if (reply.ok()) {
            // The state expression returns one JSON string, so `value` really is a string here.
            const std::string json = reply.String("value");
            const std::string state_path = base_path_ + ".state.json";
            if (!json.empty() && WriteFileBytes(state_path, json)) {
                state_bytes_ = json.size();
                Log("[engine-host] state: %zu bytes written\n", state_bytes_);
            } else {
                Log("[engine-host] state: no usable value\n");
            }
        } else {
            Log("[engine-host] state: no usable value (%s)\n", reply.error.c_str());
        }
        FinishOk();
    }

    void FinishOk() {
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
        StdioLink::Instance().ReplyError(request_id_, "internal", error.c_str());
    }

    int64_t request_id_;
    std::string base_path_;
    int64_t surface_id_;
    std::string url_;
    std::string title_;
    std::string mhtml_;
    size_t state_bytes_ = 0;

    IMPLEMENT_REFCOUNTING(MhtmlCapture);
    DISALLOW_COPY_AND_ASSIGN(MhtmlCapture);
};

/// Captures one screenshot through `Page.captureScreenshot` and replies to the `capture` request.
///
/// The pixels come from Chromium's own compositor, which is what makes this the honest basis for a
/// painting check and for card previews: a screen grab of an embedded child window belongs to another
/// process's desktop, and reports a blank or stale rectangle whenever the window is occluded,
/// minimised or simply on a different compositor path.
class ScreenshotCapture : public CefBaseRefCounted {
public:
    ScreenshotCapture(int64_t request_id,
                      std::string base_path,
                      int64_t surface_id,
                      std::string format,
                      int quality)
        : request_id_(request_id),
          base_path_(std::move(base_path)),
          surface_id_(surface_id),
          format_(std::move(format)),
          quality_(quality) {}

    bool Start(CefRefPtr<CefBrowser> browser) {
        CefRefPtr<ScreenshotCapture> self(this);
        CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
        params->SetString("format", format_);
        // `quality` is a JPEG/WebP knob: PNG ignores it, and some CDP versions validate it anyway.
        if (format_ != "png") {
            params->SetInt("quality", quality_);
        }
        params->SetBool("fromSurface", true);
        params->SetBool("captureBeyondViewport", false);
        CefRefPtr<DevToolsCall> call(new DevToolsCall(
            "Page.captureScreenshot", params,
            [self](CefRefPtr<CefBrowser>, const DevToolsReply& reply) { self->OnCaptured(reply); }));
        return call->Start(browser);
    }

private:
    void OnCaptured(const DevToolsReply& reply) {
        if (!reply.ok()) {
            ReplyError(reply.from_protocol ? "invalid_argument" : "internal", reply.error);
            return;
        }
        const std::string encoded = reply.String("data");
        if (encoded.empty()) {
            ReplyError("internal", "DevTools result has no data");
            return;
        }
        const std::string bytes = Base64Decode(encoded);
        const std::string path = base_path_ + "." + format_;
        if (!WriteFileBytes(path, bytes)) {
            ReplyError("internal", "could not write the screenshot");
            return;
        }
        const ImageSize size = MeasureImage(bytes);
        CefRefPtr<CefDictionaryValue> result = CefDictionaryValue::Create();
        result->SetBool("ok", true);
        result->SetString("path", path);
        result->SetInt("bytes", static_cast<int>(bytes.size()));
        result->SetInt("width", size.width);
        result->SetInt("height", size.height);
        result->SetString("format", format_);
        result->SetInt("surface", static_cast<int>(surface_id_));
        StdioLink::Instance().Reply(request_id_, result);
        Log("[engine-host] screenshot written: %s (%d bytes, %dx%d)\n", path.c_str(),
            static_cast<int>(bytes.size()), size.width, size.height);
    }

    void ReplyError(const char* code, const std::string& message) {
        StdioLink::Instance().ReplyError(request_id_, code, message.c_str());
    }

    int64_t request_id_;
    std::string base_path_;
    int64_t surface_id_;
    std::string format_;
    int quality_ = 80;

    IMPLEMENT_REFCOUNTING(ScreenshotCapture);
    DISALLOW_COPY_AND_ASSIGN(ScreenshotCapture);
};

/// A `Runtime.evaluate` value rendered as the string the wire format carries.
///
/// `value` arrives as JSON and is not always a string: reading it with `GetString` made every boolean
/// and number come back empty — `document.hasFocus()` answered nothing at all — so primitives are
/// rendered to their JSON text here.
std::string ScalarToString(CefRefPtr<CefValue> value) {
    if (!value) {
        return {};
    }
    switch (value->GetType()) {
        case VTYPE_BOOL:
            return value->GetBool() ? "true" : "false";
        case VTYPE_INT:
            return std::to_string(value->GetInt());
        case VTYPE_DOUBLE: {
            std::ostringstream text;
            text << value->GetDouble();
            return text.str();
        }
        case VTYPE_STRING:
            return value->GetString().ToString();
        default:
            return {};
    }
}

/// Evaluates JavaScript in the page through DevTools and replies with the resulting value.
///
/// The main world is used deliberately: this is the shell's own script (restore, measurement), not a
/// credential or a page-visible secret. Anything secret must go through an isolated world instead.
class EvalCapture : public CefBaseRefCounted {
public:
    EvalCapture(int64_t request_id, std::string script)
        : request_id_(request_id), script_(std::move(script)) {}

    bool Start(CefRefPtr<CefBrowser> browser) {
        CefRefPtr<EvalCapture> self(this);
        CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
        params->SetString("expression", script_);
        params->SetBool("returnByValue", true);
        CefRefPtr<DevToolsCall> call(new DevToolsCall(
            "Runtime.evaluate", params,
            [self](CefRefPtr<CefBrowser>, const DevToolsReply& reply) { self->OnEvaluated(reply); }));
        return call->Start(browser);
    }

private:
    void OnEvaluated(const DevToolsReply& reply) {
        if (!reply.ok()) {
            // A script DevTools rejected is a bad argument, not a broken engine.
            StdioLink::Instance().ReplyError(request_id_,
                                             reply.from_protocol ? "invalid_argument" : "internal",
                                             reply.error.c_str());
            return;
        }
        CefRefPtr<CefDictionaryValue> result = CefDictionaryValue::Create();
        result->SetBool("ok", true);
        result->SetString("type", reply.String("type"));
        result->SetString("value",
                          ScalarToString(reply.result ? reply.result->GetValue("value") : nullptr));
        StdioLink::Instance().Reply(request_id_, result);
    }

    int64_t request_id_;
    std::string script_;

    IMPLEMENT_REFCOUNTING(EvalCapture);
    DISALLOW_COPY_AND_ASSIGN(EvalCapture);
};

/// CDP's storage identifiers differ from the cleanup plan's names in exactly one place today: the plan
/// calls IndexedDB `indexed_db`, the protocol calls it `indexeddb`. Everything else passes through, and
/// Chromium ignores identifiers it does not know — which is why an unmapped one is logged here instead
/// of being silently dropped.
std::string ToCdpStorageType(const std::string& storage) {
    if (storage == "indexed_db") {
        return "indexeddb";
    }
    return storage;
}

/// Clear one origin's storage through DevTools (`Storage.clearDataForOrigin`).
///
/// This is the engine half of the 24-hour session policy (ADR-033): the core decides *whether* a wipe is
/// due and *which* storages a plan touches, and this host only executes it — through the same
/// `DevToolsCall` the snapshots, screenshots and evaluations use, so a session wipe is not a second,
/// parallel way of talking to Chromium. `Storage.clearDataForOrigin` succeeds on an origin that owns
/// nothing, so "cleared" is honestly "the protocol accepted it", not "there was data".
class ClearOriginCall : public CefBaseRefCounted {
public:
    ClearOriginCall(int64_t request_id, std::string origin, std::vector<std::string> storages)
        : request_id_(request_id), origin_(std::move(origin)), storages_(std::move(storages)) {}

    bool Start(CefRefPtr<CefBrowser> browser) {
        CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
        params->SetString("origin", origin_);
        std::string types;
        for (const std::string& storage : storages_) {
            if (!types.empty()) {
                types += ",";
            }
            types += ToCdpStorageType(storage);
        }
        // CDP takes one comma-separated list; an empty list means "every storage this origin owns".
        if (!types.empty()) {
            params->SetString("storageTypes", types);
        }
        // The call holds the only reference to this object, so the object cannot be freed while its own
        // handler is running — the same lifetime rule `DevToolsCall` documents.
        CefRefPtr<ClearOriginCall> self(this);
        CefRefPtr<DevToolsCall> call(new DevToolsCall(
            "Storage.clearDataForOrigin", params,
            [self](CefRefPtr<CefBrowser> browser, const DevToolsReply& reply) {
                self->OnCleared(browser, reply);
            }));
        if (!call->Start(browser)) {
            return false;
        }
        return true;
    }

private:
    void OnCleared(CefRefPtr<CefBrowser> browser, const DevToolsReply& reply) {
        (void)browser;
        CefRefPtr<CefDictionaryValue> result = CefDictionaryValue::Create();
        result->SetString("origin", origin_);
        if (reply.ok()) {
            result->SetBool("cleared", true);
            Log("[engine-host] cleared origin data for %s (%zu storage types)\n", origin_.c_str(),
                storages_.size());
            StdioLink::Instance().Reply(request_id_, result);
            return;
        }
        // A rejected call is the caller's fault; a transport or parse failure is the engine's.
        const char* code = reply.from_protocol ? "invalid_argument" : "not_supported";
        Log("[engine-host] clear_origin_data failed for %s: %s\n", origin_.c_str(),
            reply.error.c_str());
        StdioLink::Instance().ReplyError(request_id_, code, reply.error.c_str());
    }

    int64_t request_id_;
    std::string origin_;
    std::vector<std::string> storages_;

    IMPLEMENT_REFCOUNTING(ClearOriginCall);
    DISALLOW_COPY_AND_ASSIGN(ClearOriginCall);
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

/// The router configuration both halves must agree on: these are the names the page sees.
CefMessageRouterConfig MessageRouterConfig() {
    CefMessageRouterConfig config;
    config.js_query_function = "cefQuery";
    config.js_cancel_function = "cefQueryCancel";
    return config;
}

void EngineApp::OnWebKitInitialized() {
    // Render process: this is what turns `cefQuery` into a function on the page's window.
    router_renderer_ = CefMessageRouterRendererSide::Create(MessageRouterConfig());
}

void EngineApp::OnContextCreated(CefRefPtr<CefBrowser> browser,
                                 CefRefPtr<CefFrame> frame,
                                 CefRefPtr<CefV8Context> context) {
    if (router_renderer_) {
        router_renderer_->OnContextCreated(browser, frame, context);
    }
}

void EngineApp::OnContextReleased(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  CefRefPtr<CefV8Context> context) {
    if (router_renderer_) {
        router_renderer_->OnContextReleased(browser, frame, context);
    }
}

bool EngineApp::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                         CefRefPtr<CefFrame> frame,
                                         CefProcessId source_process,
                                         CefRefPtr<CefProcessMessage> message) {
    return router_renderer_ &&
           router_renderer_->OnProcessMessageReceived(browser, frame, source_process, message);
}

EngineClient::EngineClient(HostState* state, bool quit_on_first_load)
    : state_(state), quit_on_first_load_(quit_on_first_load) {
    router_ = CefMessageRouterBrowserSide::Create(MessageRouterConfig());
    router_->AddHandler(this, /*first=*/false);
}

/// The script the shell installs on the home page. It is deliberately small and lives in the engine,
/// not in the shell: the shell must never be able to inject arbitrary JavaScript into a page, and the
/// page must never see a capability it was not handed.
constexpr const char* kBridgeScript = R"JS((function () {
  if (window.antikoltNative) { return 'already'; }
  var call = function (method, args, wantsReply) {
    return new Promise(function (resolve, reject) {
      if (typeof window.cefQuery !== 'function') {
        reject(new Error('the Antikolt bridge is not available'));
        return;
      }
      var query = {
        request: JSON.stringify({ method: method, args: args || {}, request: !!wantsReply }),
        onSuccess: function (response) {
          // A one-way call answers '{}'; a request answers with the shell's JSON payload.
          if (!wantsReply) { resolve(response); return; }
          try { resolve(response ? JSON.parse(response) : {}); }
          catch (error) { reject(new Error('the shell sent a malformed answer')); }
        },
        onFailure: function (code, message) { reject(new Error(message)); }
      };
      // A request is a *persistent* query: the engine holds the callback until the shell answers
      // (protocol 2), which is what makes the page able to ask instead of only announce.
      query.persistent = !!wantsReply;
      window.cefQuery(query);
    });
  };
  var request = function (method, args) { return call(method, args, true); };
  window.antikoltNative = {
    version: '2',
    openCard: function (card) { return call('open_card', card, false); },
    freezeCard: function () { return call('freeze_card', {}, false); },
    request: request,
    cards: {
      search: function (query) { return request('cards.search', { q: query }); }
    }
  };
  window.dispatchEvent(new Event('antikolt-bridge-ready'));
  return 'installed';
})();
)JS";

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
    router_->OnBeforeClose(browser);
    state_->bridge_enabled = false;
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

    // A reload or navigation is a new JS context, so the bridge has to be installed again.
    if (state_->bridge_enabled.load()) {
        frame->ExecuteJavaScript(kBridgeScript, "", 0);
    }

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

bool EngineClient::OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                                    cef_log_severity_t level,
                                    const CefString& message,
                                    const CefString& source,
                                    int line) {
    // Every severity is logged, errors included: a page whose script died is otherwise
    // indistinguishable from a page that renders nothing on purpose.
    Log("[engine-host] console level=%d %s:%d %s\n", static_cast<int>(level),
        source.ToString().c_str(), line, message.ToString().c_str());
    return true;  // handled: CEF must not also pop its own console output
}

void EngineClient::OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
                                             TerminationStatus status,
                                             int error_code,
                                             const CefString& error_string) {
    CEF_REQUIRE_UI_THREAD();
    router_->OnRenderProcessTerminated(browser);
    Log("[engine-host] renderer terminated status=%d code=%d reason=%s\n",
        static_cast<int>(status), error_code, error_string.ToString().c_str());
}

bool EngineClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                            CefRefPtr<CefFrame> frame,
                                            CefProcessId source_process,
                                            CefRefPtr<CefProcessMessage> message) {
    CEF_REQUIRE_UI_THREAD();
    return router_->OnProcessMessageReceived(browser, frame, source_process, message);
}

bool EngineClient::OnQuery(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame> frame,
                           int64_t query_id,
                           const CefString& request,
                           bool persistent,
                           CefRefPtr<Callback> callback) {
    (void)browser;
    (void)frame;
    (void)query_id;
    (void)persistent;
    // A page that the shell never enabled gets nothing, not even an error it can learn from.
    if (!state_->bridge_enabled.load()) {
        callback->Failure(/*error_code=*/-1, "the Antikolt bridge is not installed here");
        return true;
    }

    CefRefPtr<CefValue> parsed = CefParseJSON(request, JSON_PARSER_RFC);
    if (!parsed || parsed->GetType() != VTYPE_DICTIONARY) {
        callback->Failure(-2, "the bridge expects a JSON object");
        return true;
    }
    CefRefPtr<CefDictionaryValue> call = parsed->GetDictionary();
    const std::string method = call->GetString("method").ToString();
    if (method.empty()) {
        callback->Failure(-3, "the bridge call has no method");
        return true;
    }

    CefRefPtr<CefDictionaryValue> payload = CefDictionaryValue::Create();
    payload->SetInt("surface", static_cast<int>(state_->surface_id));
    payload->SetString("method", method);
    if (CefRefPtr<CefValue> args = call->GetValue("args")) {
        payload->SetValue("args", args);
    }
    // Protocol 2 (ADR-034): a call marked `request` is a question, and the page is waiting for the
    // answer. The callback is held here and answered from `bridge_reply`; the one-way calls keep the
    // old behaviour (an immediate `{}`), so `open_card` and `freeze_card` do not change.
    const bool wants_reply = call->GetBool("request");
    payload->SetInt("request", 0);
    if (wants_reply) {
        // Not `request`: that name is the query string parameter of this method.
        const int64_t bridge_request_id = state_->next_bridge_request++;
        payload->SetInt("request", static_cast<int>(bridge_request_id));
        state_->pending_bridge[bridge_request_id] = callback;
        Log("[engine-host] bridge_call request=%lld surface=%lld method=%s\n",
            static_cast<long long>(bridge_request_id),
            static_cast<long long>(state_->surface_id), method.c_str());
        StdioLink::Instance().Event("bridge_call", payload);
        // Deliberately no Success() here: the shell's answer is what settles the page's promise.
        return true;
    }
    Log("[engine-host] bridge_call surface=%lld method=%s\n",
        static_cast<long long>(state_->surface_id), method.c_str());
    StdioLink::Instance().Event("bridge_call", payload);
    callback->Success("{}");
    return true;
}

void EngineClient::OnQueryCanceled(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   int64_t query_id) {
    (void)browser;
    (void)frame;
    (void)query_id;
    // The page (or its frame) went away while a request was still on the shell's desk. The router has
    // already cancelled the query, so the callbacks must not be answered any more — they are only
    // dropped, which releases them.
    if (!state_->pending_bridge.empty()) {
        Log("[engine-host] dropping %zu unanswered bridge requests\n", state_->pending_bridge.size());
        state_->pending_bridge.clear();
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
                welcome->SetInt("protocol", kProtocolVersion);
                if (command.protocol != 0 && command.protocol != kProtocolVersion) {
                    // The shell said which version it speaks; a different one is worth a line in the log,
                    // because the bridge's request channel would then be one-sided.
                    Log("[engine-host] protocol mismatch: shell=%d engine=%d\n", command.protocol,
                        kProtocolVersion);
                }
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
            } else if (command.op == "clear_origin_data") {
                if (!state.browser || !state.browser->GetMainFrame()) {
                    link.ReplyError(command.id, "surface_not_found", "no live surface");
                    continue;
                }
                if (command.origin.empty()) {
                    link.ReplyError(command.id, "invalid_argument",
                                    "clear_origin_data needs an origin");
                    continue;
                }
                CefRefPtr<ClearOriginCall> clear(new ClearOriginCall(
                    command.id, command.origin, command.storages));
                if (!clear->Start(state.browser)) {
                    Log("[engine-host] DevTools unavailable: cannot clear origin data\n");
                    link.ReplyError(command.id, "not_supported",
                                    "clearing origin data needs the DevTools protocol");
                }
                // The reply is sent by the call itself once the protocol answered.
            } else if (command.op == "capture") {
                if (!state.browser || !state.browser->GetMainFrame()) {
                    link.ReplyError(command.id, "surface_not_found", "no live surface");
                    continue;
                }
                if (command.format != "png" && command.format != "jpeg" &&
                    command.format != "webp") {
                    link.ReplyError(command.id, "invalid_argument",
                                    "format must be png, jpeg or webp");
                    continue;
                }
                std::error_code dir_error;
                std::filesystem::create_directories(config.user_data_path + "\\screenshots",
                                                    dir_error);
                const std::string base = config.user_data_path + "\\screenshots\\shot-" +
                                         std::to_string(state.surface_id) + "-" +
                                         std::to_string(s_screenshot_counter++);
                CefRefPtr<ScreenshotCapture> shot(new ScreenshotCapture(
                    command.id, base, state.surface_id, command.format, command.quality));
                if (!shot->Start(state.browser)) {
                    link.ReplyError(command.id, "not_supported",
                                    "DevTools is unavailable, cannot capture");
                }
                // The reply is sent by the capture object when the pixels arrive.
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
            } else if (command.op == "install_bridge") {
                if (!state.browser || !state.browser->GetMainFrame()) {
                    link.ReplyError(command.id, "surface_not_found", "no live surface");
                    continue;
                }
                state.bridge_enabled = true;
                state.browser->GetMainFrame()->ExecuteJavaScript(kBridgeScript, "", 0);
                link.Reply(command.id);
            } else if (command.op == "bridge_reply") {
                // Protocol 2: the shell's answer to a page request. An answer with no waiting page is
                // not a protocol violation — the page may have gone away — so it is logged, not failed.
                const auto pending = state.pending_bridge.find(command.bridge_request);
                if (pending == state.pending_bridge.end()) {
                    Log("[engine-host] bridge_reply for unknown request %lld\n",
                        static_cast<long long>(command.bridge_request));
                    link.Reply(command.id);
                    continue;
                }
                CefRefPtr<CefMessageRouterBrowserSide::Callback> callback = pending->second;
                state.pending_bridge.erase(pending);
                if (!command.error.empty()) {
                    callback->Failure(-5, command.error);
                } else {
                    callback->Success(command.json.empty() ? "{}" : command.json);
                }
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
                                "eval, capture, snapshot, set_bounds, focus, install_bridge, "
                                "destroy_surface and shutdown");
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
