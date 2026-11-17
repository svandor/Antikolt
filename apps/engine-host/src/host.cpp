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

    return CefInitialize(main_args, settings, app.get(), nullptr);
}

/// Create one native window with one browser surface. Returns false on failure.
bool CreateSurface(const HostConfig& config, HostState* state, const std::string& url) {
    HWND window = CreateHostWindow(config);
    if (window == nullptr) {
        return false;
    }
    state->window = window;
    g_state = state;

    RECT client = {};
    GetClientRect(window, &client);

    CefWindowInfo window_info;
    window_info.SetAsChild(window,
                           CefRect(0, 0, client.right - client.left, client.bottom - client.top));

    CefRefPtr<EngineClient> client_handler(new EngineClient(state, config.quit_on_first_load));
    CefBrowserSettings browser_settings;
    if (!CefBrowserHost::CreateBrowser(window_info, client_handler, url, browser_settings, nullptr,
                                       nullptr)) {
        Log("[engine-host] CreateBrowser failed\n");
        return false;
    }

    if (!config.hidden) {
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);
    }
    return true;
}

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

/// Destroy the native window that hosted a surface.
void DestroyHostWindow(HostState* state) {
    if (state->window != nullptr) {
        DestroyWindow(static_cast<HWND>(state->window));
        state->window = nullptr;
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

    HostState state;
    bool shutdown_requested = false;

    while (!shutdown_requested && link.active()) {
        PumpWindowsMessages();
        CefDoMessageLoopWork();

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
                state.surface_id = g_next_surface_id++;
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
                // `<user_data>/snapshots/surface-N-M.html`: a real file the shell can verify, and
                // later move into the content-addressed blob store.
                std::error_code dir_error;
                std::filesystem::create_directories(config.user_data_path + "\\snapshots",
                                                    dir_error);
                const std::string path = config.user_data_path + "\\snapshots\\surface-" +
                                         std::to_string(state.surface_id) + "-" +
                                         std::to_string(s_snapshot_counter++) + ".html";
                CefRefPtr<SnapshotVisitor> visitor(
                    new SnapshotVisitor(command.id, path, state.final_url, state.title));
                state.browser->GetMainFrame()->GetSource(visitor);
                // The reply is sent from SnapshotVisitor::Visit when the DOM arrives.
            } else if (command.op == "destroy_surface") {
                DrainAndCloseBrowser(&state);
                DestroyHostWindow(&state);
                link.Reply(command.id);
            } else if (command.op == "shutdown") {
                shutdown_requested = true;
                link.Reply(command.id);
            } else {
                link.ReplyError(command.id, "not_supported",
                                "this milestone implements hello, ping, create_surface, navigate, "
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
