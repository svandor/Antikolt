// SPDX-License-Identifier: MPL-2.0
//
// Engine host internals: the CEF application, the client, the native window that hosts one browser
// surface, and the protocol server that the shell talks to.
//
// Two modes share one pump loop:
//   * **one-shot** (`--selftest`, `--url`): create one surface, optionally quit when it loads;
//   * **server** (`--stdio`): no surface at start; commands create, navigate and destroy surfaces.
//
// The message loop is ours (external message pump) rather than CefRunMessageLoop, because a page can
// finish loading before CEF's blocking loop starts, and then the quit signal is lost. The same shape
// is what the Qt shell needs.

#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <string>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_request_handler.h"
#include "include/wrapper/cef_message_router.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace antikolt {

/// Everything the host needs to know to start.
struct HostConfig {
    /// Absolute path of the process's profile/cache directory.
    std::string user_data_path;
    /// The process executable, used as the CEF subprocess helper path.
    std::string exe_path;
    /// Page to load (one-shot mode).
    std::string start_url = "about:blank";
    /// Window client size.
    int width = 1280;
    int height = 800;
    /// Do not show the window (used by --selftest so CI never pops a window).
    bool hidden = false;
    /// Stop as soon as the main frame has loaded (selftest).
    bool quit_on_first_load = false;
    /// Native window to embed the surface into; `nullptr` means the host creates a top-level window.
    ///
    /// This is how the one live card lives inside the Qt shell's window while the engine keeps its
    /// own process (ADR-004). The handle belongs to the shell process, so the host never destroys it.
    void* parent_window = nullptr;
    /// Placement inside `parent_window`, in physical pixels.
    int origin_x = 0;
    int origin_y = 0;
};

/// State shared by the window procedure, the browser callbacks and the pump loop.
///
/// The flags are atomic because the window procedure and the reader thread touch them; the strings
/// are only written on the CEF UI thread.
struct HostState {
    std::atomic<bool> load_finished{false};
    std::atomic<bool> load_failed{false};
    std::atomic<bool> timed_out{false};
    /// The window was asked to close. The pump loop closes the browser and shuts CEF down.
    std::atomic<bool> quit_requested{false};
    /// Set once CEF has actually created the browser. CEF creates it asynchronously, so the pump
    /// loop must not treat "no browser yet" as "browser closed" (that bug exited immediately).
    std::atomic<bool> browser_created{false};
    int http_status = 0;
    std::string final_url;
    std::string title;
    CefRefPtr<CefBrowser> browser;
    /// Handle of the native window hosting the surface.
    void* window = nullptr;
    /// True when `window` was created by this process. An embedded surface lives in a window the
    /// shell owns, and destroying that window would take the whole UI down with it.
    bool window_owned = false;
    /// Placement the shell asked for, in device independent pixels, and whether it still has to be
    /// applied. The first `set_bounds` usually arrives *before* Chromium has created its child
    /// window, so the rectangle is remembered and applied as soon as the child exists; dropping it
    /// left the surface parked at a negative coordinate, invisible.
    int bounds_x = 0;
    int bounds_y = 0;
    int bounds_width = 0;
    int bounds_height = 0;
    bool bounds_pending = false;
    /// The shell installed the JS bridge on this surface. Off by default: a page may only reach the
    /// shell when the shell decided it should.
    std::atomic<bool> bridge_enabled{false};
    /// Page requests the shell has not answered yet (protocol 2, ADR-034): request number → the page's
    /// callback.
    ///
    /// A page call marked `request` is a *persistent* `cefQuery`; the engine keeps the callback and asks
    /// the shell with a `bridge_call` event, then answers the page from `bridge_reply`. A request the
    /// shell never answers would leave the page's promise pending forever, so a surface that goes away
    /// drops them (see `OnQueryCanceled`).
    std::map<int64_t, CefRefPtr<CefMessageRouterBrowserSide::Callback>> pending_bridge;
    /// The next request number for `pending_bridge`; 1-based, 0 means "one-way call".
    int64_t next_bridge_request = 1;
    int64_t surface_id = 0;
};

/// The CEF application object. Handles the subprocess entry point and command-line switches.
///
/// It carries both halves of the message router: the render process creates the JS side (`cefQuery`)
/// and the browser process serves it. Without the render half the injected bridge is a function that
/// does not exist, which on a page looks exactly like a broken home page.
class EngineApp : public CefApp,
                  public CefBrowserProcessHandler,
                  public CefRenderProcessHandler {
public:
    EngineApp() = default;

    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

    void OnBeforeCommandLineProcessing(const CefString& process_type,
                                       CefRefPtr<CefCommandLine> command_line) override;

    // ---- render process side --------------------------------------------------------------
    void OnWebKitInitialized() override;
    void OnContextCreated(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefV8Context> context) override;
    void OnContextReleased(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefV8Context> context) override;
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  CefProcessId source_process,
                                  CefRefPtr<CefProcessMessage> message) override;

private:
    CefRefPtr<CefMessageRouterRendererSide> router_renderer_;

    IMPLEMENT_REFCOUNTING(EngineApp);
    DISALLOW_COPY_AND_ASSIGN(EngineApp);
};

/// One browser surface's client: lifespan, load, title callbacks and the JS bridge.
class EngineClient : public CefClient,
                     public CefLifeSpanHandler,
                     public CefLoadHandler,
                     public CefDisplayHandler,
                     public CefRequestHandler,
                     public CefMessageRouterBrowserSide::Handler {
public:
    EngineClient(HostState* state, bool quit_on_first_load);

    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

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
    /// Page-side console output and JavaScript errors go to stderr.
    ///
    /// Without this a broken page is indistinguishable from a page that has not loaded: the grid
    /// rendered nothing for a whole slice because a module script was refused by CORS and the
    /// failure only existed inside the renderer's console.
    bool OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                          cef_log_severity_t level,
                          const CefString& message,
                          const CefString& source,
                          int line) override;
    void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
                                   TerminationStatus status,
                                   int error_code,
                                   const CefString& error_string) override;

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  CefProcessId source_process,
                                  CefRefPtr<CefProcessMessage> message) override;

    // ---- JS bridge ------------------------------------------------------------------------
    /// Serve one `cefQuery` from the page. Only surfaces the shell explicitly enabled can talk to us,
    /// otherwise any page that guessed the function name would get a capability it never asked for.
    bool OnQuery(CefRefPtr<CefBrowser> browser,
                 CefRefPtr<CefFrame> frame,
                 int64_t query_id,
                 const CefString& request,
                 bool persistent,
                 CefRefPtr<Callback> callback) override;
    void OnQueryCanceled(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefFrame> frame,
                         int64_t query_id) override;

private:
    HostState* state_;
    bool quit_on_first_load_;
    CefRefPtr<CefMessageRouterBrowserSide> router_;

    IMPLEMENT_REFCOUNTING(EngineClient);
    DISALLOW_COPY_AND_ASSIGN(EngineClient);
};

#ifdef _WIN32
/// Create the native parent window for a surface. Returns nullptr on failure.
HWND CreateHostWindow(const HostConfig& config);
#endif

/// Run the one-shot engine host. Returns the process exit code.
int RunEngineHost(const HostConfig& config);

/// Run the protocol server on stdin/stdout (`--stdio`). Returns the process exit code.
int RunEngineHostServer(const HostConfig& config);

/// CEF re-launches this executable for its helper processes (`--type=renderer`, `--type=gpu-process`,
/// `--type=utility`). Those processes must reach `CefExecuteProcess` before anything else touches
/// their command line: the browser's own argument parser must never see them.
int RunCefSubprocess();

}  // namespace antikolt
