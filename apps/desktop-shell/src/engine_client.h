// SPDX-License-Identifier: MPL-2.0
//
// The shell's client for the engine host.
//
// The contract is the one in crates/antikolt-ipc and proto/README.md: length-prefixed JSON envelopes
// over the child process's standard IO. The Rust crate is the reference implementation and holds the
// tests; this is the same wire format on the Qt side, using Qt's own JSON so the shell needs no extra
// dependency.
//
// Rules:
//   * the engine host's stdout carries frames only, so nothing here writes to it;
//   * every request has a unique id and a timeout: a silent engine must not freeze the UI;
//   * an event that arrives before a reply is kept, not dropped.

#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>

class QProcess;
class QTimer;

/// What a surface carries besides its storage partition (ADR-071): the container it belongs to and the policy that comes
/// with it. The engine keeps the partition as the storage key (its own cookie jar per card) and this as the policy key,
/// so two cards of one container share the proxy and the permission rules but never a cookie.
struct SurfacePolicy {
    QString container;
    /// Empty = Chromium's own proxy setting, `direct` = no proxy, otherwise `scheme://host:port`.
    QString proxy;
    /// Chromium bypass list, `;`-separated; only meaningful with a fixed proxy.
    QString proxyBypass;
    /// Permission name → `allow`/`deny`. Omitted names are denied by the engine, because there is no prompt UI yet.
    QJsonObject permissions;
};

/// Build a card's policy from a container row (ADR-071): the proxy mode/host/port and the permission map the core
/// stores. A bare host is an HTTP proxy; a host that already names a scheme (`socks5://…`) is honoured as written.
SurfacePolicy surfacePolicyFromEntry(const QJsonObject& entry);

class EngineClient : public QObject {
    Q_OBJECT

public:
    using ReplyHandler = std::function<void(const QJsonObject& result, const QString& error)>;

    explicit EngineClient(QObject* parent = nullptr);
    ~EngineClient() override;

    /// Start the engine host in server mode. `profilePath` becomes its cache directory. `previewsPath`,
    /// when non-empty, is where this engine serves `antikolt://blob/…` from (the shared preview
    /// directory, so every engine process serves the same previews).
    bool start(const QString& executablePath, const QString& profilePath, bool hidden = false,
               const QString& previewsPath = QString());

    bool isRunning() const;

    /// Engine identity reported by the `welcome` handshake (empty until it arrives).
    QJsonObject engineInfo() const { return engine_info_; }

    /// Send `hello` and wait for `welcome` asynchronously.
    void handshake();

    /// Create a browser surface. `width`/`height` are **physical** pixels.
    ///
    /// `parentWindow` is a native window handle (a Qt `winId()`): when non-zero the surface is
    /// embedded into that window instead of getting a window of its own, which is how the live card
    /// appears inside the shell while the engine keeps its own process (ADR-004).
    ///
    /// `hidden` keeps a surface the shell creates for itself unseen: the background preview worker
    /// photographs a page and must never put a window over the grid (ADR-041).
    void createSurface(const QString& partition, int width, int height, int deviceScalePercent,
                       quintptr parentWindow, ReplyHandler handler, bool hidden = false,
                       const SurfacePolicy& policy = {});
    void navigate(qint64 surface, const QString& url, ReplyHandler handler = {});
    /// Place an embedded surface; all values are physical pixels relative to the parent's client area.
    void setBounds(qint64 surface, int x, int y, int width, int height,
                   ReplyHandler handler = {});
    /// Give the surface keyboard focus (or take it away).
    void focus(qint64 surface, bool focused, ReplyHandler handler = {});
    /// Evaluate JavaScript in the page's main world. The reply carries the value as a string.
    void evaluate(qint64 surface, const QString& script, ReplyHandler handler);
    /// Install the engine-side JS bridge (`window.antikoltNative`) on a surface.
    ///
    /// Only surfaces the shell enables this way may call back: the engine refuses everything else, so
    /// a random web page cannot reach the shell through the same channel as the home page.
    void installBridge(qint64 surface, ReplyHandler handler = {});
    void snapshot(qint64 surface, ReplyHandler handler);
    /// Capture one screenshot through Chromium's own compositor (`Page.captureScreenshot`).
    ///
    /// The reply carries the written file `path`, its `bytes`, and the encoded image's real
    /// `width`/`height` — the compositor's pixels, not a screen grab of another process's window.
    /// `format` is `png`, `jpeg` or `webp`; `quality` (0..100) only matters for the lossy ones.
    void capture(qint64 surface, const QString& format, int quality, ReplyHandler handler);
    /// Clear one origin's storage through the engine (`Storage.clearDataForOrigin`).
    ///
    /// The policy is the core's (ADR-033): the shell passes the origin and the storage list a
    /// `CleanupPlan` named, and the engine executes it in the same browser session the page lives in.
    /// The reply carries `{cleared: true, origin}`; a rejected call comes back as an error.
    void clearOriginData(qint64 surface, const QString& origin, const QStringList& storages,
                         ReplyHandler handler);
    /// Scroll the live page by CSS pixels (`scroll`), optionally animated.
    void scroll(qint64 surface, int x, int y, bool animate, ReplyHandler handler = {});
    /// Set the page zoom (`set_zoom`), 100 is the default; the viewport reflows (ADR-038).
    void setZoom(qint64 surface, int factorPercent, ReplyHandler handler = {});
    /// Emulate the page's `prefers-color-scheme` (`auto` clears it): `dark` or `light` (ADR-037).
    ///
    /// Only pages that support the query change — the browser tells them and their own CSS does the rest.
    void setMedia(qint64 surface, const QString& media, ReplyHandler handler = {});
    /// Darken the page itself (`set_forced_dark`, ADR-044/ADR-045): `off`, `invert`, `dim` or `warm`.
    ///
    /// The engine injects one named style element and replaces or removes exactly that, so a mode change never
    /// needs a reload. The user's switch, not a default — see `MainWindow::setPageDarkMode`.
    void setPageDarkMode(qint64 surface, const QString& mode, ReplyHandler handler = {});
    /// What one surface costs: `{dom_nodes, js_heap_bytes, engine_working_set_bytes}` (ADR-036).
    ///
    /// The numbers are measured by the engine — the page's own node count and JS heap through DevTools, and
    /// the engine host's working set — so the status bar can show a real figure instead of a guess.
    void metrics(qint64 surface, ReplyHandler handler);
    /// Save the live page as a PDF (`print_pdf`, F7 "nyomtatás"). The reply carries `{ok, path}` once the
    /// engine's compositor has written the file.
    void printPdf(qint64 surface, const QString& path, ReplyHandler handler);
    /// Find text in the live page (`find`, F7 "Ctrl+F"). The result arrives as `findResult`, not a reply,
    /// because CEF reports it asynchronously.
    void find(qint64 surface, const QString& text, bool findNext = false, bool forward = true,
              ReplyHandler handler = {});
    /// Clear the find-in-page highlight (`stop_find`).
    void stopFind(qint64 surface, ReplyHandler handler = {});
    void destroySurface(qint64 surface, ReplyHandler handler = {});
    /// Answer a page request (`request > 0`) the engine is holding open.
    ///
    /// Protocol 2: the page's `window.antikoltNative.request(...)` is a persistent `cefQuery`; the engine
    /// keeps the callback and forwards the question as a `bridge_call` event with a request number. The
    /// answer travels back here — `json` on success, `error` on failure — and the page's promise settles
    /// with it. A request the shell never answers would hang the page, so every path must reply.
    void bridgeReply(qint64 request, const QJsonObject& json, const QString& error = {});
    void shutdown();

signals:
    void engineReady(const QJsonObject& engineInfo);
    void loadFinished(qint64 surface, const QString& url, int httpStatus);
    void loadFailed(qint64 surface, const QString& url, const QString& reason);
    void surfaceCreated(qint64 surface);
    void surfaceClosed(qint64 surface);
    /// The page asked for a popup (CEF OnBeforePopup). `disposition` is `authenticated_window` (a user-gesture
    /// login window was opened, sharing the card's partition) or `blocked` (refused).
    void popupRequested(qint64 surface, const QString& url, const QString& disposition);
    /// A login popup closed (the flow finished, or the engine's 60 s cap fired).
    void popupClosed(qint64 surface);
    /// A find-in-page result arrived (F7 "Ctrl+F"): `count` total matches, `active` the highlighted one (1-based).
    void findResult(qint64 surface, int count, int active);
    /// The home page asked for something through the bridge (`open_card`, `freeze_card`, …).
    ///
    /// `request` is 0 for a one-way call (the page does not wait) and the engine's request number for a
    /// question — those must be answered with `bridgeReply`.
    void bridgeCall(qint64 surface, const QString& method, const QJsonObject& args, qint64 request);
    void failed(const QString& message);

private slots:
    void onReadyRead();
    void onProcessError();
    void onProcessFinished(int exitCode);

private:
    qint64 send(const QString& op, const QJsonObject& fields, ReplyHandler handler);
    void handleEnvelope(const QJsonObject& envelope);
    void handleEvent(const QJsonObject& event);
    void failPending(const QString& message);

    QProcess* process_ = nullptr;
    QByteArray buffer_;
    qint64 next_id_ = 1;
    QHash<qint64, ReplyHandler> pending_;
    QJsonObject engine_info_;
    /// Set in the destructor: queued replies must never reach a partly destroyed owner.
    bool destroying_ = false;
};
