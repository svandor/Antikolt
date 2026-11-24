// SPDX-License-Identifier: MPL-2.0

#include "host.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <psapi.h>
#endif

#include "include/cef_command_line.h"
#include "include/cef_parser.h"
#include "include/cef_string_visitor.h"
#include "include/cef_version.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_helpers.h"
#include "include/wrapper/cef_stream_resource_handler.h"
#include "devtools.h"
#include "protocol.h"

namespace antikolt {

namespace {

/// A page that never finishes loading must not hang the harness.
constexpr int kLoadTimeoutSeconds = 30;

constexpr wchar_t kWindowClass[] = L"AntikoltEngineHostWindow";

/// Never show a crash dialog: a failed run must exit with a code that CI can read.
constexpr int kPumpIntervalMs = 2;

/// True when the host runs as the shell's protocol server (stdout carries frames only).
bool g_server_mode = false;

int64_t g_next_surface_id = 1;

/// Most surfaces one engine process may hold at once (ADR-041).
///
/// The rule this encodes belongs to the shell (invariant I1: one live card), but the engine is where it
/// becomes unbreakable. Two is the documented, bounded exception: the user's surface and one short-lived
/// background preview worker. A third request is refused with `engine_busy` rather than quietly growing
/// the process.
constexpr std::size_t kMaxSurfaces = 2;

/// The login popup's lifetime (docs/HU/02 §8.5): a short OAuth/SSO window, not a second card.
constexpr int64_t kPopupTimeoutMs = 60000;

/// Steady-clock milliseconds, so the popup timer is immune to wall-clock changes.
int64_t SteadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Native window → the surface it hosts.
///
/// Only the UI thread touches this. It exists because the window procedure receives a handle, not a
/// pointer: with more than one surface per process, "which state does this WM_CLOSE belong to" has an
/// answer, and closing the preview window must not shut the browser down.
std::map<void*, HostState*>& WindowRegistry() {
    static std::map<void*, HostState*> registry;
    return registry;
}

void RegisterWindow(void* window, HostState* state) {
    if (window != nullptr) {
        WindowRegistry()[window] = state;
    }
}

void ForgetWindow(void* window) {
    if (window != nullptr) {
        WindowRegistry().erase(window);
    }
}

HostState* StateForWindow(void* window) {
    const auto found = WindowRegistry().find(window);
    return found == WindowRegistry().end() ? nullptr : found->second;
}

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
            // loop close the browser first (CefShutdown with a live browser crashes). In server mode
            // only *that* surface goes away; another one may be the page the user is reading.
            if (HostState* state = StateForWindow(window)) {
                if (g_server_mode) {
                    state->close_requested = true;
                } else {
                    state->quit_requested = true;
                }
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
    // Portable layout: when the CEF runtime sits next to the host (the dist folder), say so explicitly.
    // Without this the loader can end up with no ICU data ("Invalid file descriptor to ICU data received")
    // even though icudtl.dat is right there — measured in the first packaged build.
    const std::string exe_dir =
        config.exe_path.substr(0, config.exe_path.find_last_of("\\/") == std::string::npos
                                      ? 0
                                      : config.exe_path.find_last_of("\\/"));
    if (std::ifstream(exe_dir + "\\icudtl.dat").good()) {
        CefString(&settings.resources_dir_path).FromString(exe_dir);
        CefString(&settings.locales_dir_path).FromString(exe_dir + "\\locales");
    }
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
    // Windowless rendering is the escape hatch for a host that has no usable compositor (a broken GPU driver, a
    // locked-down session, CI). It is an environment switch, never the product's mode: the shipped browser embeds a
    // real window (`docs/HU/11-OSR.md` explains why both shapes exist).
    const char* windowless = std::getenv("ANTIKOLT_ENGINE_WINDOWLESS");
    const bool use_windowless = windowless != nullptr && *windowless != '\0' &&
                                std::strcmp(windowless, "0") != 0;
    settings.windowless_rendering_enabled = use_windowless;

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
    RegisterWindow(window, state);

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

    CefRefPtr<EngineClient> client_handler(new EngineClient(
        state, config.quit_on_first_load, config.webauthn_test, config.isolated_world_test,
        config.autofill_test, config.passkey_flow_test, config.pip_test, config.pip_window_test,
        config.permissions));
    CefBrowserSettings browser_settings;
    // The surface's request context: its own storage (cookies, cache, localStorage) and its container's proxy. Passing
    // nullptr here was the old behaviour — every card shared one global cookie jar and the `partition` key was a
    // comment (ADR-009 promised isolation the engine never gave). ADR-071 makes it real.
    CefRefPtr<CefRequestContext> context = RequestContextForPartition(config, nullptr);
    if (!CefBrowserHost::CreateBrowser(window_info, client_handler, url, browser_settings, nullptr,
                                       context)) {
        Log("[engine-host] CreateBrowser failed\n");
        ForgetWindow(window);
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
        ForgetWindow(state->window);
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
    // Save-as-PDF (F7 "nyomtatás").
    capabilities->SetString(5, "print_pdf");
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

/// Scrolls the page's document by a number of CSS pixels (`scroll`).
///
/// `window.scrollBy` is deliberate: it is deterministic, needs no pointer position, and is exactly what a
/// restore-scroll-position feature needs. It scrolls the *document*, not an inner overflow container — a
/// gesture-level scroll (`Input.synthesizeScrollGesture`) would move whatever is under a point, which is a
/// different, later feature.
class ScrollCall : public CefBaseRefCounted {
public:
    ScrollCall(int64_t request_id, int x, int y, bool animate)
        : request_id_(request_id), x_(x), y_(y), animate_(animate) {}

    bool Start(CefRefPtr<CefBrowser> browser) {
        CefRefPtr<ScrollCall> self(this);
        CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
        std::ostringstream script;
        script << "window.scrollBy({left:" << x_ << ",top:" << y_
               << ",behavior:'" << (animate_ ? "smooth" : "auto") << "'});'ok'";
        params->SetString("expression", script.str());
        params->SetBool("returnByValue", true);
        CefRefPtr<DevToolsCall> call(new DevToolsCall(
            "Runtime.evaluate", params,
            [self](CefRefPtr<CefBrowser>, const DevToolsReply& reply) { self->OnScrolled(reply); }));
        return call->Start(browser);
    }

private:
    void OnScrolled(const DevToolsReply& reply) {
        if (!reply.ok()) {
            StdioLink::Instance().ReplyError(request_id_,
                                             reply.from_protocol ? "invalid_argument" : "internal",
                                             reply.error.c_str());
            return;
        }
        CefRefPtr<CefDictionaryValue> result = CefDictionaryValue::Create();
        result->SetBool("ok", true);
        result->SetInt("x", x_);
        result->SetInt("y", y_);
        StdioLink::Instance().Reply(request_id_, result);
    }

    int64_t request_id_;
    int x_;
    int y_;
    bool animate_;

    IMPLEMENT_REFCOUNTING(ScrollCall);
    DISALLOW_COPY_AND_ASSIGN(ScrollCall);
};

/// Sets the page zoom (`set_zoom`) by overriding the device metrics.
///
/// Zooming by *reflow* is what a browser does: the CSS viewport shrinks and the text rewraps, instead of the
/// page being scaled (which leaves the layout unchanged and the text blurry). The device scale factor is
/// *multiplied* by the screen's own (measured first through DevTools), so 150 % on a 125 % display gives
/// 1.875 instead of 1.5 — an absolute factor would silently zoom out on a HiDPI screen. 100 % clears it.
class ZoomCall : public CefBaseRefCounted {
public:
    ZoomCall(int64_t request_id, int factor_percent, int viewport_width, int viewport_height)
        : request_id_(request_id),
          factor_percent_(factor_percent),
          viewport_width_(viewport_width),
          viewport_height_(viewport_height) {}

    bool Start(CefRefPtr<CefBrowser> browser) {
        browser_ = browser;
        if (factor_percent_ == 100) {
            return Apply(1.0);
        }
        CefRefPtr<ZoomCall> self(this);
        CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
        params->SetString("expression", "String(window.devicePixelRatio)");
        params->SetBool("returnByValue", true);
        CefRefPtr<DevToolsCall> call(new DevToolsCall(
            "Runtime.evaluate", params,
            [self](CefRefPtr<CefBrowser>, const DevToolsReply& reply) {
                double natural = 1.0;
                if (reply.ok()) {
                    const std::string text =
                        ScalarToString(reply.result ? reply.result->GetValue("value") : nullptr);
                    if (!text.empty()) {
                        natural = std::atof(text.c_str());
                    }
                }
                if (!(natural > 0.0)) {
                    natural = 1.0;
                }
                self->Apply(natural);
            }));
        return call->Start(browser);
    }

private:
    bool Apply(double natural_dpr) {
        CefRefPtr<ZoomCall> self(this);
        const char* method = "Emulation.setDeviceMetricsOverride";
        CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
        if (factor_percent_ != 100) {
            const double zoom = static_cast<double>(factor_percent_) / 100.0;
            // The shell's bounds are CSS pixels at 100 %; at 150 % the same window holds fewer of them.
            params->SetInt("width", static_cast<int>(viewport_width_ / zoom));
            params->SetInt("height", static_cast<int>(viewport_height_ / zoom));
            params->SetDouble("deviceScaleFactor", natural_dpr * zoom);
            params->SetBool("mobile", false);
        } else {
            method = "Emulation.clearDeviceMetricsOverride";
            params = nullptr;
        }
        CefRefPtr<DevToolsCall> call(new DevToolsCall(
            method, params,
            [self](CefRefPtr<CefBrowser>, const DevToolsReply& reply) { self->OnZoomed(reply); }));
        return call->Start(browser_);
    }

    void OnZoomed(const DevToolsReply& reply) {
        if (!reply.ok()) {
            StdioLink::Instance().ReplyError(request_id_,
                                             reply.from_protocol ? "invalid_argument" : "internal",
                                             reply.error.c_str());
            return;
        }
        CefRefPtr<CefDictionaryValue> result = CefDictionaryValue::Create();
        result->SetBool("ok", true);
        result->SetInt("factor_percent", factor_percent_);
        StdioLink::Instance().Reply(request_id_, result);
    }

    int64_t request_id_;
    int factor_percent_;
    int viewport_width_;
    int viewport_height_;
    CefRefPtr<CefBrowser> browser_;

    IMPLEMENT_REFCOUNTING(ZoomCall);
    DISALLOW_COPY_AND_ASSIGN(ZoomCall);
};

/// Darkens a page that has no dark theme of its own (`set_forced_dark`, ADR-044/ADR-045).
///
/// This is the *user's* switch, off by default: the site theme query (ADR-037) only tells a page what the
/// system prefers, and a page without a dark theme stays light. When the user asks for dark pages, this injects
/// one named style element. The element is *replaced* on a mode change and removed on `off`, so switching never
/// needs a reload and never touches anything the host did not add.
///
/// The modes are the same trick at different strengths — invert the page and invert images back (the classic
/// double inversion, which keeps photographs looking like photographs) — and `contrast()` is what lifts the
/// inverted black to a dark grey instead of pitch black.
class ForcedDark : public CefBaseRefCounted {
public:
    ForcedDark(int64_t request_id, std::string mode) : request_id_(request_id), mode_(std::move(mode)) {}

    /// The modes the host understands; anything else is refused, so a typo cannot paint a page white-on-white.
    static bool IsKnownMode(const std::string& mode) {
        return mode == "off" || mode == "invert" || mode == "dim" || mode == "warm";
    }

    bool Start(CefRefPtr<CefBrowser> browser) {
        CefRefPtr<ForcedDark> self(this);
        CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
        params->SetString("expression", Script());
        params->SetBool("returnByValue", true);
        CefRefPtr<DevToolsCall> call(new DevToolsCall(
            "Runtime.evaluate", params,
            [self](CefRefPtr<CefBrowser>, const DevToolsReply& reply) { self->OnApplied(reply); }));
        return call->Start(browser);
    }

private:
    /// The page filter for this mode. `off` gets no filter — the element is removed instead.
    std::string PageFilter() const {
        if (mode_ == "invert") {
            return "invert(1) hue-rotate(180deg)";
        }
        if (mode_ == "dim") {
            // contrast() pulls the inverted black up to a dark grey and the text down from pure white.
            return "invert(1) hue-rotate(180deg) contrast(0.72)";
        }
        if (mode_ == "warm") {
            // Sepia after the inversion is a warm tint on the darkened page, not a beige one.
            return "invert(1) hue-rotate(180deg) contrast(0.78) sepia(0.35)";
        }
        return {};
    }

    std::string Script() const {
        // One expression, so the whole switch is a single round trip. Images are always re-inverted with the
        // plain double inversion, whatever the page filter is: a photograph must not be dimmed a second time.
        std::string script =
            "(function(){var id='antikolt-forced-dark';var old=document.getElementById(id);";
        if (mode_ == "off") {
            script += "if(old){old.remove();}return 'off';})()";
            return script;
        }
        const std::string page = PageFilter();
        script += "var css='html{filter:" + page + ";}'"
                  "+'img,video,picture,canvas,svg,iframe{filter:invert(1) hue-rotate(180deg);}';";
        script += "if(old){old.textContent=css;return 'on';}";
        script += "var s=document.createElement('style');s.id=id;s.textContent=css;";
        script += "document.documentElement.appendChild(s);return 'on';})()";
        return script;
    }

    void OnApplied(const DevToolsReply& reply) {
        if (!reply.ok()) {
            StdioLink::Instance().ReplyError(request_id_,
                                             reply.from_protocol ? "invalid_argument" : "internal",
                                             reply.error.c_str());
            return;
        }
        CefRefPtr<CefDictionaryValue> result = CefDictionaryValue::Create();
        result->SetBool("ok", true);
        result->SetString("mode", mode_ == "off" ? "off" : mode_);
        StdioLink::Instance().Reply(request_id_, result);
    }

    int64_t request_id_;
    std::string mode_ = "off";

    IMPLEMENT_REFCOUNTING(ForcedDark);
    DISALLOW_COPY_AND_ASSIGN(ForcedDark);
};

/// Caps the displayed height of top-level videos (`set_video_cap`, ADR-020/ADR-076).
///
/// This is the profile's `video_max_height` (720p on `Saver`, 480p on `Ultra`, …) applied to the page: one
/// named style element (`max-height` on `video`), replaced on a change and removed on 0 — exactly the same
/// trick the dark-pages switch uses, so a profile change never needs a reload. It is a *display* cap, not a
/// decode-resolution cap: CEF exposes no per-page video decode-resolution limit, and a cross-origin iframe
/// keeps its own document (a security boundary this host cannot cross), so the cap reaches only top-level
/// `video` elements of the page's own document. The honest consequence, documented rather than hidden: it
/// limits the rendered box, not the decoded stream.
class VideoCap : public CefBaseRefCounted {
public:
    VideoCap(int64_t request_id, int max_height) : request_id_(request_id), max_height_(max_height) {}

    bool Start(CefRefPtr<CefBrowser> browser) {
        CefRefPtr<VideoCap> self(this);
        CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
        params->SetString("expression", Script());
        params->SetBool("returnByValue", true);
        CefRefPtr<DevToolsCall> call(new DevToolsCall(
            "Runtime.evaluate", params,
            [self](CefRefPtr<CefBrowser>, const DevToolsReply& reply) { self->OnApplied(reply); }));
        return call->Start(browser);
    }

private:
    std::string Script() const {
        // One expression, so the whole switch is a single round trip. The rule targets only top-level
        // `video` elements — the honest scope, since iframes are out of reach (see the class comment).
        std::string script =
            "(function(){var id='antikolt-video-cap';var old=document.getElementById(id);";
        if (max_height_ <= 0) {
            script += "if(old){old.remove();}return 'off';})()";
            return script;
        }
        // `max_height_` is a validated integer, so the CSS carries no user text and cannot escape the rule.
        const std::string css = "video{max-height:" + std::to_string(max_height_) + "px;}";
        script += "var css='" + css + "';";
        script += "if(old){old.textContent=css;return 'on';}";
        script += "var s=document.createElement('style');s.id=id;s.textContent=css;";
        script += "document.documentElement.appendChild(s);return 'on';})()";
        return script;
    }

    void OnApplied(const DevToolsReply& reply) {
        if (!reply.ok()) {
            StdioLink::Instance().ReplyError(request_id_,
                                             reply.from_protocol ? "invalid_argument" : "internal",
                                             reply.error.c_str());
            return;
        }
        CefRefPtr<CefDictionaryValue> result = CefDictionaryValue::Create();
        result->SetBool("ok", true);
        result->SetInt("max_height", max_height_);
        StdioLink::Instance().Reply(request_id_, result);
    }

    int64_t request_id_;
    int max_height_ = 0;

    IMPLEMENT_REFCOUNTING(VideoCap);
    DISALLOW_COPY_AND_ASSIGN(VideoCap);
};

/// Emulates the page's `prefers-color-scheme` (`set_media`, ADR-037).
///
/// This is how a page that *supports* a dark theme actually gets one: the browser reports the emulated
/// value through `Emulation.setEmulatedMedia`, and the page's own CSS does the rest. A page without such
/// support is untouched — forcing colours onto a page that never asked for them is a different, riskier
/// feature (and would need `forced-colors`, not this).
class MediaEmulation : public CefBaseRefCounted {
public:
    MediaEmulation(int64_t request_id, std::string media)
        : request_id_(request_id), media_(std::move(media)) {}

    bool Start(CefRefPtr<CefBrowser> browser) {
        CefRefPtr<MediaEmulation> self(this);
        CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
        CefRefPtr<CefListValue> features = CefListValue::Create();
        if (media_ != "auto") {
            CefRefPtr<CefDictionaryValue> feature = CefDictionaryValue::Create();
            feature->SetString("name", "prefers-color-scheme");
            feature->SetString("value", media_);
            features->SetDictionary(0, feature);
        }
        // An empty feature list is how the override is cleared.
        params->SetList("features", features);
        CefRefPtr<DevToolsCall> call(new DevToolsCall(
            "Emulation.setEmulatedMedia", params,
            [self](CefRefPtr<CefBrowser>, const DevToolsReply& reply) { self->OnApplied(reply); }));
        return call->Start(browser);
    }

private:
    void OnApplied(const DevToolsReply& reply) {
        if (!reply.ok()) {
            StdioLink::Instance().ReplyError(request_id_,
                                             reply.from_protocol ? "invalid_argument" : "internal",
                                             reply.error.c_str());
            return;
        }
        CefRefPtr<CefDictionaryValue> result = CefDictionaryValue::Create();
        result->SetBool("ok", true);
        result->SetString("media", media_);
        StdioLink::Instance().Reply(request_id_, result);
    }

    int64_t request_id_;
    std::string media_;

    IMPLEMENT_REFCOUNTING(MediaEmulation);
    DISALLOW_COPY_AND_ASSIGN(MediaEmulation);
};

/// Accounts what one surface costs: DOM nodes and JS heap from the page, plus this process's working set.
///
/// The memory promise is only honest if it is measured, so the numbers come from the browser itself rather
/// than from a guess: `Runtime.evaluate` for the page's own view (its node count and `performance.memory`)
/// and `GetProcessMemoryInfo` for the engine host's footprint. The reply is
/// `{dom_nodes, js_heap_bytes, engine_working_set_bytes}` — the shape `Message::Metrics` documents.
class MetricsCapture : public CefBaseRefCounted {
public:
    explicit MetricsCapture(int64_t request_id) : request_id_(request_id) {}

    bool Start(CefRefPtr<CefBrowser> browser) {
        CefRefPtr<MetricsCapture> self(this);
        CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
        params->SetString("expression",
                          "JSON.stringify({nodes: document.getElementsByTagName('*').length,"
                          " heap: (performance.memory && performance.memory.usedJSHeapSize) || 0})");
        params->SetBool("returnByValue", true);
        CefRefPtr<DevToolsCall> call(new DevToolsCall(
            "Runtime.evaluate", params,
            [self](CefRefPtr<CefBrowser>, const DevToolsReply& reply) { self->OnMeasured(reply); }));
        return call->Start(browser);
    }

private:
    void OnMeasured(const DevToolsReply& reply) {
        if (!reply.ok()) {
            StdioLink::Instance().ReplyError(request_id_,
                                             reply.from_protocol ? "invalid_argument" : "internal",
                                             reply.error.c_str());
            return;
        }
        const std::string json =
            ScalarToString(reply.result ? reply.result->GetValue("value") : nullptr);
        int64_t nodes = 0;
        int64_t heap = 0;
        if (CefRefPtr<CefValue> parsed = CefParseJSON(CefString(json), JSON_PARSER_RFC)) {
            if (CefRefPtr<CefDictionaryValue> measured = parsed->GetDictionary()) {
                nodes = measured->GetInt("nodes");
                heap = measured->GetInt("heap");
            }
        }
        int64_t working_set = 0;
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS_EX counters{};
        if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                                    reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                    sizeof(counters))) {
            working_set = static_cast<int64_t>(counters.WorkingSetSize);
        }
#endif
        CefRefPtr<CefDictionaryValue> result = CefDictionaryValue::Create();
        result->SetBool("ok", true);
        result->SetInt("dom_nodes", static_cast<int>(nodes));
        result->SetString("js_heap_bytes", std::to_string(heap));
        result->SetString("engine_working_set_bytes", std::to_string(working_set));
        StdioLink::Instance().Reply(request_id_, result);
    }

    int64_t request_id_;

    IMPLEMENT_REFCOUNTING(MetricsCapture);
    DISALLOW_COPY_AND_ASSIGN(MetricsCapture);
};

/// Writes the live page to a PDF file (`print_pdf`, F7 "nyomtatás"). The browser's own compositor does the
/// work (`CefBrowserHost::PrintToPDF`); the shell names the file, and the callback replies `{ok, path}` once the
/// PDF is on disk — the same "the object replies when the work lands" pattern the capture classes use.
class PdfPrint : public CefPdfPrintCallback {
public:
    explicit PdfPrint(int64_t request_id, std::string path)
        : request_id_(request_id), path_(std::move(path)) {}

    void Start(CefRefPtr<CefBrowser> browser) {
        // Default settings: portrait, letter, no header/footer chrome — the PDF is the page's own content.
        CefPdfPrintSettings settings;
        browser->GetHost()->PrintToPDF(CefString(path_), settings, this);
    }

private:
    void OnPdfPrintFinished(const CefString& path, bool ok) override {
        CefRefPtr<CefDictionaryValue> result = CefDictionaryValue::Create();
        result->SetBool("ok", ok);
        result->SetString("path", path.ToString());
        StdioLink::Instance().Reply(request_id_, result);
    }

    int64_t request_id_;
    std::string path_;

    IMPLEMENT_REFCOUNTING(PdfPrint);
    DISALLOW_COPY_AND_ASSIGN(PdfPrint);
};

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

/// Where `antikolt://blob/…` serves from: `<user data>/previews`. Set before CEF starts, because the
/// handler factory is installed in `OnContextInitialized` and a page may ask immediately.
std::string g_previews_path;

constexpr const char* kPreviewScheme = "antikolt";
constexpr const char* kPreviewHost = "blob";

/// Make the preview directory exist. A missing one is not an error; an empty scheme answer would be.
void PreparePreviewDirectory(const HostConfig& config) {
    g_previews_path = config.previews_path.empty()
                          ? config.user_data_path + "\\previews"
                          : config.previews_path;
    std::error_code error;
    std::filesystem::create_directories(g_previews_path, error);
    if (error) {
        Log("[engine-host] cannot create the preview directory %s: %s\n", g_previews_path.c_str(),
            error.message().c_str());
    }
}

/// The content type for a preview file, by extension.
std::string PreviewMimeType(const std::string& name) {
    const std::string::size_type dot = name.rfind('.');
    const std::string extension = dot == std::string::npos ? std::string() : name.substr(dot);
    if (extension == ".png") {
        return "image/png";
    }
    if (extension == ".jpg" || extension == ".jpeg") {
        return "image/jpeg";
    }
    if (extension == ".webp") {
        return "image/webp";
    }
    if (extension == ".html" || extension == ".htm") {
        return "text/html";
    }
    return "application/octet-stream";
}

/// Serve one preview file from the profile.
///
/// A preview URL never reaches the network: the engine answers `antikolt://blob/<name>` from the profile's
/// `previews` directory, so a preview is a local file with a stable name a page can refer to. The name is
/// resolved *inside* that directory — an empty name, a `..` or a path separator is refused — because card
/// data must never be able to read an arbitrary file through the browser.
class PreviewHandlerFactory : public CefSchemeHandlerFactory {
public:
    // `DISALLOW_COPY_AND_ASSIGN` declares a private copy constructor, which suppresses the implicit
    // default one — so it has to be asked for explicitly.
    PreviewHandlerFactory() = default;

    CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser,
                                         CefRefPtr<CefFrame> frame,
                                         const CefString& scheme_name,
                                         CefRefPtr<CefRequest> request) override {
        (void)browser;
        (void)frame;
        (void)scheme_name;
        if (g_previews_path.empty() || !request) {
            return nullptr;
        }
        const std::string url = request->GetURL().ToString();
        const std::string name = url.substr(url.rfind('/') + 1);
        if (name.empty() || name.find("..") != std::string::npos ||
            name.find('\\') != std::string::npos) {
            Log("[engine-host] refused a preview name: %s\n", name.c_str());
            return nullptr;
        }
        CefRefPtr<CefStreamReader> stream =
            CefStreamReader::CreateForFile(g_previews_path + "\\" + name);
        if (!stream) {
            Log("[engine-host] preview not found: %s\n", name.c_str());
            return nullptr;
        }
        return new CefStreamResourceHandler(PreviewMimeType(name), stream);
    }

    IMPLEMENT_REFCOUNTING(PreviewHandlerFactory);
    DISALLOW_COPY_AND_ASSIGN(PreviewHandlerFactory);
};

void EngineApp::OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) {
    // Standard + secure so the scheme behaves like an origin of its own, CORS-enabled so a page may fetch
    // it, and fetch-enabled so `Image`/`fetch` work instead of being treated as an opaque scheme.
    const int options = CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE |
                        CEF_SCHEME_OPTION_CORS_ENABLED | CEF_SCHEME_OPTION_FETCH_ENABLED;
    if (!registrar->AddCustomScheme(kPreviewScheme, options)) {
        Log("[engine-host] cannot register the %s scheme\n", kPreviewScheme);
    }
}

void EngineApp::OnContextInitialized() {
    if (g_previews_path.empty()) {
        return;
    }
    if (!CefRegisterSchemeHandlerFactory(kPreviewScheme, kPreviewHost,
                                         new PreviewHandlerFactory())) {
        Log("[engine-host] cannot install the preview scheme handler\n");
        return;
    }
    Log("[engine-host] previews: %s://%s/ from %s\n", kPreviewScheme, kPreviewHost,
        g_previews_path.c_str());
}

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
        // `disable-gpu` alone still starts a GPU process and still crashes on a broken driver
        // (`Failed to send GpuControl.CreateCommandBuffer`, then `GPU process exited unexpectedly`). Routing the
        // compositor through SwiftShader and giving the GPU process no sandbox is what actually keeps the host
        // alive in that environment; it is slower, which is why it stays an explicit environment switch.
        command_line->AppendSwitch("disable-gpu-sandbox");
        command_line->AppendSwitchWithValue("use-angle", "swiftshader");
        // Do NOT append `disable-software-rasterizer` here: it turns SwiftShader off again, so the shared
        // context for virtualization fails (`ContextResult::kFatalFailure: Failed to create shared context
        // for virtualization`, measured on VECTOR — RTX 5080 + Intel, Chromium 158). `disable-dev-shm-usage`
        // is Linux-only and does nothing on Windows, so it is not appended either.
    }

    // Diagnostic escape hatch for a host whose GPU process crash-loops: space separated extra
    // Chromium switches from the environment, applied last so they win. It exists so a broken
    // GPU stack can be bisected (ANGLE backend, sandbox, blocklist) without a rebuild per attempt;
    // like the switch above it is an environment switch, never a product setting, and it must not
    // be used for a release measurement (docs/HU/18-F0-JELENTES.md).
    if (const char* extra = std::getenv("ANTIKOLT_ENGINE_EXTRA_SWITCHES");
        extra != nullptr && *extra != '\0') {
        std::istringstream tokens(extra);
        std::string token;
        while (tokens >> token) {
            const std::string::size_type equals = token.find('=');
            if (equals == std::string::npos) {
                command_line->AppendSwitch(token);
            } else {
                command_line->AppendSwitchWithValue(token.substr(0, equals), token.substr(equals + 1));
            }
        }
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

EngineClient::EngineClient(HostState* state, bool quit_on_first_load, bool webauthn_test,
                           bool isolated_world_test, bool autofill_test, bool passkey_flow_test,
                           bool pip_test, bool pip_window_test,
                           std::map<std::string, std::string> permissions)
    : state_(state),
      quit_on_first_load_(quit_on_first_load),
      webauthn_test_(webauthn_test),
      isolated_world_test_(isolated_world_test),
      autofill_test_(autofill_test),
      passkey_flow_test_(passkey_flow_test),
      pip_test_(pip_test),
      pip_window_test_(pip_window_test),
      permission_(new PermissionPolicy(std::move(permissions))) {
    router_ = CefMessageRouterBrowserSide::Create(MessageRouterConfig());
    router_->AddHandler(this, /*first=*/false);
}

// ---- the per-container policy (ADR-071) --------------------------------------------------

bool ApplyProxyPreference(CefRefPtr<CefRequestContext> context,
                          const std::string& proxy,
                          const std::string& bypass,
                          std::string* error);

void PartitionContextHandler::OnRequestContextInitialized(
    CefRefPtr<CefRequestContext> context) {
    if (proxy_.empty()) {
        Log("[engine-host] partition=%s proxy=system\n", partition_.c_str());
        return;
    }
    // A preference the build refuses (a command-line proxy, for instance) is reported, not swallowed: a container
    // whose traffic silently goes somewhere else would be the worst possible outcome of this feature.
    proxy_applied_ = ApplyProxyPreference(context, proxy_, bypass_, &proxy_error_);
    Log("[engine-host] partition=%s proxy=%s applied=%d error=%s\n", partition_.c_str(),
        proxy_.c_str(), proxy_applied_ ? 1 : 0, proxy_error_.c_str());
}

const char* PermissionPolicy::KeyForType(uint32_t type) {    // One bit per call: CEF hands the prompt a mask, and the mask is decomposed here so the policy keys stay the
    // readable names the core validates (ADR-071).
    switch (type) {
        case CEF_PERMISSION_TYPE_GEOLOCATION:
            return "geolocation";
        case CEF_PERMISSION_TYPE_NOTIFICATIONS:
            return "notifications";
        case CEF_PERMISSION_TYPE_CLIPBOARD:
            return "clipboard";
        case CEF_PERMISSION_TYPE_MIDI_SYSEX:
            return "midi";
        case CEF_PERMISSION_TYPE_CAMERA_STREAM:
        case CEF_PERMISSION_TYPE_CAMERA_PAN_TILT_ZOOM:
            return "camera";
        case CEF_PERMISSION_TYPE_MIC_STREAM:
            return "microphone";
        case CEF_PERMISSION_TYPE_POINTER_LOCK:
            return "pointer_lock";
        case CEF_PERMISSION_TYPE_KEYBOARD_LOCK:
            return "keyboard_lock";
        case CEF_PERMISSION_TYPE_IDLE_DETECTION:
            return "idle_detection";
        case CEF_PERMISSION_TYPE_LOCAL_FONTS:
            return "local_fonts";
        case CEF_PERMISSION_TYPE_STORAGE_ACCESS:
        case CEF_PERMISSION_TYPE_TOP_LEVEL_STORAGE_ACCESS:
            return "storage_access";
        case CEF_PERMISSION_TYPE_FILE_SYSTEM_ACCESS:
            return "file_system_access";
        case CEF_PERMISSION_TYPE_MULTIPLE_DOWNLOADS:
            return "multiple_downloads";
        case CEF_PERMISSION_TYPE_REGISTER_PROTOCOL_HANDLER:
            return "protocol_handler";
#if CEF_API_ADDED(14700)
        case CEF_PERMISSION_TYPE_SENSORS:
            return "sensors";
#endif
        default:
            return "";
    }
}

bool PermissionPolicy::Permits(const std::map<std::string, std::string>& rules, const char* key) {
    if (key == nullptr || *key == '\0') {
        return false;
    }
    const auto rule = rules.find(key);
    return rule != rules.end() && rule->second == "allow";
}

bool PermissionPolicy::OnRequestMediaAccessPermission(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    const CefString& requesting_origin,
    uint32_t requested_permissions,
    CefRefPtr<CefMediaAccessCallback> callback) {
    const auto rule = Permits(rules_, "media");
    const bool allow = rule;
    Log("[engine-host] permission=media origin=%s allow=%d requested=%u\n",
        requesting_origin.ToString().c_str(), allow ? 1 : 0, requested_permissions);
    if (!callback) {
        return true;
    }
    if (allow) {
        callback->Continue(requested_permissions);
    } else {
        callback->Cancel();
    }
    // Handled either way: the answer is the policy, never a Chromium prompt this host cannot show.
    return true;
}

bool PermissionPolicy::OnShowPermissionPrompt(
    CefRefPtr<CefBrowser> browser,
    uint64_t prompt_id,
    const CefString& requesting_origin,
    uint32_t requested_permissions,
    CefRefPtr<CefPermissionPromptCallback> callback) {
    // Every requested bit must be allowed, and an unnamed bit is a denial: "ask the user" is not an answer this host
    // can give yet, and a silent grant is the one outcome that must never happen by omission.
    bool allow = requested_permissions != 0;
    std::string names;
    for (uint32_t bit = 1; bit != 0 && bit <= requested_permissions; bit <<= 1) {
        if ((requested_permissions & bit) == 0) {
            continue;
        }
        const char* key = KeyForType(bit);
        if (key == nullptr || *key == '\0') {
            allow = false;
            names += "?";
            continue;
        }
        if (!Permits(rules_, key)) {
            allow = false;
        }
        if (!names.empty()) {
            names += ",";
        }
        names += key;
    }
    Log("[engine-host] permission=prompt origin=%s requested=%s allow=%d\n",
        requesting_origin.ToString().c_str(), names.c_str(), allow ? 1 : 0);
    if (callback) {
        callback->Continue(allow ? CEF_PERMISSION_RESULT_ACCEPT : CEF_PERMISSION_RESULT_DENY);
    }
    return true;
}

/// One request context per partition (ADR-071). File-scope rather than function-static so the self test can release
/// them before `CefShutdown`: a CefRefPtr that outlives the library it points into is a crash at exit.
std::map<std::string, CefRefPtr<CefRequestContext>>& PartitionContexts() {
    static std::map<std::string, CefRefPtr<CefRequestContext>> contexts;
    return contexts;
}

std::map<std::string, CefRefPtr<PartitionContextHandler>>& PartitionHandlers() {
    static std::map<std::string, CefRefPtr<PartitionContextHandler>> handlers;
    return handlers;
}

CefRefPtr<CefRequestContext> RequestContextForPartition(const HostConfig& config,
                                                        std::string* created_or_reused) {
    auto& contexts = PartitionContexts();
    auto& handlers = PartitionHandlers();
    // UI thread only: the server loop and the one-shot host both call this from the same thread.
    const std::string key = config.partition.empty() ? "persist:default" : config.partition;
    const auto found = contexts.find(key);
    if (found != contexts.end()) {
        if (created_or_reused != nullptr) {
            *created_or_reused = "reused";
        }
        return found->second;
    }

    CefRequestContextSettings settings;
    // The cache directory is the partition's storage: cookies, localStorage, the lot. Sanitised, because the shell's
    // partition key is a string and a path separator must never turn one partition into another's directory.
    std::string directory = key;
    for (char& character : directory) {
        const bool safe = (character >= 'a' && character <= 'z') ||
                          (character >= 'A' && character <= 'Z') ||
                          (character >= '0' && character <= '9') || character == '-' ||
                          character == '_' || character == '.';
        if (!safe) {
            character = '_';
        }
    }
    // Chromium's profile manager expects one directory *directly* under the root for each profile, so the partition's
    // cache lives beside `Default` — not in a nested `partitions\` folder, which made the profile creation fail
    // ("Cannot create profile at path") and silently degrade to an in-memory cache (measured). The *separator* matters
    // just as much: CEF compares the cache path's parent against the profile root with an exact string match
    // (`cache_path_.DirName() == user_data_dir` in `chrome_browser_context.cc`), so a mixed or forward-slash child
    // fails that check and falls back to memory. `user_data_path` is already normalised to backslashes at the entry
    // point, so the child is written with the same separator and the check holds.
    const char separator = config.user_data_path.find('\\') != std::string::npos ? '\\' : '/';
    const std::string cache = config.user_data_path + std::string(1, separator) + directory;
    // A partition directory that exists but holds nothing is not a profile: Chromium refuses to adopt it ("Cannot
    // create profile at path") and silently falls back to memory, so an empty leftover — from a crash, or from an
    // earlier build of this very feature — is removed before the context is created. A directory with content is
    // never touched: that is the card's session.
    {
        std::error_code probe_error;
        const std::filesystem::path partition_dir(cache);
        if (std::filesystem::is_directory(partition_dir, probe_error)) {
            const auto entries =
                std::filesystem::directory_iterator(partition_dir, probe_error);
            if (!probe_error && entries == std::filesystem::directory_iterator()) {
                std::error_code remove_error;
                std::filesystem::remove(partition_dir, remove_error);
                Log("[engine-host] partition=%s empty_profile_removed=%d\n", key.c_str(),
                    remove_error ? 0 : 1);
            }
        }
    }
    CefString(&settings.cache_path).FromString(cache);
    settings.persist_session_cookies = true;
    CefRefPtr<PartitionContextHandler> handler(new PartitionContextHandler(key, config.proxy, config.proxy_bypass));
    CefRefPtr<CefRequestContext> context = CefRequestContext::CreateContext(settings, handler);
    if (!context) {
        Log("[engine-host] partition=%s context=FAILED\n", key.c_str());
        return nullptr;
    }
    // The preview scheme handler must be registered on *this* context too: a custom request context does not
    // inherit the global `CefRegisterSchemeHandlerFactory`, so a page in a partition could not load
    // `antikolt://blob/…` otherwise (measured: the preview generation check's image load failed).
    if (!g_previews_path.empty()) {
        context->RegisterSchemeHandlerFactory(kPreviewScheme, kPreviewHost,
                                              new PreviewHandlerFactory());
    }
    contexts[key] = context;
    handlers[key] = handler;
    if (created_or_reused != nullptr) {
        *created_or_reused = "created";
    }
    Log("[engine-host] partition=%s container=%s context=created cache=%s\n", key.c_str(),
        config.container.c_str(), CefString(&settings.cache_path).ToString().c_str());
    return context;
}

/// Put one container's proxy into a request context as a preference. Returns whether CEF accepted it, and fills
/// `error` when it did not.
///
/// One helper for the production path (`OnRequestContextInitialized`) and for `--partitiontest`: a test that wrote the
/// preference its own way would prove the test, not the product.
bool ApplyProxyPreference(CefRefPtr<CefRequestContext> context,
                          const std::string& proxy,
                          const std::string& bypass,
                          std::string* error) {
    if (!context) {
        if (error != nullptr) {
            *error = "no context";
        }
        return false;
    }
    CefRefPtr<CefDictionaryValue> proxy_dict = CefDictionaryValue::Create();
    const bool direct = proxy == "direct";
    proxy_dict->SetString("mode", direct ? "direct" : "fixed_servers");
    if (!direct) {
        proxy_dict->SetString("server", proxy);
        if (!bypass.empty()) {
            proxy_dict->SetString("bypass_list", bypass);
        }
    }
    CefRefPtr<CefValue> value = CefValue::Create();
    value->SetDictionary(proxy_dict);
    CefString cef_error;
    const bool applied = context->SetPreference("proxy", value, cef_error);
    if (error != nullptr) {
        *error = cef_error.ToString();
    }
    return applied;
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
      search: function (query) { return request('cards.search', { q: query }); },
      upsert: function (card) { return request('cards.upsert', { card: card }); },
      delete: function (id) { return request('cards.delete', { id: id }); }
    }
  };
  window.dispatchEvent(new Event('antikolt-bridge-ready'));
  return 'installed';
})();
)JS";

void EngineClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();
    // A second browser on this client is the login popup OnBeforePopup allowed: the card's surface keeps
    // `state_->browser`, the popup gets its own slot, so its close never takes the card down.
    if (state_->browser != nullptr && state_->browser != browser) {
        state_->popup_browser = browser;
        state_->popup_opened_at_ms = SteadyNowMs();
        return;
    }
    state_->browser = browser;
    state_->browser_created = true;
    // A `navigate` queued while the browser was still being created is applied now, on the surface the
    // shell meant it for.
    if (!state_->pending_navigation.empty()) {
        const std::string url = state_->pending_navigation;
        state_->pending_navigation.clear();
        browser->GetMainFrame()->LoadURL(url);
    }
    if (g_server_mode) {
        CefRefPtr<CefDictionaryValue> payload = CefDictionaryValue::Create();
        payload->SetInt("surface", static_cast<int>(state_->surface_id));
        StdioLink::Instance().Event("surface_created", payload);
    }
    if (webauthn_test_) {
        StartWebAuthnProbe(browser);
    }
    if (isolated_world_test_) {
        StartIsolatedWorldProbe(browser);
    }
    if (autofill_test_) {
        StartAutofillProbe(browser);
    }
    if (passkey_flow_test_) {
        StartPasskeyFlowProbe(browser);
    }
}

void EngineClient::StartWebAuthnProbe(CefRefPtr<CefBrowser> browser) {
    // F0 (ADR-027/023): does CEF 158's DevTools transport reach Chromium's `WebAuthn` domain, and does
    // `addVirtualAuthenticator` hand back an authenticator id? The domain must be `enable`d first — the
    // virtual authenticator environment is off until then, which is exactly the error the first probe hit.
    auto fail = [state = state_](const std::string& error) {
        std::printf("webauthn_authenticator=unsupported error=%s\n", error.c_str());
        std::fflush(stdout);
        state->webauthn_done.store(true);
        state->webauthn_ok.store(false);
    };

    CefRefPtr<DevToolsCall> enable(new DevToolsCall(
        "WebAuthn.enable", nullptr,
        [state = state_, browser, fail](CefRefPtr<CefBrowser>, const DevToolsReply& reply) {
            if (!reply.ok()) {
                fail(reply.error);
                return;
            }
            CefRefPtr<CefDictionaryValue> options = CefDictionaryValue::Create();
            options->SetString("protocol", "ctap2");
            options->SetString("transport", "internal");
            options->SetBool("hasResidentKey", true);
            options->SetBool("hasUserVerification", true);
            options->SetBool("isUserVerified", true);
            options->SetBool("automaticPresenceSimulation", true);
            CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
            params->SetDictionary("options", options);
            CefRefPtr<DevToolsCall> add(new DevToolsCall(
                "WebAuthn.addVirtualAuthenticator", params,
                [state](CefRefPtr<CefBrowser>, const DevToolsReply& reply) {
                    const bool ok = reply.ok() && !reply.String("authenticatorId").empty();
                    if (ok) {
                        std::printf("webauthn_authenticator=ok id=%s\n",
                                    reply.String("authenticatorId").c_str());
                    } else if (reply.ok()) {
                        std::printf("webauthn_authenticator=missing\n");
                    } else {
                        std::printf("webauthn_authenticator=unsupported error=%s\n",
                                    reply.error.c_str());
                    }
                    std::fflush(stdout);
                    state->webauthn_done.store(true);
                    state->webauthn_ok.store(ok);
                }));
            if (!add->Start(browser)) {
                fail("addVirtualAuthenticator submit failed");
            }
        }));
    if (!enable->Start(browser)) {
        fail("devtools unavailable");
    }
}

void EngineClient::StartIsolatedWorldProbe(CefRefPtr<CefBrowser> browser) {
    // ADR-053 (no dialog on the page) and safe autofill both need an isolated world: the page's main
    // world must never see the credential the shell fills. This spike proves CEF 158's CDP path can
    // create one, and that a probe set there really is invisible to the page.
    auto fail = [state = state_](const std::string& error) {
        std::printf("isolated_world=unsupported error=%s\n", error.c_str());
        std::fflush(stdout);
        state->isolated_world_done.store(true);
        state->isolated_world_ok.store(false);
    };

    // 1. The main frame's id, which `Page.createIsolatedWorld` needs.
    CefRefPtr<DevToolsCall> get_frame_tree(new DevToolsCall(
        "Page.getFrameTree", nullptr,
        [state = state_, browser, fail](CefRefPtr<CefBrowser>, const DevToolsReply& reply) {
            if (!reply.ok()) {
                fail(reply.error);
                return;
            }
            CefRefPtr<CefDictionaryValue> frame_tree = reply.Dictionary("frameTree");
            CefRefPtr<CefDictionaryValue> frame =
                frame_tree ? frame_tree->GetDictionary("frame") : nullptr;
            const std::string frame_id =
                frame ? frame->GetString("id").ToString() : std::string();
            if (frame_id.empty()) {
                fail("Page.getFrameTree gave no main frame id");
                return;
            }
            // 2. Create the isolated world (the autofill world).
            CefRefPtr<CefDictionaryValue> create_params = CefDictionaryValue::Create();
            create_params->SetString("frameId", frame_id);
            create_params->SetString("worldName", "antikolt-autofill");
            CefRefPtr<DevToolsCall> create_world(new DevToolsCall(
                "Page.createIsolatedWorld", create_params,
                [state, browser, fail](CefRefPtr<CefBrowser>,
                                       const DevToolsReply& create_reply) {
                    if (!create_reply.ok()) {
                        fail(create_reply.error);
                        return;
                    }
                    const int context_id = create_reply.Int("executionContextId", 0);
                    if (context_id == 0) {
                        fail("Page.createIsolatedWorld gave no executionContextId");
                        return;
                    }
                    // 3. Set a probe inside the isolated world.
                    CefRefPtr<CefDictionaryValue> set_params = CefDictionaryValue::Create();
                    set_params->SetString("expression",
                                          "window.__antikolt_isolated_probe = 'secret'; 'set'");
                    set_params->SetBool("returnByValue", true);
                    set_params->SetInt("contextId", context_id);
                    CefRefPtr<DevToolsCall> set_probe(new DevToolsCall(
                        "Runtime.evaluate", set_params,
                        [state, browser, fail](CefRefPtr<CefBrowser>,
                                               const DevToolsReply& set_reply) {
                            if (!set_reply.ok()) {
                                fail(set_reply.error);
                                return;
                            }
                            // 4. Read the same name from the MAIN world: it must be invisible.
                            CefRefPtr<CefDictionaryValue> get_params = CefDictionaryValue::Create();
                            get_params->SetString("expression",
                                                  "typeof window.__antikolt_isolated_probe");
                            get_params->SetBool("returnByValue", true);
                            CefRefPtr<DevToolsCall> get_probe(new DevToolsCall(
                                "Runtime.evaluate", get_params,
                                [state](CefRefPtr<CefBrowser>, const DevToolsReply& get_reply) {
                                    const std::string seen = get_reply.String("value");
                                    const bool invisible =
                                        get_reply.ok() && seen == "undefined";
                                    std::printf("isolated_world=%s main_sees=%s\n",
                                                invisible ? "ok" : "leaked", seen.c_str());
                                    std::fflush(stdout);
                                    state->isolated_world_done.store(true);
                                    state->isolated_world_ok.store(invisible);
                                }));
                            if (!get_probe->Start(browser)) {
                                fail("Runtime.evaluate submit failed");
                            }
                        }));
                    if (!set_probe->Start(browser)) {
                        fail("Runtime.evaluate submit failed");
                    }
                }));
            if (!create_world->Start(browser)) {
                fail("Page.createIsolatedWorld submit failed");
            }
        }));
    if (!get_frame_tree->Start(browser)) {
        fail("Page.getFrameTree submit failed");
    }
}

void EngineClient::StartAutofillProbe(CefRefPtr<CefBrowser> browser) {
    // The fill script runs in the isolated world: it sets the username/password on the shared DOM with
    // the native value setter (so React-style value tracking cannot swallow it) and marks the isolated
    // world with a probe. The verification runs in the MAIN world: the fields must be filled, and the
    // probe must be invisible — the credential must never appear as a page-visible variable.
    static const char* const kFillScript =
        "(function(){var u='alice';var p='hunter2';"
        "var s=Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value').set;"
        "var pw=document.querySelector('input[type=password]');if(!pw)return 'no-password';"
        "s.call(pw,p);pw.dispatchEvent(new Event('input',{bubbles:true}));"
        "pw.dispatchEvent(new Event('change',{bubbles:true}));"
        "var un=document.querySelector('input[name=username],input[autocomplete=username]');"
        "if(!un){var ins=document.querySelectorAll('input');for(var i=0;i<ins.length;i++)"
        "{if(ins[i]===pw&&i>0){un=ins[i-1];break;}}}"
        "if(un){s.call(un,u);un.dispatchEvent(new Event('input',{bubbles:true}));"
        "un.dispatchEvent(new Event('change',{bubbles:true}));}"
        "window.__antikolt_fill_probe='filled';return 'filled';})()";
    static const char* const kVerifyScript =
        "(function(){var pw=document.querySelector('input[type=password]');"
        "var un=document.querySelector('input[name=username]');"
        "var filled=!!pw&&pw.value==='hunter2'&&!!un&&un.value==='alice';"
        "var leak=typeof window.__antikolt_fill_probe!=='undefined';"
        "return (filled&&!leak)?'ok':'FAILED';})()";

    auto fail = [state = state_](const std::string& error) {
        std::printf("autofill=unsupported error=%s\n", error.c_str());
        std::fflush(stdout);
        state->autofill_done.store(true);
        state->autofill_ok.store(false);
    };

    CefRefPtr<DevToolsCall> get_frame_tree(new DevToolsCall(
        "Page.getFrameTree", nullptr,
        [state = state_, browser, fail](CefRefPtr<CefBrowser>, const DevToolsReply& reply) {
            if (!reply.ok()) {
                fail(reply.error);
                return;
            }
            CefRefPtr<CefDictionaryValue> frame_tree = reply.Dictionary("frameTree");
            CefRefPtr<CefDictionaryValue> frame =
                frame_tree ? frame_tree->GetDictionary("frame") : nullptr;
            const std::string frame_id =
                frame ? frame->GetString("id").ToString() : std::string();
            if (frame_id.empty()) {
                fail("Page.getFrameTree gave no main frame id");
                return;
            }
            CefRefPtr<CefDictionaryValue> create_params = CefDictionaryValue::Create();
            create_params->SetString("frameId", frame_id);
            create_params->SetString("worldName", "antikolt-autofill");
            CefRefPtr<DevToolsCall> create_world(new DevToolsCall(
                "Page.createIsolatedWorld", create_params,
                [state, browser, fail](CefRefPtr<CefBrowser>,
                                       const DevToolsReply& create_reply) {
                    if (!create_reply.ok()) {
                        fail(create_reply.error);
                        return;
                    }
                    const int context_id = create_reply.Int("executionContextId", 0);
                    if (context_id == 0) {
                        fail("Page.createIsolatedWorld gave no executionContextId");
                        return;
                    }
                    CefRefPtr<CefDictionaryValue> fill_params = CefDictionaryValue::Create();
                    fill_params->SetString("expression", kFillScript);
                    fill_params->SetBool("returnByValue", true);
                    fill_params->SetInt("contextId", context_id);
                    CefRefPtr<DevToolsCall> fill(new DevToolsCall(
                        "Runtime.evaluate", fill_params,
                        [state, browser, fail](CefRefPtr<CefBrowser>,
                                               const DevToolsReply& fill_reply) {
                            if (!fill_reply.ok()) {
                                fail(fill_reply.error);
                                return;
                            }
                            CefRefPtr<CefDictionaryValue> verify_params =
                                CefDictionaryValue::Create();
                            verify_params->SetString("expression", kVerifyScript);
                            verify_params->SetBool("returnByValue", true);
                            CefRefPtr<DevToolsCall> verify(new DevToolsCall(
                                "Runtime.evaluate", verify_params,
                                [state](CefRefPtr<CefBrowser>,
                                        const DevToolsReply& verify_reply) {
                                    const std::string verdict = verify_reply.String("value");
                                    const bool ok = verify_reply.ok() && verdict == "ok";
                                    std::printf("autofill=%s verdict=%s\n",
                                                ok ? "ok" : "FAILED", verdict.c_str());
                                    std::fflush(stdout);
                                    state->autofill_done.store(true);
                                    state->autofill_ok.store(ok);
                                }));
                            if (!verify->Start(browser)) {
                                fail("Runtime.evaluate submit failed");
                            }
                        }));
                    if (!fill->Start(browser)) {
                        fail("Runtime.evaluate submit failed");
                    }
                }));
            if (!create_world->Start(browser)) {
                fail("Page.createIsolatedWorld submit failed");
            }
        }));
    if (!get_frame_tree->Start(browser)) {
        fail("Page.getFrameTree submit failed");
    }
}

void EngineClient::StartPasskeyFlowProbe(CefRefPtr<CefBrowser> browser) {
    // The full passkey flow (ADR-023): enable WebAuthn and install a resident virtual authenticator.
    // The page's `navigator.credentials.create` runs in `StartPasskeyCreate` from `OnLoadEnd`, once the
    // execution context exists — running it from `OnAfterCreated` races the load and the target reports
    // "Inspected target navigated or closed".
    auto fail = [state = state_](const std::string& error) {
        std::printf("passkey_flow=unsupported error=%s\n", error.c_str());
        std::fflush(stdout);
        state->passkey_flow_done.store(true);
        state->passkey_flow_ok.store(false);
    };

    CefRefPtr<DevToolsCall> enable(new DevToolsCall(
        "WebAuthn.enable", nullptr,
        [this, browser, fail](CefRefPtr<CefBrowser>, const DevToolsReply& enable_reply) {
            if (!enable_reply.ok()) {
                fail(enable_reply.error);
                return;
            }
            CefRefPtr<CefDictionaryValue> options = CefDictionaryValue::Create();
            options->SetString("protocol", "ctap2");
            options->SetString("transport", "internal");
            options->SetBool("hasResidentKey", true);
            options->SetBool("hasUserVerification", true);
            options->SetBool("isUserVerified", true);
            options->SetBool("automaticPresenceSimulation", true);
            CefRefPtr<CefDictionaryValue> add_params = CefDictionaryValue::Create();
            add_params->SetDictionary("options", options);
            CefRefPtr<DevToolsCall> add(new DevToolsCall(
                "WebAuthn.addVirtualAuthenticator", add_params,
                [this, fail](CefRefPtr<CefBrowser>, const DevToolsReply& add_reply) {
                    if (!add_reply.ok()) {
                        fail(add_reply.error);
                        return;
                    }
                    passkey_authenticator_ = add_reply.String("authenticatorId");
                    if (passkey_authenticator_.empty()) {
                        fail("WebAuthn.addVirtualAuthenticator gave no authenticatorId");
                    }
                }));
            if (!add->Start(browser)) {
                fail("WebAuthn.addVirtualAuthenticator submit failed");
            }
        }));
    if (!enable->Start(browser)) {
        fail("WebAuthn.enable submit failed");
    }
}

void EngineClient::StartPasskeyCreate(CefRefPtr<CefBrowser> browser) {
    static const char* const kCreate =
        "(async function(){try{"
        "var c=await navigator.credentials.create({publicKey:{"
        "challenge:new Uint8Array([1,2,3,4,5,6,7,8]),"
        "rp:{name:'antikolt-test'},"
        "user:{id:new Uint8Array([1]),name:'u',displayName:'User'},"
        "pubKeyCredParams:[{type:'public-key',alg:-7},{type:'public-key',alg:-257}]"
        "}});"
        "return JSON.stringify({type:c.type,hasId:!!c.id});"
        "}catch(e){return 'ERROR:'+e.name+':'+e.message;}})()";
    auto fail = [state = state_](const std::string& error) {
        std::printf("passkey_flow=unsupported error=%s\n", error.c_str());
        std::fflush(stdout);
        state->passkey_flow_done.store(true);
        state->passkey_flow_ok.store(false);
    };
    CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
    params->SetString("expression", kCreate);
    params->SetBool("awaitPromise", true);
    params->SetBool("returnByValue", true);
    const std::string authenticator = passkey_authenticator_;
    CefRefPtr<DevToolsCall> create(new DevToolsCall(
        "Runtime.evaluate", params,
        [state = state_, authenticator](CefRefPtr<CefBrowser>, const DevToolsReply& create_reply) {
            const std::string result = create_reply.String("value");
            const bool ok =
                create_reply.ok() && result.find("\"type\":\"public-key\"") != std::string::npos;
            std::printf("passkey_flow=%s result=%s authenticator=%s\n", ok ? "ok" : "FAILED",
                        result.c_str(), authenticator.c_str());
            std::fflush(stdout);
            state->passkey_flow_done.store(true);
            state->passkey_flow_ok.store(ok);
        }));
    if (!create->Start(browser)) {
        fail("Runtime.evaluate submit failed");
    }
}

void EngineClient::StartPipProbe(CefRefPtr<CefBrowser> browser) {
    // Is the Document Picture-in-Picture API exposed in this runtime, and on a secure context? The
    // original ADR-094 probe measured `'documentPictureInPicture' in window` on a `data:` URL and got
    // `absent` — but a `data:` URL is an opaque, non-secure origin, so that measurement could not tell
    // "the runtime lacks Document-PiP" apart from "the API is gated behind a secure context". This
    // probe runs on `antikolt://` (registered SECURE, ADR-093) and reports both axes at once, so a
    // `secure:present` here proves Document-PiP exists without any views-architecture switch.
    CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
    params->SetString("expression",
                      "'documentPictureInPicture' in window ? "
                      "(window.isSecureContext ? 'secure:present' : 'insecure:present') : "
                      "(window.isSecureContext ? 'secure:absent' : 'insecure:absent')");
    params->SetBool("returnByValue", true);
    CefRefPtr<DevToolsCall> eval(new DevToolsCall(
        "Runtime.evaluate", params,
        [state = state_](CefRefPtr<CefBrowser>, const DevToolsReply& reply) {
            const std::string verdict = reply.String("value");
            const bool present = reply.ok() && verdict.find("present") != std::string::npos;
            const bool secure = verdict.find("insecure") == std::string::npos &&
                                verdict.find("secure") != std::string::npos;
            std::printf("pip_secure=%d pip_api=%s\n", secure ? 1 : 0,
                        present ? "present" : "absent");
            std::fflush(stdout);
            state->pip_done.store(true);
            state->pip_ok.store(present);
        }));
    if (!eval->Start(browser)) {
        std::printf("pip_api=unsupported error=Runtime.evaluate submit failed\n");
        std::fflush(stdout);
        state_->pip_done.store(true);
        state_->pip_ok.store(false);
    }
}

void EngineClient::StartPipWindowProbe(CefRefPtr<CefBrowser> browser) {
    // Does `documentPictureInPicture.requestWindow()` actually open a PiP window in CEF 158's windowed
    // mode? The call needs transient activation (a user gesture); CDP's `Runtime.evaluate` `userGesture`
    // flag grants it without a real click. Measured (ADR-096): the promise resolves (`pip_window=ok`,
    // the window has a document), but Document-PiP also spawns internal windows that never reach
    // OnAfterCreated on this client, and the shutdown then crashes with 0xC0000005 — the window
    // lifecycle is the remaining gap, not the API.
    CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
    params->SetString("expression",
                      "(async () => { try { "
                      "const w = await documentPictureInPicture.requestWindow({width:320,height:180}); "
                      "const opened = !!(w && w.document); "
                      "return 'ok:' + (opened ? 'doc' : 'win'); "
                      "} catch (e) { return 'rejected:' + e.name + ':' + e.message; } })()");
    params->SetBool("userGesture", true);
    params->SetBool("awaitPromise", true);
    params->SetBool("returnByValue", true);
    CefRefPtr<DevToolsCall> eval(new DevToolsCall(
        "Runtime.evaluate", params,
        [state = state_](CefRefPtr<CefBrowser>, const DevToolsReply& reply) {
            const std::string verdict = reply.String("value");
            const bool ok = reply.ok() && verdict.rfind("ok:", 0) == 0;
            std::printf("pip_window=%s\n", ok ? "ok" : verdict.c_str());
            std::fflush(stdout);
            state->pip_window_done.store(true);
            state->pip_window_ok.store(ok);
        }));
    if (!eval->Start(browser)) {
        std::printf("pip_window=unsupported error=Runtime.evaluate submit failed\n");
        std::fflush(stdout);
        state_->pip_window_done.store(true);
        state_->pip_window_ok.store(false);
    }
}

namespace {
/// A string rendered as a JSON string literal (quoted and escaped), so a credential can be embedded in
/// a script without its quotes or backslashes breaking the JavaScript.
std::string JsonQuote(const std::string& value) {
    CefRefPtr<CefValue> boxed = CefValue::Create();
    boxed->SetString(value);
    return CefWriteJSON(boxed, JSON_WRITER_DEFAULT).ToString();
}
}  // namespace

/// Fills a login form with a credential in the isolated world (ADR-053) and replies to `fill_credential`.
class FillCredential : public CefBaseRefCounted {
public:
    FillCredential(int64_t request_id, std::string username, std::string password)
        : request_id_(request_id), username_(std::move(username)), password_(std::move(password)) {}

    bool Start(CefRefPtr<CefBrowser> browser) {
        CefRefPtr<FillCredential> self(this);
        return Submit(browser, "Page.getFrameTree", nullptr,
                      [self](CefRefPtr<CefBrowser> browser, const DevToolsReply& reply) {
                          self->OnFrameTree(browser, reply);
                      });
    }

private:
    bool Submit(CefRefPtr<CefBrowser> browser, const char* method,
                CefRefPtr<CefDictionaryValue> params, DevToolsCall::Handler handler) {
        CefRefPtr<DevToolsCall> call(new DevToolsCall(method, params, std::move(handler)));
        return call->Start(browser);
    }

    std::string FillScript() const {
        return std::string("(function(){var u=") + JsonQuote(username_) + ";var p=" +
               JsonQuote(password_) +
               ";var s=Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value').set;"
               "var pw=document.querySelector('input[type=password]');if(!pw)return 'no-password';"
               "s.call(pw,p);pw.dispatchEvent(new Event('input',{bubbles:true}));"
               "pw.dispatchEvent(new Event('change',{bubbles:true}));"
               "var un=document.querySelector('input[name=username],input[autocomplete=username]');"
               "if(!un){var ins=document.querySelectorAll('input');for(var i=0;i<ins.length;i++)"
               "{if(ins[i]===pw&&i>0){un=ins[i-1];break;}}}"
               "if(un){s.call(un,u);un.dispatchEvent(new Event('input',{bubbles:true}));"
               "un.dispatchEvent(new Event('change',{bubbles:true}));}"
               "window.__antikolt_fill_probe='filled';return 'filled';})()";
    }

    std::string VerifyScript() const {
        return std::string("(function(){var pw=document.querySelector('input[type=password]');"
                           "var un=document.querySelector('input[name=username]');"
                           "var filled=!!pw&&pw.value===") +
               JsonQuote(password_) + "&&!!un&&un.value===" + JsonQuote(username_) +
               ";var leak=typeof window.__antikolt_fill_probe!=='undefined';"
               "return (filled&&!leak)?'ok':'FAILED';})()";
    }

    void OnFrameTree(CefRefPtr<CefBrowser> browser, const DevToolsReply& reply) {
        if (!reply.ok()) {
            Finish(reply.error);
            return;
        }
        CefRefPtr<CefDictionaryValue> frame_tree = reply.Dictionary("frameTree");
        CefRefPtr<CefDictionaryValue> frame =
            frame_tree ? frame_tree->GetDictionary("frame") : nullptr;
        frame_id_ = frame ? frame->GetString("id").ToString() : std::string();
        if (frame_id_.empty()) {
            Finish("Page.getFrameTree gave no main frame id");
            return;
        }
        CefRefPtr<FillCredential> self(this);
        CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
        params->SetString("frameId", frame_id_);
        params->SetString("worldName", "antikolt-autofill");
        if (!Submit(browser, "Page.createIsolatedWorld", params,
                    [self](CefRefPtr<CefBrowser> browser, const DevToolsReply& reply) {
                        self->OnWorldCreated(browser, reply);
                    })) {
            Finish("could not submit Page.createIsolatedWorld");
        }
    }

    void OnWorldCreated(CefRefPtr<CefBrowser> browser, const DevToolsReply& reply) {
        if (!reply.ok()) {
            Finish(reply.error);
            return;
        }
        const int context_id = reply.Int("executionContextId", 0);
        if (context_id == 0) {
            Finish("Page.createIsolatedWorld gave no executionContextId");
            return;
        }
        CefRefPtr<FillCredential> self(this);
        CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
        params->SetString("expression", FillScript());
        params->SetBool("returnByValue", true);
        params->SetInt("contextId", context_id);
        if (!Submit(browser, "Runtime.evaluate", params,
                    [self](CefRefPtr<CefBrowser> browser, const DevToolsReply& reply) {
                        self->OnFilled(browser, reply);
                    })) {
            Finish("could not submit the fill");
        }
    }

    void OnFilled(CefRefPtr<CefBrowser> browser, const DevToolsReply& reply) {
        if (!reply.ok()) {
            Finish(reply.error);
            return;
        }
        CefRefPtr<FillCredential> self(this);
        CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
        params->SetString("expression", VerifyScript());
        params->SetBool("returnByValue", true);
        if (!Submit(browser, "Runtime.evaluate", params,
                    [self](CefRefPtr<CefBrowser>, const DevToolsReply& reply) {
                        self->OnVerified(reply);
                    })) {
            Finish("could not submit the verification");
        }
    }

    void OnVerified(const DevToolsReply& reply) {
        const std::string verdict = reply.String("value");
        if (!reply.ok() || verdict != "ok") {
            Finish(verdict.empty() ? "autofill failed" : verdict);
            return;
        }
        CefRefPtr<CefDictionaryValue> result = CefDictionaryValue::Create();
        result->SetBool("filled", true);
        StdioLink::Instance().Reply(request_id_, result);
        Log("[engine-host] fill_credential filled the form in the isolated world\n");
    }

    void Finish(const std::string& error) {
        StdioLink::Instance().ReplyError(request_id_, "internal", error.c_str());
    }

    int64_t request_id_;
    std::string username_;
    std::string password_;
    std::string frame_id_;

    IMPLEMENT_REFCOUNTING(FillCredential);
    DISALLOW_COPY_AND_ASSIGN(FillCredential);
};

/// Install a resident CDP virtual authenticator (`WebAuthn.enable` → `addVirtualAuthenticator`) and
/// reply to `webauthn_setup` with its id, so a secure-context page can then create a credential.
class WebAuthnSetup : public CefBaseRefCounted {
public:
    explicit WebAuthnSetup(int64_t request_id) : request_id_(request_id) {}

    bool Start(CefRefPtr<CefBrowser> browser) {
        CefRefPtr<WebAuthnSetup> self(this);
        // WebAuthn requires the page to "have focus" (a transient user gesture); `Emulation` overrides
        // `document.hasFocus()` so a hidden probe surface is not refused with a NotAllowedError.
        CefRefPtr<CefDictionaryValue> focus = CefDictionaryValue::Create();
        focus->SetBool("enabled", true);
        CefRefPtr<DevToolsCall> set_focus(new DevToolsCall(
            "Emulation.setFocusEmulationEnabled", focus,
            [self](CefRefPtr<CefBrowser> browser, const DevToolsReply& focus_reply) {
                if (!focus_reply.ok()) {
                    self->Finish(focus_reply.error);
                    return;
                }
                // Bring the page to front so the view really has focus — WebAuthn's NOT_FOCUSED check
                // is the view's own focus flag, not the emulated `document.hasFocus()`.
                CefRefPtr<DevToolsCall> bring(new DevToolsCall(
                    "Page.bringToFront", nullptr,
                    [self](CefRefPtr<CefBrowser> browser, const DevToolsReply& bring_reply) {
                        if (!bring_reply.ok()) {
                            self->Finish(bring_reply.error);
                            return;
                        }
                        self->Enable(browser);
                    }));
                if (!bring->Start(browser)) {
                    self->Finish("Page.bringToFront submit failed");
                }
            }));
        return set_focus->Start(browser);
    }

private:
    void Enable(CefRefPtr<CefBrowser> browser) {
        CefRefPtr<WebAuthnSetup> self(this);
        CefRefPtr<DevToolsCall> enable(new DevToolsCall(
            "WebAuthn.enable", nullptr,
            [self](CefRefPtr<CefBrowser> browser, const DevToolsReply& enable_reply) {
                if (!enable_reply.ok()) {
                    self->Finish(enable_reply.error);
                    return;
                }
                CefRefPtr<CefDictionaryValue> options = CefDictionaryValue::Create();
                options->SetString("protocol", "ctap2");
                options->SetString("transport", "internal");
                options->SetBool("hasResidentKey", true);
                options->SetBool("hasUserVerification", true);
                options->SetBool("isUserVerified", true);
                options->SetBool("automaticPresenceSimulation", true);
                CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
                params->SetDictionary("options", options);
                CefRefPtr<DevToolsCall> add(new DevToolsCall(
                    "WebAuthn.addVirtualAuthenticator", params,
                    [self](CefRefPtr<CefBrowser>, const DevToolsReply& add_reply) {
                        if (!add_reply.ok()) {
                            self->Finish(add_reply.error);
                            return;
                        }
                        const std::string id = add_reply.String("authenticatorId");
                        if (id.empty()) {
                            self->Finish("WebAuthn.addVirtualAuthenticator gave no authenticatorId");
                            return;
                        }
                        CefRefPtr<CefDictionaryValue> result = CefDictionaryValue::Create();
                        result->SetString("authenticator_id", id);
                        StdioLink::Instance().Reply(self->request_id_, result);
                    }));
                if (!add->Start(browser)) {
                    self->Finish("WebAuthn.addVirtualAuthenticator submit failed");
                }
            }));
        if (!enable->Start(browser)) {
            self->Finish("WebAuthn.enable submit failed");
        }
    }

    void Finish(const std::string& error) {
        StdioLink::Instance().ReplyError(request_id_, "internal", error.c_str());
    }

    int64_t request_id_;

    IMPLEMENT_REFCOUNTING(WebAuthnSetup);
    DISALLOW_COPY_AND_ASSIGN(WebAuthnSetup);
};

/// Read the credentials a virtual authenticator holds (`WebAuthn.getCredentials`) and reply to
/// `webauthn_credentials` with their count — the export side of the passkey flow (ADR-023/093).
class WebAuthnCredentials : public CefBaseRefCounted {
public:
    WebAuthnCredentials(int64_t request_id, std::string authenticator_id)
        : request_id_(request_id), authenticator_id_(std::move(authenticator_id)) {}

    bool Start(CefRefPtr<CefBrowser> browser) {
        CefRefPtr<WebAuthnCredentials> self(this);
        CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
        params->SetString("authenticatorId", authenticator_id_);
        CefRefPtr<DevToolsCall> call(new DevToolsCall(
            "WebAuthn.getCredentials", params,
            [self](CefRefPtr<CefBrowser>, const DevToolsReply& reply) {
                if (!reply.ok() || !reply.result) {
                    self->Finish(reply.ok() ? "WebAuthn.getCredentials gave no result" : reply.error);
                    return;
                }
                CefRefPtr<CefListValue> credentials = reply.result->GetList("credentials");
                const int count = credentials ? static_cast<int>(credentials->GetSize()) : 0;
                CefRefPtr<CefDictionaryValue> result = CefDictionaryValue::Create();
                result->SetInt("count", count);
                StdioLink::Instance().Reply(self->request_id_, result);
            }));
        return call->Start(browser);
    }

private:
    void Finish(const std::string& error) {
        StdioLink::Instance().ReplyError(request_id_, "internal", error.c_str());
    }

    int64_t request_id_;
    std::string authenticator_id_;

    IMPLEMENT_REFCOUNTING(WebAuthnCredentials);
    DISALLOW_COPY_AND_ASSIGN(WebAuthnCredentials);
};

void EngineClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();
    // The popup closes on its own (login finished) or after the 60 s cap: only the popup goes, the card stays.
    if (browser == state_->popup_browser) {
        router_->OnBeforeClose(browser);
        state_->popup_browser = nullptr;
        state_->popup_opened_at_ms = 0;
        if (g_server_mode) {
            CefRefPtr<CefDictionaryValue> payload = CefDictionaryValue::Create();
            payload->SetInt("surface", static_cast<int>(state_->surface_id));
            StdioLink::Instance().Event("popup_closed", payload);
        }
        return;
    }
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

bool EngineClient::OnBeforePopup(CefRefPtr<CefBrowser> browser,
                                 CefRefPtr<CefFrame> frame,
                                 int popup_id,
                                 const CefString& target_url,
                                 const CefString& target_frame_name,
                                 CefLifeSpanHandler::WindowOpenDisposition target_disposition,
                                 bool user_gesture,
                                 const CefPopupFeatures& popupFeatures,
                                 CefWindowInfo& windowInfo,
                                 CefRefPtr<CefClient>& client,
                                 CefBrowserSettings& settings,
                                 CefRefPtr<CefDictionaryValue>& extra_info,
                                 bool* no_javascript_access) {
    CEF_REQUIRE_UI_THREAD();
    const bool allow = user_gesture;
    if (g_server_mode) {
        CefRefPtr<CefDictionaryValue> payload = CefDictionaryValue::Create();
        payload->SetInt("surface", static_cast<int>(state_->surface_id));
        payload->SetString("url", target_url.ToString());
        payload->SetString("disposition", allow ? "authenticated_window" : "blocked");
        StdioLink::Instance().Event("popup_requested", payload);
    }
    // Only a user-gesture popup is the login window (docs/HU/02 §8.5): it inherits the opener's client and
    // partition (CEF's default), so it shares the card's cookies — the whole point of an SSO flow. A page
    // firing popups on its own is refused, like an unasked-for permission.
    return !allow;
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

    // The passkey flow's `create` needs the page's execution context, so it waits for this load.
    if (passkey_flow_test_ && !passkey_authenticator_.empty() &&
        !state_->passkey_flow_done.load()) {
        StartPasskeyCreate(browser);
    }

    // The Document-PiP probes must run on the actually-loaded page, not the initial empty document:
    // `OnAfterCreated` fires before the first navigation commits, and `documentPictureInPicture` is
    // gated behind a secure context, so it can only be measured once the `antikolt://` page is up.
    if (pip_test_ && !state_->pip_done.load()) {
        StartPipProbe(browser);
    }
    if (pip_window_test_ && !state_->pip_window_done.load()) {
        StartPipWindowProbe(browser);
    }

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
    // Tell the shell (F7 "crash recovery"): the page is gone, and the shell must not leave a dead surface up.
    CefRefPtr<CefDictionaryValue> payload = CefDictionaryValue::Create();
    payload->SetInt("surface", static_cast<int>(state_->surface_id));
    payload->SetInt("status", static_cast<int>(status));
    payload->SetInt("code", error_code);
    StdioLink::Instance().Event("renderer_terminated", payload);
}

void EngineClient::OnFindResult(CefRefPtr<CefBrowser> browser,
                                int identifier,
                                int count,
                                const CefRect& selectionRect,
                                int activeMatchOrdinal,
                                bool finalUpdate) {
    CEF_REQUIRE_UI_THREAD();
    (void)browser;
    (void)identifier;
    (void)selectionRect;
    (void)finalUpdate;
    CefRefPtr<CefDictionaryValue> payload = CefDictionaryValue::Create();
    payload->SetInt("surface", static_cast<int>(state_->surface_id));
    payload->SetInt("count", count);
    payload->SetInt("active", activeMatchOrdinal);
    StdioLink::Instance().Event("find_result", payload);
}

void EngineClient::OnDownloadUpdated(CefRefPtr<CefBrowser> browser,
                                     CefRefPtr<CefDownloadItem> download_item,
                                     CefRefPtr<CefDownloadItemCallback> callback) {
    CEF_REQUIRE_UI_THREAD();
    (void)browser;
    (void)callback;
    // Progress updates fire many times; the shell only needs the terminal state.
    if (!download_item->IsComplete() && !download_item->IsCanceled()) {
        return;
    }
    CefRefPtr<CefDictionaryValue> payload = CefDictionaryValue::Create();
    payload->SetInt("surface", static_cast<int>(state_->surface_id));
    payload->SetBool("success", download_item->IsComplete());
    payload->SetString("path", download_item->GetFullPath().ToString());
    StdioLink::Instance().Event("download_finished", payload);
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

namespace {

/// The views spike's delegate: reports CHROME runtime style for both the browser view and the window.
/// Document-PiP is a Chrome-UI feature, so the spike must measure the CHROME style path (the cefclient
/// sample implements the same two methods on one object that doubles as both delegates).
class ViewsTestDelegate : public CefBrowserViewDelegate, public CefWindowDelegate {
public:
    ViewsTestDelegate() = default;
    cef_runtime_style_t GetBrowserRuntimeStyle() override { return CEF_RUNTIME_STYLE_CHROME; }
    cef_runtime_style_t GetWindowRuntimeStyle() override { return CEF_RUNTIME_STYLE_CHROME; }

private:
    IMPLEMENT_REFCOUNTING(ViewsTestDelegate);
    DISALLOW_COPY_AND_ASSIGN(ViewsTestDelegate);
};

}  // namespace

/// `--viewstest`: the first brick of the views switch (ADR-097). Create one browser through the views
/// framework (`CefBrowserView` + `CefWindow`, CHROME style) instead of the windowed `SetAsChild` path, and
/// report whether a page loads. It also measures that CHROME-style views reject a custom cache path — the
/// partition mechanism — which is the blocker the views switch must solve before it can replace the
/// windowed path.
int RunViewsTest(const HostConfig& config) {
    PreparePreviewDirectory(config);
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
    CefRefPtr<EngineClient> client_handler(new EngineClient(
        &state, /*quit_on_first_load=*/true, /*webauthn_test=*/false, /*isolated_world_test=*/false,
        /*autofill_test=*/false, /*passkey_flow_test=*/false, /*pip_test=*/false,
        /*pip_window_test=*/false, config.permissions));
    CefBrowserSettings browser_settings;
    CefRefPtr<ViewsTestDelegate> delegate(new ViewsTestDelegate());

    // Probe 1: CHROME-style views with the default profile — the first brick of the views switch.
    CefRefPtr<CefBrowserView> view = CefBrowserView::CreateBrowserView(
        client_handler, config.start_url, browser_settings, nullptr, nullptr, delegate);
    if (!view) {
        Log("[engine-host] CreateBrowserView failed (default profile)\n");
        std::printf("views_load=FAILED\n");
        std::fflush(stdout);
        CefShutdown();
        return 1;
    }

    // Probe 2: does CHROME-style views accept a custom cache path? That path is the partition mechanism
    // (ADR-009/071): one request context — and so one storage directory — per container. Measured: it
    // does NOT (CreateBrowserView returns null for a secondary-profile context, ADR-097).
    CefRequestContextSettings cache_settings;
    CefString(&cache_settings.cache_path).FromString(config.user_data_path + "\\partitionprobe");
    CefRefPtr<CefRequestContext> cache_context = CefRequestContext::CreateContext(cache_settings, nullptr);
    CefRefPtr<CefBrowserView> cache_view = CefBrowserView::CreateBrowserView(
        client_handler, config.start_url, browser_settings, nullptr, cache_context, delegate);
    std::printf("views_partition_context=%s\n", cache_view ? "accepted" : "rejected");
    std::fflush(stdout);

    CefRefPtr<CefWindow> window = CefWindow::CreateTopLevelWindow(delegate);
    if (!window) {
        Log("[engine-host] CreateTopLevelWindow failed\n");
        CefShutdown();
        return 1;
    }
    window->AddChildView(view);
    if (!config.hidden) {
        window->CenterWindow(CefSize(config.width, config.height));
        window->Show();
    }

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

    const bool ok = state.load_finished.load() && !state.load_failed.load() &&
                    !state.timed_out.load();
    std::printf("views_load=%s\n", ok ? "ok" : "FAILED");
    std::fflush(stdout);
    Log("load=%s\n", state.timed_out.load() ? "timeout" : (state.load_failed.load() ? "error" : "ok"));

    if (window && !window->IsClosed()) {
        window->Close();
    }
    DrainAndCloseBrowser(&state);
    CefShutdown();
    return ok ? 0 : 1;
}

int RunEngineHost(const HostConfig& config) {
    PreparePreviewDirectory(config);
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
        // The WebAuthn spike also waits for its CDP answer, so a fast load cannot end the process
        // before `webauthn_authenticator=…` is printed.
        while ((!state.load_finished.load() ||
                (config.webauthn_test && !state.webauthn_done.load()) ||
                (config.isolated_world_test && !state.isolated_world_done.load()) ||
                (config.autofill_test && !state.autofill_done.load()) ||
                (config.passkey_flow_test && !state.passkey_flow_done.load()) ||
                (config.pip_test && !state.pip_done.load()) ||
                (config.pip_window_test && !state.pip_window_done.load())) &&
               !state.load_failed.load() && !state.quit_requested.load()) {
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
    if (config.webauthn_test && !state.webauthn_ok.load()) {
        return 1;  // the spike must not report a green when the authenticator did not come back.
    }
    if (config.isolated_world_test && !state.isolated_world_ok.load()) {
        return 1;  // the spike must not report a green when the isolation did not hold.
    }
    if (config.autofill_test && !state.autofill_ok.load()) {
        return 1;  // the spike must not report a green when the form was not filled safely.
    }
    if (config.passkey_flow_test && !state.passkey_flow_ok.load()) {
        return 1;  // the spike must not report a green when no credential came back.
    }
    if (config.pip_test && !state.pip_ok.load()) {
        return 1;  // the probe must not report a green when the API is absent.
    }
    if (config.pip_window_test && !state.pip_window_ok.load()) {
        return 1;  // the spike must not report a green when the PiP window did not open.
    }
    return (config.quit_on_first_load && !loaded) ? 1 : 0;
}

/// Serve one command that names a live surface (`navigate` … `eval`).
///
/// Extracted from the server loop when one engine process learned to host two surfaces (ADR-041): with
/// more than one page alive, a command runs against the surface its `surface` field names, and a missing
/// one is answered `surface_not_found` — never against "whatever page happens to be there". Every early
/// exit is a `return`, because the caller's loop has to keep draining the command queue.
void HandleSurfaceCommand(StdioLink& link, HostState& state, const Command& command,
                          const HostConfig& config) {
    if (command.op == "navigate") {
        // The browser is created asynchronously: the surface is in the map, but `browser` is null until
        // `OnAfterCreated`. The shell sends `navigate` right after the `create_surface` reply, so it can
        // arrive in the same pump batch — queuing the URL and applying it on creation beats rejecting a
        // valid navigation (the shell ignores the reply, so a rejection silently parks the page on
        // `about:blank`).
        if (!state.browser_created.load()) {
            state.pending_navigation = command.url;
            link.Reply(command.id);
            return;
        }
        if (!state.browser) {
            link.ReplyError(command.id, "surface_not_found", "no live surface");
            return;
        }
        state.browser->GetMainFrame()->LoadURL(command.url);
        link.Reply(command.id);
    } else if (command.op == "snapshot") {
        if (!state.browser || !state.browser->GetMainFrame()) {
            link.ReplyError(command.id, "surface_not_found", "no live surface");
            return;
        }
        // `<user_data>/snapshots/surface-N-M.mhtml`: a real file the shell can verify, and later move
        // into the content-addressed blob store.
        std::error_code dir_error;
        std::filesystem::create_directories(config.user_data_path + "\\snapshots", dir_error);
        const std::string base = config.user_data_path + "\\snapshots\\surface-" +
                                 std::to_string(state.surface_id) + "-" +
                                 std::to_string(s_snapshot_counter++);
        // Preferred: full MHTML plus the runtime state, through the DevTools protocol.
        CefRefPtr<MhtmlCapture> capture(
            new MhtmlCapture(command.id, base, state.surface_id, state.final_url, state.title));
        if (!capture->Start(state.browser)) {
            // Fallback: DOM only. Less useful offline, but never a missing snapshot.
            Log("[engine-host] DevTools unavailable, falling back to a DOM-only snapshot\n");
            CefRefPtr<SnapshotVisitor> visitor(
                new SnapshotVisitor(command.id, base + ".html", state.final_url, state.title));
            state.browser->GetMainFrame()->GetSource(visitor);
        }
        // The reply is sent by the capture object when the payload arrives.
    } else if (command.op == "clear_origin_data") {
        if (!state.browser || !state.browser->GetMainFrame()) {
            link.ReplyError(command.id, "surface_not_found", "no live surface");
            return;
        }
        if (command.origin.empty()) {
            link.ReplyError(command.id, "invalid_argument", "clear_origin_data needs an origin");
            return;
        }
        CefRefPtr<ClearOriginCall> clear(
            new ClearOriginCall(command.id, command.origin, command.storages));
        if (!clear->Start(state.browser)) {
            Log("[engine-host] DevTools unavailable: cannot clear origin data\n");
            link.ReplyError(command.id, "not_supported",
                            "clearing origin data needs the DevTools protocol");
        }
        // The reply is sent by the call itself once the protocol answered.
    } else if (command.op == "scroll") {
        if (!state.browser || !state.browser->GetMainFrame()) {
            link.ReplyError(command.id, "surface_not_found", "no live surface");
            return;
        }
        CefRefPtr<ScrollCall> scroll(
            new ScrollCall(command.id, command.scroll_x, command.scroll_y, command.animate));
        if (!scroll->Start(state.browser)) {
            link.ReplyError(command.id, "not_supported", "DevTools is unavailable, cannot scroll");
        }
    } else if (command.op == "set_zoom") {
        if (!state.browser || !state.browser->GetMainFrame()) {
            link.ReplyError(command.id, "surface_not_found", "no live surface");
            return;
        }
        if (command.factor_percent < 25 || command.factor_percent > 500) {
            link.ReplyError(command.id, "invalid_argument",
                            "factor_percent must be between 25 and 500");
            return;
        }
        const int width = state.bounds_width > 0 ? state.bounds_width : 1280;
        const int height = state.bounds_height > 0 ? state.bounds_height : 800;
        CefRefPtr<ZoomCall> zoom(new ZoomCall(command.id, command.factor_percent, width, height));
        if (!zoom->Start(state.browser)) {
            link.ReplyError(command.id, "not_supported", "DevTools is unavailable, cannot zoom");
        }
    } else if (command.op == "set_media") {
        if (!state.browser || !state.browser->GetMainFrame()) {
            link.ReplyError(command.id, "surface_not_found", "no live surface");
            return;
        }
        if (command.media != "auto" && command.media != "dark" && command.media != "light") {
            link.ReplyError(command.id, "invalid_argument", "media must be auto, dark or light");
            return;
        }
        CefRefPtr<MediaEmulation> media(new MediaEmulation(command.id, command.media));
        if (!media->Start(state.browser)) {
            link.ReplyError(command.id, "not_supported",
                            "DevTools is unavailable, cannot emulate media");
        }
        // The reply is sent by the emulation object when DevTools confirms it.
    } else if (command.op == "set_forced_dark") {
        if (!state.browser || !state.browser->GetMainFrame()) {
            link.ReplyError(command.id, "surface_not_found", "no live surface");
            return;
        }
        // An empty mode means "leave the page alone" — the shell sends `off` explicitly, but a hand-written
        // request must not be able to inject a filter by omission.
        const std::string mode = command.dark_mode.empty() ? "off" : command.dark_mode;
        if (!ForcedDark::IsKnownMode(mode)) {
            link.ReplyError(command.id, "invalid_argument",
                            "mode must be off, invert, dim or warm");
            return;
        }
        CefRefPtr<ForcedDark> forced(new ForcedDark(command.id, mode));
        if (!forced->Start(state.browser)) {
            link.ReplyError(command.id, "not_supported",
                            "DevTools is unavailable, cannot force dark pages");
        }
        // The reply is sent by the object above when DevTools confirms it.
    } else if (command.op == "set_video_cap") {
        if (!state.browser || !state.browser->GetMainFrame()) {
            link.ReplyError(command.id, "surface_not_found", "no live surface");
            return;
        }
        if (command.video_max_height < 0) {
            link.ReplyError(command.id, "invalid_argument", "max_height must be >= 0");
            return;
        }
        CefRefPtr<VideoCap> cap(new VideoCap(command.id, command.video_max_height));
        if (!cap->Start(state.browser)) {
            link.ReplyError(command.id, "not_supported",
                            "DevTools is unavailable, cannot cap video height");
        }
        // The reply is sent by the object above when DevTools confirms it.
    } else if (command.op == "metrics") {
        // What one surface costs: the page's own numbers plus this process's working set. Only a live
        // surface can be measured; a hidden one has no document to count.
        if (!state.browser || !state.browser->GetMainFrame()) {
            link.ReplyError(command.id, "surface_not_found", "no live surface");
            return;
        }
        CefRefPtr<MetricsCapture> metrics(new MetricsCapture(command.id));
        if (!metrics->Start(state.browser)) {
            link.ReplyError(command.id, "not_supported",
                            "DevTools is unavailable, cannot measure");
        }
        // The reply is sent by the metrics object when the numbers arrive.
    } else if (command.op == "print_pdf") {
        // Save the page as a PDF (F7 "nyomtatás"): the compositor writes the file, the shell names it. Only a
        // live surface has a document to print.
        if (!state.browser || !state.browser->GetMainFrame()) {
            link.ReplyError(command.id, "surface_not_found", "no live surface");
            return;
        }
        if (command.path.empty()) {
            link.ReplyError(command.id, "invalid_argument", "path is required");
            return;
        }
        CefRefPtr<PdfPrint> print(new PdfPrint(command.id, command.path));
        print->Start(state.browser);
        // The reply is sent by the callback when the PDF is on disk.
    } else if (command.op == "find") {
        // Find-in-page (F7 "Ctrl+F"): start a search or advance to the next match. The result — the total match
        // count and the active one — comes back as a `find_result` event, not a reply, because CEF reports it
        // asynchronously.
        if (!state.browser || !state.browser->GetMainFrame()) {
            link.ReplyError(command.id, "surface_not_found", "no live surface");
            return;
        }
        if (command.find_text.empty()) {
            link.ReplyError(command.id, "invalid_argument", "text is required");
            return;
        }
        state.browser->GetHost()->Find(CefString(command.find_text), command.find_forward,
                                       /*match_case=*/false, command.find_next);
        link.Reply(command.id);
    } else if (command.op == "stop_find") {
        if (!state.browser) {
            link.ReplyError(command.id, "surface_not_found", "no live surface");
            return;
        }
        state.browser->GetHost()->StopFinding(true);
        link.Reply(command.id);
    } else if (command.op == "download") {
        // Start a file download (F7 "letöltés"): the URL is fetched and written to the default download directory.
        // The terminal state arrives as a `download_finished` event.
        if (!state.browser) {
            link.ReplyError(command.id, "surface_not_found", "no live surface");
            return;
        }
        if (command.url.empty()) {
            link.ReplyError(command.id, "invalid_argument", "url is required");
            return;
        }
        state.browser->GetHost()->StartDownload(CefString(command.url));
        link.Reply(command.id);
    } else if (command.op == "capture") {
        if (!state.browser || !state.browser->GetMainFrame()) {
            link.ReplyError(command.id, "surface_not_found", "no live surface");
            return;
        }
        if (command.format != "png" && command.format != "jpeg" && command.format != "webp") {
            link.ReplyError(command.id, "invalid_argument", "format must be png, jpeg or webp");
            return;
        }
        std::error_code dir_error;
        std::filesystem::create_directories(config.user_data_path + "\\screenshots", dir_error);
        const std::string base = config.user_data_path + "\\screenshots\\shot-" +
                                 std::to_string(state.surface_id) + "-" +
                                 std::to_string(s_screenshot_counter++);
        CefRefPtr<ScreenshotCapture> shot(new ScreenshotCapture(
            command.id, base, state.surface_id, command.format, command.quality));
        if (!shot->Start(state.browser)) {
            link.ReplyError(command.id, "not_supported", "DevTools is unavailable, cannot capture");
        }
        // The reply is sent by the capture object when the pixels arrive.
    } else if (command.op == "set_bounds") {
        if (!state.browser) {
            link.ReplyError(command.id, "surface_not_found", "no live surface");
            return;
        }
#ifdef _WIN32
        if (state.window_owned) {
            link.ReplyError(command.id, "not_supported",
                            "this surface is not embedded in a parent window");
            return;
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
            return;
        }
        state.browser->GetHost()->SetFocus(command.focused);
        link.Reply(command.id);
    } else if (command.op == "install_bridge") {
        // Same race as `navigate`: the shell sends `install_bridge` right after `create_surface`, while the
        // browser is still being created. The flag alone is enough when there is no browser yet — `OnLoadEnd`
        // injects the bridge on the next load, and `install_bridge` always precedes a navigation.
        state.bridge_enabled = true;
        if (!state.browser_created.load()) {
            link.Reply(command.id);
            return;
        }
        if (!state.browser || !state.browser->GetMainFrame()) {
            link.ReplyError(command.id, "surface_not_found", "no live surface");
            return;
        }
        state.browser->GetMainFrame()->ExecuteJavaScript(kBridgeScript, "", 0);
        link.Reply(command.id);
    } else if (command.op == "eval") {
        if (!state.browser) {
            link.ReplyError(command.id, "surface_not_found", "no live surface");
            return;
        }
        if (command.script.empty()) {
            link.ReplyError(command.id, "invalid_argument", "empty script");
            return;
        }
        CefRefPtr<EvalCapture> capture(new EvalCapture(command.id, command.script));
        if (!capture->Start(state.browser)) {
            link.ReplyError(command.id, "not_supported", "DevTools is unavailable, cannot evaluate");
        }
        // The reply is sent by EvalCapture when the value arrives.
    } else if (command.op == "fill_credential") {
        if (!state.browser) {
            link.ReplyError(command.id, "surface_not_found", "no live surface");
            return;
        }
        if (command.password.empty()) {
            link.ReplyError(command.id, "invalid_argument", "fill_credential needs a password");
            return;
        }
        CefRefPtr<FillCredential> fill(
            new FillCredential(command.id, command.username, command.password));
        if (!fill->Start(state.browser)) {
            link.ReplyError(command.id, "not_supported", "DevTools is unavailable, cannot autofill");
        }
        // The reply is sent by FillCredential when the form is filled (or the sequence failed).
    } else if (command.op == "webauthn_setup") {
        if (!state.browser) {
            link.ReplyError(command.id, "surface_not_found", "no live surface");
            return;
        }
        CefRefPtr<WebAuthnSetup> setup(new WebAuthnSetup(command.id));
        if (!setup->Start(state.browser)) {
            link.ReplyError(command.id, "not_supported", "DevTools is unavailable, cannot set up WebAuthn");
        }
        // The reply is sent by WebAuthnSetup once the authenticator is installed.
    } else if (command.op == "webauthn_credentials") {
        if (!state.browser) {
            link.ReplyError(command.id, "surface_not_found", "no live surface");
            return;
        }
        if (command.authenticator_id.empty()) {
            link.ReplyError(command.id, "invalid_argument", "webauthn_credentials needs authenticator_id");
            return;
        }
        CefRefPtr<WebAuthnCredentials> credentials(
            new WebAuthnCredentials(command.id, command.authenticator_id));
        if (!credentials->Start(state.browser)) {
            link.ReplyError(command.id, "not_supported", "DevTools is unavailable, cannot read WebAuthn");
        }
        // The reply is sent by WebAuthnCredentials once the credential list is read.
    } else {
        link.ReplyError(command.id, "not_supported",
                        "this milestone implements hello, ping, create_surface, navigate, eval, "
                        "fill_credential, capture, snapshot, print_pdf, find, download, set_bounds, "
                        "focus, install_bridge, bridge_reply, destroy_surface and shutdown");
    }
}

int RunEngineHostServer(const HostConfig& config) {
    PreparePreviewDirectory(config);
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

    // Every surface this process hosts, keyed by the id the shell knows (ADR-041 allows two: the page the
    // user is looking at and one short-lived background preview worker). The states are heap-allocated
    // because each browser's callbacks keep a pointer for that surface's whole lifetime, and a map node
    // never moves; `unique_ptr` makes ownership obvious when a surface goes away.
    std::map<int64_t, std::unique_ptr<HostState>> surfaces;
    bool shutdown_requested = false;

    /// Close one surface and forget it: the browser first (its callbacks need the state alive), then the
    /// window. The same call closes *either* surface, which is the whole point of keeping them apart.
    const auto close_surface = [](HostState* state) {
        DrainAndCloseBrowser(state);
        DestroyHostWindow(state);
    };

    while (!shutdown_requested && link.active()) {
        PumpWindowsMessages();
        CefDoMessageLoopWork();
        for (auto it = surfaces.begin(); it != surfaces.end();) {
            HostState* state = it->second.get();
#ifdef _WIN32
            ApplyPendingEmbeddedBounds(state);
#endif
            // A window the user closed (WM_CLOSE) takes only its own surface with it: another one may be
            // the page they are reading, so the process keeps running.
            if (state->close_requested.load()) {
                Log("[engine-host] surface %lld closed by its window\n",
                    static_cast<long long>(it->first));
                close_surface(state);
                it = surfaces.erase(it);
                continue;
            }
            // The login popup is short-lived by contract (docs/HU/02 §8.5): close it after 60 s. The flag
            // is zeroed first so the close is asked exactly once; OnBeforeClose then clears the browser.
            if (state->popup_browser && state->popup_opened_at_ms.load() != 0 &&
                SteadyNowMs() - state->popup_opened_at_ms.load() > kPopupTimeoutMs) {
                Log("[engine-host] login popup timed out (%lld ms)\n",
                    static_cast<long long>(kPopupTimeoutMs));
                state->popup_opened_at_ms = 0;
                state->popup_browser->GetHost()->CloseBrowser(false);
            }
            ++it;
        }

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
                if (surfaces.size() >= kMaxSurfaces) {
                    // The documented exception is one worker, not a free-for-all: a third live surface
                    // would be an unbounded memory promise (ADR-041).
                    link.ReplyError(command.id, "engine_busy",
                                    "at most two surfaces at a time (ADR-041)");
                    continue;
                }
                HostConfig surface_config = config;
                if (command.width > 0) {
                    surface_config.width = command.width;
                }
                if (command.height > 0) {
                    surface_config.height = command.height;
                }
                // The process-wide `--hidden` flag still applies to everything; a single surface may ask
                // for the same effect (the preview worker photographs a page and never shows a window).
                surface_config.hidden = config.hidden || command.hidden;
                // The surface's storage partition and its container's policy (ADR-009/ADR-071). The partition is the
                // storage key; the container is the policy key. Both travel per surface, so two cards of one container
                // share the proxy and the permission rules but never a cookie jar.
                if (!command.partition.empty()) {
                    surface_config.partition = command.partition;
                }
                surface_config.container = command.container;
                surface_config.proxy = command.proxy;
                surface_config.proxy_bypass = command.proxy_bypass;
                surface_config.permissions = command.permissions;
#ifdef _WIN32
                // Embedding is a window-handle feature: without a parent the host owns the window.
                surface_config.parent_window =
                    command.parent_window != 0
                        ? reinterpret_cast<void*>(static_cast<uintptr_t>(command.parent_window))
                        : nullptr;
#endif
                auto created = std::make_unique<HostState>();
                HostState& fresh = *created;
                fresh.surface_id = g_next_surface_id++;
                // A fresh surface starts at the origin of its parent, filling the rectangle the shell
                // asked for; Chromium creates its window from these units (device independent px).
                fresh.bounds_x = 0;
                fresh.bounds_y = 0;
                fresh.bounds_width = surface_config.width;
                fresh.bounds_height = surface_config.height;
                fresh.bounds_pending = surface_config.parent_window != nullptr;
                if (!CreateSurface(surface_config, &fresh, command.url.empty() ? "about:blank"
                                                                              : command.url)) {
                    link.ReplyError(command.id, "internal", "could not create the surface");
                    continue;
                }
                const int64_t surface_id = fresh.surface_id;
                surfaces.emplace(surface_id, std::move(created));
                CefRefPtr<CefDictionaryValue> result = CefDictionaryValue::Create();
                result->SetInt("surface", static_cast<int>(surface_id));
                link.Reply(command.id, result);
            } else if (command.op == "destroy_surface") {
                const auto found = surfaces.find(command.surface);
                if (found == surfaces.end()) {
                    link.ReplyError(command.id, "surface_not_found", "no live surface");
                    continue;
                }
                close_surface(found->second.get());
                surfaces.erase(found);
                link.Reply(command.id);
            } else if (command.op == "shutdown") {
                shutdown_requested = true;
                link.Reply(command.id);
            } else if (command.op == "bridge_reply") {
                // Protocol 2: the shell's answer to a page request. Requests belong to the surface that
                // asked, so they are found by scanning the live surfaces; only the grid installs a bridge,
                // and it is the surface that can hold a page promise.
                CefRefPtr<CefMessageRouterBrowserSide::Callback> waiting;
                for (auto& entry : surfaces) {
                    const auto pending = entry.second->pending_bridge.find(command.bridge_request);
                    if (pending == entry.second->pending_bridge.end()) {
                        continue;
                    }
                    waiting = pending->second;
                    entry.second->pending_bridge.erase(pending);
                    break;
                }
                // An answer with no waiting page is not a protocol violation — the page may have gone
                // away — so it is logged and acknowledged, never failed.
                if (!waiting) {
                    Log("[engine-host] bridge_reply for unknown request %lld\n",
                        static_cast<long long>(command.bridge_request));
                } else if (!command.error.empty()) {
                    waiting->Failure(-5, command.error);
                } else {
                    waiting->Success(command.json.empty() ? "{}" : command.json);
                }
                link.Reply(command.id);
            } else {
                // Everything that addresses a page resolves once, here, against the surface it names.
                const auto found = surfaces.find(command.surface);
                if (found == surfaces.end()) {
                    link.ReplyError(command.id, "surface_not_found", "no live surface");
                    continue;
                }
                HandleSurfaceCommand(link, *found->second, command, config);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(kPumpIntervalMs));
    }

    // Shutdown: every surface goes away before CEF does (CefShutdown with a live browser crashes).
    for (auto& entry : surfaces) {
        close_surface(entry.second.get());
    }
    surfaces.clear();
    Log("[engine-host] shutting down\n");
    CefShutdown();
    return 0;
}

/// `--partitiontest`: the isolation the product promises, measured without a window (ADR-071).
///
/// Three claims, all of them behavioural rather than bookkeeping:
///   * two partitions get two request contexts that do not share storage — this is what "two cards never share
///     cookies" (ADR-009) means, and the engine used to pass `nullptr` and share everything;
///   * the same partition gets its first context back, so a card that is closed and reopened keeps its login;
///   * a container's proxy is accepted by CEF as a request-context preference, and reads back.
/// The permission policy is checked on the policy object itself: the handler's decision is pure data, so the CEF
/// callback plumbing is the only part left for a page-level test.
int RunPartitionSelfTest(const HostConfig& config) {
    CefRefPtr<EngineApp> app(new EngineApp());
    if (!InitializeCef(config, app)) {
        std::printf("partition_contexts=FAILED (cef did not initialise)\n");
        std::fflush(stdout);
        return 1;
    }
    int failures = 0;
    auto check = [&failures](const char* name, bool ok) {
        std::printf("%s=%s\n", name, ok ? "ok" : "FAILED");
        std::fflush(stdout);
        if (!ok) {
            ++failures;
        }
    };

    HostConfig first = config;
    first.partition = "persist:probe-one";
    first.container = "c-one";
    first.proxy = "socks5://127.0.0.1:1080";
    first.proxy_bypass = "<local>";
    HostConfig second = config;
    second.partition = "persist:probe-two";
    second.container = "c-two";
    second.proxy = "direct";

    std::string state_one;
    std::string state_two;
    CefRefPtr<CefRequestContext> context_one = RequestContextForPartition(first, &state_one);
    CefRefPtr<CefRequestContext> context_two = RequestContextForPartition(second, &state_two);
    check("partition_contexts", context_one != nullptr && context_two != nullptr);
    if (!context_one || !context_two) {
        return 1;
    }
    check("partition_created", state_one == "created" && state_two == "created");
    check("partition_distinct",
          !context_one->IsSame(context_two) && !context_one->IsSharingWith(context_two));
    const std::string cache_one = context_one->GetCachePath().ToString();
    const std::string cache_two = context_two->GetCachePath().ToString();
    check("partition_cache_paths",
          !cache_one.empty() && !cache_two.empty() && cache_one != cache_two);
    std::printf("partition_cache=%s|%s\n", cache_one.c_str(), cache_two.c_str());

    // A card that is closed and reopened must not lose its cookies: the same key hands back the same context.
    std::string state_again;
    CefRefPtr<CefRequestContext> context_again = RequestContextForPartition(first, &state_again);
    check("partition_reused",
          context_again != nullptr && context_again->IsSame(context_one) &&
              state_again == "reused");

    // The permission policy: named grants only, everything else denied.
    const std::map<std::string, std::string> rules = {{"geolocation", "allow"},
                                                      {"notifications", "deny"}};
    check("permission_policy_allow", PermissionPolicy::Permits(rules, "geolocation"));
    check("permission_policy_deny", !PermissionPolicy::Permits(rules, "notifications"));
    check("permission_policy_unknown", !PermissionPolicy::Permits(rules, "clipboard"));
    check("permission_policy_unnamed", !PermissionPolicy::Permits(rules, ""));

    // The proxy is applied when the context is *initialised*, which CEF does when the first browser is created
    // against it — not when `CreateContext` returns. So the test creates one hidden surface per partition and asks
    // the context afterwards: the same path the product takes, not a shortcut around it.
    PreparePreviewDirectory(config);
    const char* probe_url =
        "data:text/html,%3Ctitle%3Apartition%3C/title%3E%3Ch1%3Eprobe%3C/h1%3E";
    std::unique_ptr<HostState> surface_one(new HostState());
    std::unique_ptr<HostState> surface_two(new HostState());
    const bool created_one = CreateSurface(first, surface_one.get(), probe_url);
    const bool created_two = CreateSurface(second, surface_two.get(), probe_url);
    check("partition_surfaces", created_one && created_two);
    if (created_one && created_two) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(kLoadTimeoutSeconds);
        while ((!surface_one->load_finished.load() || !surface_two->load_finished.load()) &&
               !surface_one->load_failed.load() && !surface_two->load_failed.load()) {
            PumpWindowsMessages();
            CefDoMessageLoopWork();
            std::this_thread::sleep_for(std::chrono::milliseconds(kPumpIntervalMs));
            if (std::chrono::steady_clock::now() > deadline) {
                break;
            }
        }
        check("partition_pages_loaded",
              surface_one->load_finished.load() && surface_two->load_finished.load());
    }
    auto proxy_of = [](CefRefPtr<CefRequestContext> context) {
        std::string mode;
        std::string server;
        CefRefPtr<CefValue> stored = context ? context->GetPreference("proxy") : nullptr;
        if (stored && stored->GetType() == VTYPE_DICTIONARY && stored->GetDictionary()) {
            mode = stored->GetDictionary()->GetString("mode").ToString();
            server = stored->GetDictionary()->GetString("server").ToString();
        }
        return std::make_pair(mode, server);
    };
    const auto proxy_one = proxy_of(context_one);
    const auto proxy_two = proxy_of(context_two);
    std::printf("proxy_one=%s|%s proxy_two=%s|%s\n", proxy_one.first.c_str(),
                proxy_one.second.c_str(), proxy_two.first.c_str(), proxy_two.second.c_str());
    check("proxy_fixed_applied",
          proxy_one.first == "fixed_servers" && proxy_one.second == first.proxy);
    check("proxy_direct_applied", proxy_two.first == "direct");

    // A profile that never reached the disk would forget every login: the cache directories must hold CEF's own files.
    auto populated = [](const std::string& path) {
        std::error_code listing_error;
        const auto entries =
            std::filesystem::directory_iterator(std::filesystem::path(path), listing_error);
        return !listing_error && entries != std::filesystem::directory_iterator();
    };
    check("partition_profiles_on_disk", populated(cache_one) && populated(cache_two));

    if (created_one) {
        DrainAndCloseBrowser(surface_one.get());
    }
    if (created_two) {
        DrainAndCloseBrowser(surface_two.get());
    }
    std::printf("permission_policy=%s\n", failures == 0 ? "ok" : "FAILED");
    std::fflush(stdout);
    // Every browser is gone, so the contexts can be released before CEF shuts down: the maps are file-scope exactly
    // so this release is possible before the library goes away.
    PartitionContexts().clear();
    PartitionHandlers().clear();
    CefShutdown();
    return failures == 0 ? 0 : 1;
}

}  // namespace antikolt
