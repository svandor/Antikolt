// SPDX-License-Identifier: MPL-2.0

#include "host.h"

#include <chrono>
#include <cstdio>
#include <thread>

#include "include/cef_command_line.h"
#include "include/wrapper/cef_helpers.h"

namespace antikolt {

namespace {

/// A page that never finishes loading must not hang the harness.
constexpr int kLoadTimeoutSeconds = 30;

constexpr wchar_t kWindowClass[] = L"AntikoltEngineHostWindow";

/// The single host instance of this process. CEF is a process-wide singleton, so one host per
/// process is the honest model; the window procedure needs it to translate WM_CLOSE into a request
/// that the pump loop can act on.
HostState* g_state = nullptr;

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
}

void EngineClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();
    state_->browser = browser;
    state_->browser_created = true;
}

void EngineClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();
    state_->browser = nullptr;
    if (!quit_on_first_load_) {
        CefQuitMessageLoop();
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
    std::printf("load_end url=%s status=%d\n", state_->final_url.c_str(), httpStatusCode);
    std::fflush(stdout);
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
    std::printf("load_error code=%d text=%s url=%s\n", static_cast<int>(errorCode),
                errorText.ToString().c_str(), state_->final_url.c_str());
    std::fflush(stdout);
}

void EngineClient::OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) {
    CEF_REQUIRE_UI_THREAD();
    state_->title = title.ToString();
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
            std::fprintf(stderr, "RegisterClassEx failed: %lu\n", error);
            return nullptr;
        }
    }

    // A window that is never shown still needs normal window styles: a WS_POPUP surface with no DWM
    // presence makes Chromium's GPU process fail to create a command buffer and it aborts.
    HWND window = CreateWindowExW(0, kWindowClass, L"Antikolt", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                  CW_USEDEFAULT, config.width, config.height, nullptr, nullptr,
                                  instance, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "CreateWindowEx failed: %lu\n", GetLastError());
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

    // Subprocess entry point: CEF re-launches this executable for renderer/GPU/utility processes.
    const int subprocess_code = CefExecuteProcess(main_args, app.get(), nullptr);
    if (subprocess_code >= 0) {
        return subprocess_code;
    }

    CefSettings settings;
    CefString(&settings.browser_subprocess_path).FromString(config.exe_path);
    // CEF 158 has no `user_data_path`: the per-profile cache directory is `cache_path`, and
    // `root_cache_path` is the fallback root for request contexts that do not set their own.
    CefString(&settings.cache_path).FromString(config.user_data_path);
    CefString(&settings.root_cache_path).FromString(config.user_data_path);
    CefString(&settings.log_file).FromString(config.user_data_path + "\\cef.log");
    settings.log_severity = LOGSEVERITY_WARNING;
    // We drive CEF from our own pump instead of letting it own a blocking loop. This is the same
    // integration shape the Qt shell will need, and it avoids the lost-quit race that CefQuitMessageLoop
    // produced when a page finished loading before the loop started.
    settings.external_message_pump = true;
    settings.multi_threaded_message_loop = false;
    settings.windowless_rendering_enabled = false;

    if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
        std::fprintf(stderr, "CefInitialize failed\n");
        return 1;
    }

    HostState state;
    HWND window = CreateHostWindow(config);
    if (window == nullptr) {
        CefShutdown();
        return 1;
    }

    RECT client = {};
    GetClientRect(window, &client);

    CefWindowInfo window_info;
    window_info.SetAsChild(window,
                           CefRect(0, 0, client.right - client.left, client.bottom - client.top));

    CefRefPtr<EngineClient> client_handler(new EngineClient(&state, config.quit_on_first_load));
    CefBrowserSettings browser_settings;
    if (!CefBrowserHost::CreateBrowser(window_info, client_handler, config.start_url,
                                       browser_settings, nullptr, nullptr)) {
        std::fprintf(stderr, "CreateBrowser failed\n");
        CefShutdown();
        return 1;
    }

    if (!config.hidden) {
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);
    }

    g_state = &state;

    if (config.quit_on_first_load) {
        // Deterministic pump instead of CefRunMessageLoop: the page may finish loading before that
        // loop starts (a data: URL does), and then the quit signal is lost and the process hangs.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(kLoadTimeoutSeconds);
        while (!state.load_finished.load() && !state.load_failed.load()) {
            CefDoMessageLoopWork();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (std::chrono::steady_clock::now() > deadline) {
                state.timed_out = true;
                break;
            }
        }
        // Let queued work drain so CefShutdown is not called in the middle of a navigation.
        for (int i = 0; i < 50; ++i) {
            CefDoMessageLoopWork();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

    } else {
        // Interactive: pump Windows messages ourselves and let CEF do its work each round. CEF does
        // not own a loop here, so closing the window is an orderly request (WM_CLOSE sets
        // quit_requested) instead of a CefQuitMessageLoop that the blocking loop never sees.
        MSG message = {};
        while (!state.quit_requested.load()) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            CefDoMessageLoopWork();
            // TODO(F1): CefBrowserProcessHandler::OnScheduleMessagePumpWork should wake this loop,
            // so an idle window costs no CPU instead of polling every 2 ms.
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (state.browser_created.load() && !state.browser) {
                break;  // the browser closed itself (OnBeforeClose)
            }
        }
    }

    // Both paths end here, and both need this: a live browser must be closed before CefShutdown.
    // Shutting down with a browser still attached crashes (access violation) once the renderer/GPU
    // teardown runs — that is the dialog the first interactive run produced.
    if (state.browser) {
        state.browser->GetHost()->CloseBrowser(true);
        for (int i = 0; i < 500 && state.browser; ++i) {
            CefDoMessageLoopWork();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    const bool timed_out = state.timed_out.load();
    const bool failed = state.load_failed.load();
    const bool loaded = state.load_finished.load();
    // "ok" must mean a page really loaded: an interactive run closed before any load reports "none".
    std::printf("load=%s\n", timed_out ? "timeout" : (failed ? "error" : (loaded ? "ok" : "none")));
    std::printf("url=%s\n", state.final_url.c_str());
    std::printf("status=%d\n", state.http_status);
    std::printf("title=%s\n", state.title.c_str());
    std::fflush(stdout);

    CefShutdown();
    if (timed_out || failed) {
        return 1;
    }
    // A measurement run exists to load one page; if it never did, that is a failure.
    return (config.quit_on_first_load && !loaded) ? 1 : 0;
}

}  // namespace antikolt
