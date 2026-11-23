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
#include "include/cef_permission_handler.h"
#include "include/cef_request_context.h"
#include "include/cef_request_context_handler.h"
#include "include/cef_request_handler.h"
#include "include/cef_scheme.h"
#include "include/wrapper/cef_message_router.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace antikolt {

/// Everything the host needs to know to start.
struct HostConfig {
    /// Absolute path of the process's profile/cache directory, in the canonical Windows form (backslashes).
    std::string user_data_path;
    /// Where `antikolt://blob/…` serves from. Empty means `<user_data_path>/previews`. The shell sets this
    /// explicitly so every engine process — the home engine and each container engine — serves the *same*
    /// shared preview directory, not its own profile's subdirectory.
    std::string previews_path;
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
    /// Run the WebAuthn spike: create a virtual authenticator through CDP and report whether it works
    /// (F0, ADR-027/023). Used by `--webauthntest`.
    bool webauthn_test = false;
    /// Run the isolated-world spike: create a CDP isolated world and prove the main world cannot see
    /// into it (the foundation for autofill, ADR-053). Used by `--isolatedworldtest`.
    bool isolated_world_test = false;
    /// Run the autofill spike: fill a login form in the isolated world and prove the credential never
    /// reaches the page's main world (ADR-053). Used by `--autofilltest`.
    bool autofill_test = false;
    /// Run the passkey flow: create a credential through `navigator.credentials.create` against the CDP
    /// virtual authenticator (the full F0 flow, ADR-023). Used by `--passkeyflowtest`.
    bool passkey_flow_test = false;
    /// Native window to embed the surface into; `nullptr` means the host creates a top-level window.
    ///
    /// This is how the one live card lives inside the Qt shell's window while the engine keeps its
    /// own process (ADR-004). The handle belongs to the shell process, so the host never destroys it.
    void* parent_window = nullptr;
    /// Placement inside `parent_window`, in physical pixels.
    int origin_x = 0;
    int origin_y = 0;
    /// The surface's storage partition (ADR-009): one request context — and so one cookie jar and cache directory —
    /// per partition. This is what makes "two cards never share cookies" true instead of a comment.
    std::string partition = "persist:default";
    /// The container the surface belongs to (ADR-071), for the log and for the policy that follows.
    std::string container;
    /// Proxy the container reaches the network through: empty = Chromium's own setting, `direct` = no proxy, or
    /// `scheme://host:port`. Composed and validated by the core, never guessed here.
    std::string proxy;
    /// Chromium bypass list, `;`-separated; only used with a fixed proxy.
    std::string proxy_bypass;
    /// What the container's pages may ask for: permission name → `allow` / `deny`. Anything not in the map is denied,
    /// because this host has no prompt surface: a silent grant would be worse than a silent refusal.
    std::map<std::string, std::string> permissions;
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
    /// One surface's window was asked to close (server mode): the pump loop destroys that surface and
    /// keeps the process alive, because another surface may be the one the user is looking at.
    std::atomic<bool> close_requested{false};
    /// Set once CEF has actually created the browser. CEF creates it asynchronously, so the pump
    /// loop must not treat "no browser yet" as "browser closed" (that bug exited immediately).
    std::atomic<bool> browser_created{false};
    /// The WebAuthn spike's CDP answer has arrived (set by the `DevToolsCall` handler).
    std::atomic<bool> webauthn_done{false};
    /// Whether that answer was a usable authenticator (drives the exit code).
    std::atomic<bool> webauthn_ok{false};
    /// The isolated-world spike's CDP answer has arrived.
    std::atomic<bool> isolated_world_done{false};
    /// Whether the main world really could not see the isolated world's probe (drives the exit code).
    std::atomic<bool> isolated_world_ok{false};
    /// The autofill spike's CDP answer has arrived.
    std::atomic<bool> autofill_done{false};
    /// Whether the form was filled in the isolated world and nothing leaked (drives the exit code).
    std::atomic<bool> autofill_ok{false};
    /// The passkey flow's CDP answer has arrived.
    std::atomic<bool> passkey_flow_done{false};
    /// Whether `navigator.credentials.create` produced a public-key credential (drives the exit code).
    std::atomic<bool> passkey_flow_ok{false};
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
    /// A `navigate` that arrived while the browser was still being created (CEF creates it
    /// asynchronously): the URL is remembered and applied the moment the browser exists, instead of the
    /// command being rejected with `surface_not_found` — the shell sends `navigate` immediately after
    /// `create_surface`, so without this the first navigation of a surface is a coin flip.
    std::string pending_navigation;
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
    /// The login popup this surface opened (docs/HU/02 §8.5): a second, short-lived browser on the same
    /// client and partition, so it inherits the card's cookies. Null when no popup is open.
    CefRefPtr<CefBrowser> popup_browser;
    /// Steady-clock milliseconds when the popup opened; 0 means "no popup" (or "already closing"). UI thread only.
    std::atomic<int64_t> popup_opened_at_ms{0};
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

    /// Register the `antikolt` scheme: previews are served from the profile by this host, not fetched.
    void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override;
    /// Install the preview handler once CEF is up (before any browser exists).
    void OnContextInitialized() override;

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

/// Applies one container's proxy to its request context (ADR-071).
///
/// CEF has no per-context proxy argument: the context owns a preference tree, and `proxy` is one of its entries. The
/// handler is the documented hook — `OnRequestContextInitialized` runs before the context is first used, which is the
/// only moment the setting is both allowed and effective.
class PartitionContextHandler : public CefRequestContextHandler {
public:
    PartitionContextHandler(std::string partition, std::string proxy, std::string bypass)
        : partition_(std::move(partition)), proxy_(std::move(proxy)), bypass_(std::move(bypass)) {}

    void OnRequestContextInitialized(CefRefPtr<CefRequestContext> context) override;

    /// What the host did with the policy, for the log and for `--partitiontest`.
    bool proxy_applied() const { return proxy_applied_; }
    const std::string& proxy_error() const { return proxy_error_; }

private:
    std::string partition_;
    std::string proxy_;
    std::string bypass_;
    bool proxy_applied_ = false;
    std::string proxy_error_;

    IMPLEMENT_REFCOUNTING(PartitionContextHandler);
    DISALLOW_COPY_AND_ASSIGN(PartitionContextHandler);
};

/// The per-surface permission policy (ADR-071).
///
/// Denies by default: there is no prompt UI yet, and a page that is silently granted the camera is worse than a page
/// that is refused a permission nobody asked for. A container opts in by naming the permission with `allow`.
class PermissionPolicy : public CefPermissionHandler {
public:
    PermissionPolicy() = default;
    explicit PermissionPolicy(std::map<std::string, std::string> rules)
        : rules_(std::move(rules)) {}

    bool OnRequestMediaAccessPermission(CefRefPtr<CefBrowser> browser,
                                        CefRefPtr<CefFrame> frame,
                                        const CefString& requesting_origin,
                                        uint32_t requested_permissions,
                                        CefRefPtr<CefMediaAccessCallback> callback) override;

    bool OnShowPermissionPrompt(CefRefPtr<CefBrowser> browser,
                                uint64_t prompt_id,
                                const CefString& requesting_origin,
                                uint32_t requested_permissions,
                                CefRefPtr<CefPermissionPromptCallback> callback) override;

    /// The policy key for one `cef_permission_request_types_t` bit; empty when the bit is unknown here.
    static const char* KeyForType(uint32_t type);

    /// Does this rule table grant `key`? The one place a policy means "yes", shared by the handler and the self test,
    /// so a test cannot pass with a rule the handler would not follow.
    static bool Permits(const std::map<std::string, std::string>& rules, const char* key);

private:
    std::map<std::string, std::string> rules_;

    IMPLEMENT_REFCOUNTING(PermissionPolicy);
    DISALLOW_COPY_AND_ASSIGN(PermissionPolicy);
};

/// One browser surface's client: lifespan, load, title callbacks and the JS bridge.
class EngineClient : public CefClient,
                     public CefLifeSpanHandler,
                     public CefLoadHandler,
                     public CefDisplayHandler,
                     public CefRequestHandler,
                     public CefFindHandler,
                     public CefDownloadHandler,
                     public CefMessageRouterBrowserSide::Handler {
public:
    EngineClient(HostState* state, bool quit_on_first_load, bool webauthn_test,
                 bool isolated_world_test, bool autofill_test, bool passkey_flow_test,
                 std::map<std::string, std::string> permissions = {});

    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
    CefRefPtr<CefPermissionHandler> GetPermissionHandler() override { return permission_; }
    CefRefPtr<CefFindHandler> GetFindHandler() override { return this; }
    CefRefPtr<CefDownloadHandler> GetDownloadHandler() override { return this; }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;
    /// A `window.open` / target=_blank request (CEF OnBeforePopup). The one documented second window is the
    /// login popup (docs/HU/02 §8.5): a real window in the opener's partition, allowed only on a user gesture,
    /// tracked separately so its close never takes the card's surface down. Anything else is refused.
    bool OnBeforePopup(CefRefPtr<CefBrowser> browser,
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
                       bool* no_javascript_access) override;
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
    /// Find-in-page result (F7 "Ctrl+F"): the total match count and the active (highlighted) match, sent as a
    /// `find_result` event so the shell can show "2 / 7".
    void OnFindResult(CefRefPtr<CefBrowser> browser,
                      int identifier,
                      int count,
                      const CefRect& selectionRect,
                      int activeMatchOrdinal,
                      bool finalUpdate) override;
    /// A download finished (F7 "letöltés"): the full path and whether it completed, sent as a `download_finished`
    /// event so the shell can report it.
    void OnDownloadUpdated(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefDownloadItem> download_item,
                           CefRefPtr<CefDownloadItemCallback> callback) override;

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
    bool webauthn_test_;
    bool isolated_world_test_;
    bool autofill_test_;
    bool passkey_flow_test_;
    /// The virtual authenticator's id, stored when the passkey flow enables it; the `create` step runs
    /// in `OnLoadEnd`, once the page's execution context exists.
    std::string passkey_authenticator_;
    CefRefPtr<CefMessageRouterBrowserSide> router_;
    CefRefPtr<PermissionPolicy> permission_;

    /// Submit the WebAuthn spike's `WebAuthn.addVirtualAuthenticator` call (F0, ADR-027/023). The
    /// handler reports `webauthn_authenticator=ok|unsupported` on stdout and flips `webauthn_done`.
    void StartWebAuthnProbe(CefRefPtr<CefBrowser> browser);
    /// Submit the isolated-world spike (ADR-053): `Page.getFrameTree` → `Page.createIsolatedWorld` →
    /// set a probe in the isolated world → read it from the main world. The handler reports
    /// `isolated_world=ok|leaked|unsupported` and flips `isolated_world_done`.
    void StartIsolatedWorldProbe(CefRefPtr<CefBrowser> browser);
    /// Submit the autofill spike (ADR-053): create an isolated world, fill a login form there, then
    /// read the form from the main world and check the credential did not leak. Reports
    /// `autofill=ok|FAILED|unsupported` and flips `autofill_done`.
    void StartAutofillProbe(CefRefPtr<CefBrowser> browser);
    /// Submit the passkey flow (ADR-023): `WebAuthn.enable` → `addVirtualAuthenticator` → the page's
    /// `navigator.credentials.create` through `Runtime.evaluate`. Reports
    /// `passkey_flow=ok|FAILED|unsupported` and flips `passkey_flow_done`.
    void StartPasskeyFlowProbe(CefRefPtr<CefBrowser> browser);
    /// The second half of the passkey flow, run from `OnLoadEnd`: evaluate `navigator.credentials.create`
    /// against the already-installed virtual authenticator.
    void StartPasskeyCreate(CefRefPtr<CefBrowser> browser);

    IMPLEMENT_REFCOUNTING(EngineClient);
    DISALLOW_COPY_AND_ASSIGN(EngineClient);
};

#ifdef _WIN32
/// Create the native parent window for a surface. Returns nullptr on failure.
HWND CreateHostWindow(const HostConfig& config);
#endif

/// Run the one-shot engine host. Returns the process exit code.
int RunEngineHost(const HostConfig& config);

/// The request context for one partition, created once per process and reused (ADR-071). UI thread only.
CefRefPtr<CefRequestContext> RequestContextForPartition(const HostConfig& config,
                                                       std::string* created_or_reused);

/// `--partitiontest`: prove — without a window — that two partitions get different request contexts with separate
/// storage, that a container's proxy reaches the context as a preference, and that the permission policy refuses what
/// it does not allow. Prints `partition_*` / `proxy_*` / `permission_policy_*` lines. Returns 0 when all pass.
int RunPartitionSelfTest(const HostConfig& config);

/// Run the protocol server on stdin/stdout (`--stdio`). Returns the process exit code.
int RunEngineHostServer(const HostConfig& config);

/// CEF re-launches this executable for its helper processes (`--type=renderer`, `--type=gpu-process`,
/// `--type=utility`). Those processes must reach `CefExecuteProcess` before anything else touches
/// their command line: the browser's own argument parser must never see them.
int RunCefSubprocess();

}  // namespace antikolt
