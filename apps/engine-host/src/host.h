// SPDX-License-Identifier: MPL-2.0
//
// Engine host internals: the CEF application, the client, and the native window that hosts one
// browser surface.
//
// Scope of this milestone (F0 spike, first half): initialise CEF, create a real native window with
// one browser surface, load a URL, report when the main frame finished loading. Rendering controls,
// the core protocol (crates/antikolt-ipc), OSR and the sandbox arrive in the next slices.

#pragma once

#include <atomic>
#include <string>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace antikolt {

/// Everything the host needs to know to start one surface.
struct HostConfig {
    /// Absolute path of the process's profile/cache directory.
    std::string user_data_path;
    /// The process executable, used as the CEF subprocess helper path.
    std::string exe_path;
    /// Page to load.
    std::string start_url = "about:blank";
    /// Window client size.
    int width = 1280;
    int height = 800;
    /// Do not show the window (used by --selftest so CI never pops a window).
    bool hidden = false;
    /// Stop as soon as the main frame has loaded: pump the loop ourselves instead of blocking
    /// in CefRunMessageLoop (a fast page can finish loading *before* that loop starts, and the quit
    /// signal would then be lost — this was a real hang, not a theory).
    bool quit_on_first_load = false;
};

/// State shared by the window procedure and the browser callbacks.
///
/// The flags are atomic because the window procedure (and the deadline check) read them from the
/// message-pump loop; everything else is only touched on the CEF UI thread.
struct HostState {
    std::atomic<bool> load_finished{false};
    std::atomic<bool> load_failed{false};
    std::atomic<bool> timed_out{false};
    int http_status = 0;
    std::string final_url;
    std::string title;
    CefRefPtr<CefBrowser> browser;
};

/// The CEF application object. Handles the subprocess entry point and command-line switches.
class EngineApp : public CefApp, public CefBrowserProcessHandler {
public:
    EngineApp() = default;

    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }

    void OnBeforeCommandLineProcessing(const CefString& process_type,
                                       CefRefPtr<CefCommandLine> command_line) override;

private:
    IMPLEMENT_REFCOUNTING(EngineApp);
    DISALLOW_COPY_AND_ASSIGN(EngineApp);
};

/// One browser surface's client: lifespan, load and title callbacks.
class EngineClient : public CefClient,
                     public CefLifeSpanHandler,
                     public CefLoadHandler,
                     public CefDisplayHandler {
public:
    EngineClient(HostState* state, bool quit_on_first_load)
        : state_(state), quit_on_first_load_(quit_on_first_load) {}

    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;
    void OnLoadEnd(CefRefPtr<CefBrowser> browser,
                   CefRefPtr<CefFrame> frame,
                   int httpStatusCode) override;
    void OnLoadError(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefFrame> frame,
                     ErrorCode errorCode,
                     const CefString& errorText,
                     const CefString& failedUrl) override;
    void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) override;

private:
    HostState* state_;
    bool quit_on_first_load_;

    IMPLEMENT_REFCOUNTING(EngineClient);
    DISALLOW_COPY_AND_ASSIGN(EngineClient);
};

#ifdef _WIN32
/// Create the native parent window for the surface. Returns nullptr on failure.
HWND CreateHostWindow(const HostConfig& config);
#endif

/// Run the engine host. Returns the process exit code.
int RunEngineHost(const HostConfig& config);

/// CEF re-launches this executable for its helper processes (`--type=renderer`, `--type=gpu-process`,
/// `--type=utility`). Those processes must reach `CefExecuteProcess` before anything else touches
/// their command line: the browser's own argument parser must never see them.
int RunCefSubprocess();

}  // namespace antikolt
