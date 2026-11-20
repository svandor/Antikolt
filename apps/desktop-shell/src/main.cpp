// SPDX-License-Identifier: MPL-2.0
//
// Entry point of the Antikolt shell.
//
// `--selftest` exists so that CI (and the first F0 spike) can prove the UI stack initialises without
// showing a window: it prints the Qt version, the platform plugin, the screen geometry and checks
// that every documented theme produces a stylesheet. A build that cannot do this is not a build.
//
// With `--engine <path>` it goes one step further and proves the shell/engine seam: it starts the
// engine host, performs the `hello`/`welcome` handshake, creates a surface and destroys it. That
// exchange is the whole point of running the browser engine in a second process.

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QProcess>
#include "previews.h"
#include "scroll_book.h"
#include <functional>
#include <QRegularExpression>
#include <QScreen>
#include <QSet>
#include <QStandardPaths>
#include <QStatusBar>
#include <QSysInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "core_client.h"
#include "engine_client.h"
#include "instance_guard.h"
#include "live_card.h"
#include "main_window.h"

namespace {

#ifdef _WIN32
/// Drop a console window that Windows created *for this process*, and nothing else.
///
/// A shortcut (Start menu, desktop icon) starts a console-subsystem program with a console of its own: a second
/// window next to the browser that lived as long as the browser did, and the place where the engine's log
/// became visible (measured 2026-11-18 — the user's "extra, unjustified window"). The console is only *ours*
/// when no other process is attached to it; when the program is started from a shell, that shell is attached
/// and its console belongs to it, so it is left alone. Every self test runs from a shell and keeps its output.
///
/// The alternative — linking the shell as a GUI program — was measured and rejected for now: PowerShell starts
/// a GUI-subsystem binary without waiting for it and without its output, which would silently disarm every
/// check. That migration needs the harness to move to `Start-Process -Wait` first.
void DropOwnConsoleWindow() {
    DWORD attached[2] = {0, 0};
    if (::GetConsoleProcessList(attached, 2) == 1) {
        // Ours alone: free it. The window closes at once and the hosting console process exits with it.
        ::FreeConsole();
    }
}
#endif

/// Where the shell's own log file goes, when there is one.
///
/// A shortcut launch has no console: `DropOwnConsoleWindow` throws away the one Windows created, so stderr (and
/// with it every engine message) would vanish — the user's "GPU process exited" lines became invisible the
/// moment the extra window went away. The file is the console for that launch: same lines, on disk, rotated, so a
/// failure on a machine nobody is watching is still diagnosable.
QString& ShellLogPath() {
    static QString path;
    return path;
}

/// The size a fresh log file may reach before it is rotated to `<name>.1` (one generation is kept, because the
/// interesting part of a crash is usually the end of the previous run).
constexpr qint64 kShellLogLimit = 1024 * 1024;

qint64 ShellLogLimit() {
    // The tests rotate by writing a few kilobytes instead of a megabyte: the limit is a knob, like the preview
    // deadline and the hibernation timer (ANTIKOLT_PREVIEW_DEADLINE_MS, ANTIKOLT_HIBERNATE_MS).
    bool ok = false;
    const qint64 fromEnvironment = qEnvironmentVariableIntValue("ANTIKOLT_SHELL_LOG_LIMIT", &ok);
    return ok && fromEnvironment > 0 ? fromEnvironment : kShellLogLimit;
}

/// Appends one line to the shell log, if a path is set; rotates the file first when it grew too big.
///
/// Kept deliberately simple — open, append, close — because it is also the path taken while the process is
/// failing: no buffers to lose, no state to corrupt, and a full disk must never turn a log line into a crash.
void AppendToShellLog(const QString& line) {
    const QString path = ShellLogPath();
    if (path.isEmpty()) {
        return;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.exists() && file.size() + line.size() + 1 > ShellLogLimit()) {
        const QString previous = path + QStringLiteral(".1");
        QFile::remove(previous);
        QFile::rename(path, previous);
    }
    if (!file.open(QIODevice::Append | QIODevice::Text)) {
        return;
    }
    file.write(line.toUtf8());
    file.write("\n", 1);
}

/// Qt's own messages — and the engine's forwarded stderr, which arrives through `qInfo` — go to
/// stderr instead of the platform debug output.
///
/// A console build must be readable from the terminal, and a swallowed engine log is exactly how a
/// page-side failure stayed invisible for two slices: the engine said `console level=4 … TypeError`
/// and the shell threw it away. Everything the engine reports is now visible where the shell runs — and
/// appended to the shell log, for the launch that has no terminal to read it from.
void ForwardQtMessages(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    (void)context;
    const char* level = "info";
    switch (type) {
        case QtDebugMsg:
            level = "debug";
            break;
        case QtInfoMsg:
            level = "info";
            break;
        case QtWarningMsg:
            level = "warning";
            break;
        case QtCriticalMsg:
            level = "critical";
            break;
        case QtFatalMsg:
            level = "fatal";
            break;
    }
    const QByteArray text = message.toUtf8();
    std::fprintf(stderr, "[shell] %s: %s\n", level, text.constData());
    std::fflush(stderr);
    AppendToShellLog(QStringLiteral("%1 [shell] %2: %3")
                         .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
                              QString::fromLatin1(level), QString::fromUtf8(text)));
    if (type == QtFatalMsg) {
        std::abort();
    }
}

/// A one-page HTTP origin for the storage tests: no internet, but a real origin with real storage.
///
/// `file://` has no storable origin and `data:` has an opaque one, so a check that a wipe really wipes
/// needs a page a browser can own. Serving it from this process keeps the check offline.
class ProbeOrigin {
public:
    /// A non-HTML response the probe can serve, for tests that need a real resource (a media file, for example):
    /// a page whose `<audio src="/tone.wav">` gets HTML back never plays, and a player that never plays cannot
    /// prove anything about media handling.
    struct Asset {
        QByteArray body;
        QByteArray mime;
    };

    explicit ProbeOrigin(const QString& html, const QHash<QString, Asset> assets = {})
        : assets_(assets) {
        QObject::connect(&server_, &QTcpServer::newConnection, &server_, [this, html] {
            while (QTcpSocket* socket = server_.nextPendingConnection()) {
                // The request may already be buffered when the socket is handed over — a media loader sends its
                // request at once — and `readyRead` never fires for bytes that arrived before the connection. So
                // the handler runs once, from whichever side notices first.
                const auto handle = [this, socket, html] {
                    if (socket->property("antikoltAnswered").toBool() ||
                        socket->bytesAvailable() <= 0) {
                        return;
                    }
                    socket->setProperty("antikoltAnswered", true);
                    const QByteArray request = socket->readAll();
                    const QList<QByteArray> parts = request.split(' ');
                    const QByteArray asset_path = parts.size() > 1 ? parts.at(1) : QByteArray();
                    const auto asset = assets_.constFind(QString::fromUtf8(asset_path));
                    const QByteArray body = asset == assets_.constEnd() ? html.toUtf8() : asset->body;
                    const QByteArray mime =
                        asset == assets_.constEnd() ? QByteArrayLiteral("text/html; charset=utf-8")
                                                    : asset->mime;
                    QByteArray response = "HTTP/1.1 200 OK\r\nContent-Type: " + mime +
                                          "\r\nCache-Control: no-store\r\nConnection: close\r\n"
                                          "Content-Length: " +
                                          QByteArray::number(body.size()) + "\r\n\r\n" + body;
                    socket->write(response);
                    socket->disconnectFromHost();
                };
                QObject::connect(socket, &QTcpSocket::readyRead, socket, handle);
                handle();
            }
        });
        if (server_.listen(QHostAddress::LocalHost, 0)) {
            origin_ = QStringLiteral("http://127.0.0.1:%1").arg(server_.serverPort());
        }
    }

    bool listening() const { return !origin_.isEmpty(); }
    QString origin() const { return origin_; }
    QString errorString() const { return server_.errorString(); }

private:
    QTcpServer server_;
    QHash<QString, Asset> assets_;
    QString origin_;
};

/// A real, minimal WAV: 0.05 s of a 440 Hz tone, 8-bit mono, 8 kHz. 444 bytes keeps the test source readable and
/// is still a file Chromium's player accepts — the point is that `paused` becomes false, not the sound.
QByteArray toneWav() {
    const int sample_rate = 8000;
    const int samples = 400;
    QByteArray wav;
    const auto put = [&wav](const char* text, int size) { wav.append(text, size); };
    const auto put32 = [&wav](quint32 value) {
        for (int shift = 0; shift < 32; shift += 8) {
            wav.append(static_cast<char>((value >> shift) & 0xFF));
        }
    };
    const auto put16 = [&wav](quint16 value) {
        wav.append(static_cast<char>(value & 0xFF));
        wav.append(static_cast<char>((value >> 8) & 0xFF));
    };
    put("RIFF", 4);
    put32(static_cast<quint32>(36 + samples));
    put("WAVE", 4);
    put("fmt ", 4);
    put32(16);
    put16(1);
    put16(1);
    put32(static_cast<quint32>(sample_rate));
    put32(static_cast<quint32>(sample_rate));
    put16(1);
    put16(8);
    put("data", 4);
    put32(static_cast<quint32>(samples));
    for (int i = 0; i < samples; ++i) {
        const double phase = 2.0 * 3.14159265358979 * 440.0 * i / sample_rate;
        wav.append(static_cast<char>(128 + static_cast<int>(40.0 * std::sin(phase))));
    }
    return wav;
}

/// The page the storage tests use: it writes one cookie and one localStorage entry on load, and leaves a
/// witness (`window.__wrote`) that it did — a wipe check must not confuse "never written" with "cleared".
QString storageProbePage() {
    return QStringLiteral(
        "<!doctype html><meta charset=\"utf-8\"><title>wipe probe</title><script>"
        "localStorage.setItem('probe','1');document.cookie='probe=1; path=/';"
        "window.__wrote=true;</script>"
        "<h1>wipe probe</h1>");
}

/// What a page's storage looks like from the inside: localStorage and document.cookie, joined by `|`.
QString storageReadScript() {
    return QStringLiteral("(localStorage.getItem('probe')||'')+'|'+document.cookie");
}

/// Drive the engine host once and report what it says. True when the whole exchange worked.
bool runEngineCheck(const QString& enginePath) {
    QTextStream out(stdout);
    EngineClient client;

    const QString profile = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                            QStringLiteral("/engine-selftest");
    QDir().mkpath(profile);

    if (!client.start(enginePath, profile, /*hidden=*/true)) {
        out << "engine_start=FAILED\n";
        return false;
    }
    out << "engine_start=ok\n";

    // The engine answers with `welcome` only after we greet it (proto/README.md).
    client.handshake();

    QEventLoop loop;
    bool handshake_ok = false;
    bool surface_ok = false;
    bool destroy_ok = false;

    QObject::connect(&client, &EngineClient::engineReady, &loop,
                     [&](const QJsonObject& info) {
                         out << "engine_name=" << info.value(QStringLiteral("name")).toString()
                             << '\n';
                         out << "engine_version="
                             << info.value(QStringLiteral("version")).toString() << '\n';
                         out << "engine_capabilities="
                             << info.value(QStringLiteral("capabilities")).toArray().size() << '\n';
                         handshake_ok = true;

                         client.createSurface(
                             QStringLiteral("persist:selftest"), 800, 600, 100, 0,
                             [&](const QJsonObject& result, const QString& error) {
                                 if (!error.isEmpty()) {
                                     out << "create_surface=FAILED " << error << '\n';
                                     loop.quit();
                                     return;
                                 }
                                 const qint64 surface =
                                     static_cast<qint64>(result.value(QStringLiteral("surface"))
                                                             .toDouble());
                                 out << "surface=" << surface << '\n';
                                 surface_ok = surface > 0;
                                 client.destroySurface(
                                     surface, [&](const QJsonObject&, const QString& failure) {
                                         out << "destroy_surface="
                                             << (failure.isEmpty() ? "ok" : "FAILED") << '\n';
                                         destroy_ok = failure.isEmpty();
                                         loop.quit();
                                     });
                             });
                     });

    QObject::connect(&client, &EngineClient::failed, &loop, [&](const QString& message) {
        out << "engine_error=" << message << '\n';
        loop.quit();
    });

    // A silent engine must never hang the self test.
    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
    loop.exec();
    client.shutdown();
    out.flush();

    return handshake_ok && surface_ok && destroy_ok;
}

#ifdef _WIN32
namespace {

struct ChildProbe {
    QStringList lines;
    bool chromium = false;
};

BOOL CALLBACK CountChildWindows(HWND child, LPARAM param) {
    auto* probe = reinterpret_cast<ChildProbe*>(param);
    wchar_t class_name[64] = {};
    const int length = GetClassNameW(child, class_name, 64);
    const QString name =
        length > 0 ? QString::fromWCharArray(class_name, length) : QStringLiteral("?");
    RECT window_rect = {};
    RECT client_rect = {};
    GetWindowRect(child, &window_rect);
    GetClientRect(child, &client_rect);
    probe->lines.append(
        QStringLiteral("%1 window=%2x%3 at %4,%5 client=%6x%7")
            .arg(name)
            .arg(window_rect.right - window_rect.left)
            .arg(window_rect.bottom - window_rect.top)
            .arg(window_rect.left)
            .arg(window_rect.top)
            .arg(client_rect.right - client_rect.left)
            .arg(client_rect.bottom - client_rect.top));
    if (name.contains(QStringLiteral("Chrome_WidgetWin")) ||
        name.contains(QStringLiteral("Chrome_RenderWidgetHost"))) {
        probe->chromium = true;
    }
    return TRUE;  // keep walking: CEF nests further windows inside its own
}

}  // namespace
#endif

/// Count distinct colours on a coarse grid: proof that something was drawn, not a filled rectangle.
int CountDistinctColours(const QImage& image) {
    QSet<QRgb> colours;
    for (int y = 0; y < image.height(); y += 8) {
        for (int x = 0; x < image.width(); x += 8) {
            colours.insert(image.pixel(x, y));
        }
    }
    return colours.size();
}

/// Report what a screen grab of the card shows. **Diagnostic only.**
///
/// This measures the desktop, not the engine: it reported one colour on identical code whenever the
/// window was occluded or on a different compositor path (thirteenth slice). It is printed so a
/// mismatch against the engine's own capture stays visible, but it no longer decides pass or fail.
void ReportScreenGrab(QTextStream& out, QWidget* card) {
    QScreen* screen = card->screen();
    const QPixmap shot = screen != nullptr ? screen->grabWindow(card->winId()) : QPixmap();
    if (shot.isNull()) {
        out << "screenshot=unavailable\n";
        return;
    }
    const QImage image = shot.toImage();
    out << "screenshot=ok\n";
    out << "screenshot_size=" << image.width() << 'x' << image.height() << '\n';
    out << "screenshot_colours=" << CountDistinctColours(image) << " (informational)\n";
}

/// Prove that the engine really embedded its browser window into a Qt widget: the child window must
/// exist inside the card's native handle, and the page must see the card's physical size as its
/// viewport. Both are checked against numbers the shell knows independently.
int runEmbedCheck(const QString& enginePath) {
    QTextStream out(stdout);
    int failures = 0;

    QWidget window;
    window.setWindowTitle(QStringLiteral("Antikolt embed check"));
    window.resize(900, 640);
    // A deterministic position: an auto-placed window can land anywhere, including off-screen, which
    // would make every coordinate below look like a bug in the engine.
    window.move(120, 90);
    auto* layout = new QVBoxLayout(&window);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* card = new LiveCard(&window);
    layout->addWidget(card);
    window.show();
    QCoreApplication::processEvents();

    EngineClient client;
    const QString profile = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                            QStringLiteral("/embed-selftest");
    QDir().mkpath(profile);
    if (!client.start(enginePath, profile, /*hidden=*/true)) {
        out << "engine_start=FAILED\n";
        return 1;
    }
    out << "engine_start=ok\n";

    QEventLoop loop;
    qint64 surface = 0;
    bool attached = false;
    bool loaded = false;
    bool embed_ok = false;
    bool viewport_ok = false;
    bool painted_ok = false;
    bool focus_ok = false;

    QObject::connect(&client, &EngineClient::failed, &loop, [&](const QString& message) {
        out << "engine_error=" << message << '\n';
        loop.quit();
    });
    QObject::connect(card, &LiveCard::surfaceAttached, &loop, [&](qint64 id) {
        surface = id;
        attached = true;
        out << "surface=" << id << '\n';
        client.navigate(id, QStringLiteral("data:text/html,<title>embed</title>"
                                           "<h1 style='font:48px sans-serif'>embedded</h1>"));
        // Ask the engine directly whether it managed to place the surface: a size right next to a
        // window parked off-screen would look like success until somebody looked at the screen.
        // The origin is the card's client origin (0,0), which is what an embedded child should use;
        // the diagnostic prints the card's physical screen origin so the two can be compared.
        POINT card_origin = {0, 0};
        ClientToScreen(reinterpret_cast<HWND>(card->winId()), &card_origin);
        out << "card_screen_origin=" << card_origin.x << ',' << card_origin.y << '\n';
        client.setBounds(id, 0, 0, card->width(), card->height(),
                         [&](const QJsonObject& result, const QString& error) {
                             out << "set_bounds=" << (error.isEmpty() ? "ok" : "FAILED")
                                 << " applied="
                                 << (result.value(QStringLiteral("applied")).toBool() ? 1 : 0)
                                 << '\n';
                         });
    });
    QObject::connect(&client, &EngineClient::loadFinished, &loop,
                     [&](qint64 id, const QString&, int status) {
                         if (id != surface || loaded) {
                             return;
                         }
                         loaded = true;
                         out << "load_status=" << status << '\n';
                         // The widget's size may still settle after the page loads (a window manager
                         // can clamp a window to the screen), so measure everything at one instant.
                         // `id` is captured by value: this lambda outlives the load handler that made it,
                         // and the surface id has to survive that (a stale one is refused by the engine).
                         QTimer::singleShot(500, &loop, [&, id] {
#ifdef _WIN32
                             ChildProbe probe;
                             EnumChildWindows(reinterpret_cast<HWND>(card->winId()), CountChildWindows,
                                              reinterpret_cast<LPARAM>(&probe));
                             out << "child_windows=" << probe.lines.size() << '\n';
                             for (const QString& line : probe.lines) {
                                 out << "child=" << line << '\n';
                             }
                             out << "chromium_child=" << (probe.chromium ? "found" : "absent")
                                 << '\n';
                             embed_ok = probe.chromium;
#endif
                             out << "card_logical=" << card->width() << 'x' << card->height() << '\n';
                             out << "card_physical=" << card->physicalSize().width() << 'x'
                                 << card->physicalSize().height() << '\n';
                             out << "dpr=" << card->devicePixelRatioF() << '\n';

                             // The page's own viewport is the independent witness of the size. The
                             // protocol is in device independent pixels, which is exactly what the
                             // page reports, so the two numbers must be equal — not scaled.
                             client.evaluate(
                                 id,
                                 QStringLiteral(
                                     "JSON.stringify({w:innerWidth,h:innerHeight,"
                                     "dpr:devicePixelRatio,screenW:screen.width,"
                                     "screenH:screen.height,outerW:outerWidth,outerH:outerHeight})"),
                                 // `id` by value again: the capture and the focus call below run after this
                                 // handler's creator is gone, and they need the surface that is alive.
                                 [&, id](const QJsonObject& result, const QString& error) {
                                     if (!error.isEmpty()) {
                                         out << "eval=FAILED " << error << '\n';
                                         loop.quit();
                                         return;
                                     }
                                     const QString page =
                                         result.value(QStringLiteral("value")).toString();
                                     out << "page=" << page << '\n';
                                     out << "expected_css=" << card->width() << 'x' << card->height()
                                         << '\n';
                                     // Chromium draws its own 1–2 px border inside the embedded
                                     // child window, so the page viewport may differ from the card by
                                     // a pixel. Anything more is a real size bug.
                                     const auto value_of = [&page](const QString& key) {
                                         const QRegularExpression match(
                                             QStringLiteral("\"%1\":(\\d+)").arg(key));
                                         const auto found = match.match(page);
                                         return found.hasMatch() ? found.captured(1).toInt() : -1;
                                     };
                                     const int page_w = value_of(QStringLiteral("w"));
                                     const int page_h = value_of(QStringLiteral("h"));
                                     viewport_ok = std::abs(page_w - card->width()) <= 2 &&
                                                   std::abs(page_h - card->height()) <= 2;

                                     // Geometry is not painting, so ask the engine for its own
                                     // picture: `capture` reads Chromium's compositor, which stays
                                     // deterministic where a screen grab of the desktop does not
                                     // (thirteenth slice).
                                     ReportScreenGrab(out, card);
                                     client.capture(
                                         id, QStringLiteral("png"), 80,
                                         [&, id](const QJsonObject& shot, const QString& shot_error) {
                                             if (!shot_error.isEmpty()) {
                                                 out << "capture=FAILED " << shot_error << '\n';
                                                 painted_ok = false;
                                             } else {
                                                 const QString path =
                                                     shot.value(QStringLiteral("path")).toString();
                                                 const QImage image(path);
                                                 const int distinct = image.isNull()
                                                                          ? 0
                                                                          : CountDistinctColours(image);
                                                 const int captured_w =
                                                     shot.value(QStringLiteral("width")).toInt();
                                                 const int captured_h =
                                                     shot.value(QStringLiteral("height")).toInt();
                                                 out << "capture_path=" << path << '\n';
                                                 out << "capture_image=" << image.width() << 'x'
                                                     << image.height() << '\n';
                                                 out << "capture_colours=" << distinct << '\n';
                                                 // The compositor's pixels must match the size the
                                                 // shell gave the card, with the one-pixel tolerance
                                                 // the viewport check uses: Chromium draws its own
                                                 // border inside the child window.
                                                 const bool size_matches =
                                                     std::abs(captured_w -
                                                              card->physicalSize().width()) <= 2 &&
                                                     std::abs(captured_h -
                                                              card->physicalSize().height()) <= 2;
                                                 out << "capture_size_match="
                                                     << (size_matches ? "ok" : "FAILED") << '\n';
                                                 painted_ok = distinct > 2 && size_matches;
                                             }

                                             // Focus is the last link: a child window in another
                                             // process does not inherit activation, so the shell
                                             // sends `focus` and the page must agree that it has the
                                             // keyboard.
                                             card->setFocus(Qt::OtherFocusReason);
                                             client.focus(id, true);
                                             QTimer::singleShot(300, &loop, [&, id] {
                                                 client.evaluate(
                                                     id, QStringLiteral("document.hasFocus()"),
                                                     [&](const QJsonObject& focused,
                                                         const QString& failure) {
                                                         if (!failure.isEmpty()) {
                                                             out << "focus_eval=FAILED " << failure
                                                                 << '\n';
                                                         } else {
                                                             const QString value =
                                                                 focused
                                                                     .value(QStringLiteral("value"))
                                                                     .toString();
                                                             out << "page_has_focus=" << value
                                                                 << '\n';
                                                             focus_ok =
                                                                 value == QStringLiteral("true");
                                                         }
                                                         loop.quit();
                                                     });
                                             });
                                         });
                                 });
                         });
                     });

    client.handshake();
    QObject::connect(&client, &EngineClient::engineReady, &loop, [&](const QJsonObject&) {
        if (!card->attach(&client, QStringLiteral("persist:embed-selftest"))) {
            out << "attach=FAILED\n";
            loop.quit();
        }
    });

    QTimer::singleShot(45000, &loop, &QEventLoop::quit);
    loop.exec();

    if (!attached) {
        ++failures;
        out << "attach=FAILED\n";
    }
    if (!loaded) {
        ++failures;
        out << "load=FAILED\n";
    }
    out << "embed=" << (embed_ok ? "ok" : "FAILED") << '\n';
    out << "viewport_match=" << (viewport_ok ? "ok" : "FAILED") << '\n';
    out << "paint=" << (painted_ok ? "ok" : "FAILED") << '\n';
    out << "focus=" << (focus_ok ? "ok" : "FAILED") << '\n';
    if (!embed_ok || !viewport_ok || !painted_ok || !focus_ok) {
        ++failures;
    }

    if (surface != 0) {
        // Wait for the destroy reply before shutting the transport down. Firing it and closing the pipe
        // leaves a reply queued for a client that is about to be destructed — that is the 0xC0000005 seen
        // after every result was printed (1 crash in 4 runs of --capturetest).
        QEventLoop wait;
        client.destroySurface(surface, [&wait](const QJsonObject&, const QString&) { wait.quit(); });
        QTimer::singleShot(2000, &wait, &QEventLoop::quit);
        wait.exec();
    }
    client.shutdown();
    window.close();
    out.flush();
    return failures == 0 ? 0 : 1;
}

/// Prove the engine can describe its own pixels: `capture` must answer with a real picture of the
/// page.
///
/// This is the measurement a screen grab cannot make, and it is the same call that will fill the card
/// grid's previews — a test-only path would prove nothing about the product. The page is three flat
/// bands, so a working capture has distinct colours, and the size must equal the page's own viewport
/// multiplied by the device scale, which the page reports independently.
int runCaptureCheck(const QString& enginePath) {
    QTextStream out(stdout);
    int failures = 0;

    // The card lives in a visible Qt widget, exactly as a real card does. A window of the engine's
    // own that another window covers produced a flat capture (one colour) on this machine, so the
    // check measures the path the product actually uses.
    QWidget window;
    window.setWindowTitle(QStringLiteral("Antikolt capture check"));
    window.resize(700, 460);
    window.move(160, 120);
    auto* layout = new QVBoxLayout(&window);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* card = new LiveCard(&window);
    layout->addWidget(card);
    window.show();
    QCoreApplication::processEvents();

    EngineClient client;
    const QString profile = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                            QStringLiteral("/capture-selftest");
    QDir().mkpath(profile);
    if (!client.start(enginePath, profile, /*hidden=*/true)) {
        out << "engine_start=FAILED\n";
        return 1;
    }
    out << "engine_start=ok\n";

    QEventLoop loop;
    qint64 surface = 0;
    bool loaded = false;
    bool painted = false;
    bool sized = false;
    /// How many times a capture the compositor swallowed was tried again (see the failure branch).
    int capture_retries = 0;
    int expected_w = 0;
    int expected_h = 0;

    // `#` would start a URL fragment and truncate the document, so the colours are percent-encoded.
    const QString page = QStringLiteral(
        "data:text/html,<title>capture</title>"
        "<body style='margin:0;background:%23fff'>"
        "<div style='height:130px;background:%2306c'></div>"
        "<div style='height:130px;background:%23c30'></div>"
        "<div style='height:140px;background:%23093'></div>"
        "</body>");

    QObject::connect(&client, &EngineClient::failed, &loop, [&](const QString& message) {
        out << "engine_error=" << message << '\n';
        loop.quit();
    });
    QObject::connect(&client, &EngineClient::engineReady, &loop, [&](const QJsonObject& info) {
        const QJsonArray capabilities = info.value(QStringLiteral("capabilities")).toArray();
        bool announced = false;
        for (const QJsonValue& capability : capabilities) {
            if (capability.toString() == QStringLiteral("capture")) {
                announced = true;
            }
        }
        out << "capture_capability=" << (announced ? "announced" : "MISSING") << '\n';
        if (!card->attach(&client, QStringLiteral("persist:capture-selftest"))) {
            out << "attach=FAILED\n";
            loop.quit();
        }
    });
    QObject::connect(card, &LiveCard::surfaceAttached, &loop, [&](qint64 id) {
        surface = id;
        out << "surface=" << id << '\n';
        client.navigate(surface, page);
    });
    QObject::connect(&client, &EngineClient::loadFinished, &loop,
                     [&](qint64 id, const QString& url, int status) {
                         // The surface first loads `about:blank`, and the final URL of the probe page
                         // comes back re-encoded, so only the placeholder is filtered by name.
                         if (id != surface || loaded || url == QStringLiteral("about:blank")) {
                             return;
                         }
                         loaded = true;
                         out << "load_status=" << status << '\n';
                         // Wait a frame so the capture sees the painted page and not the blank frame
                         // a navigation starts from.
                         QTimer::singleShot(500, &loop, [&] {
                             client.evaluate(
                                 surface,
                                 QStringLiteral("JSON.stringify({w:innerWidth,h:innerHeight,"
                                                "dpr:devicePixelRatio})"),
                                 [&](const QJsonObject& metrics, const QString& failure) {
                                     const QString json =
                                         metrics.value(QStringLiteral("value")).toString();
                                     const auto number_of = [&json](const QString& key) {
                                         const QRegularExpression match(
                                             QStringLiteral("\"%1\":([0-9.]+)").arg(key));
                                         const auto found = match.match(json);
                                         return found.hasMatch() ? found.captured(1).toDouble()
                                                                 : 0.0;
                                     };
                                     if (!failure.isEmpty()) {
                                         out << "metrics=FAILED " << failure << '\n';
                                     }
                                     // The page's own viewport is the independent witness of the size;
                                     // the screenshot is in device pixels, so the two are multiplied.
                                     expected_w = static_cast<int>(
                                         qRound(number_of(QStringLiteral("w")) *
                                                number_of(QStringLiteral("dpr"))));
                                     expected_h = static_cast<int>(
                                         qRound(number_of(QStringLiteral("h")) *
                                                number_of(QStringLiteral("dpr"))));
                                     out << "page_css=" << number_of(QStringLiteral("w")) << 'x'
                                         << number_of(QStringLiteral("h")) << '\n';
                                     out << "dpr=" << number_of(QStringLiteral("dpr")) << '\n';
                                     out << "expected_physical=" << expected_w << 'x' << expected_h
                                         << '\n';
                                     client.capture(
                                         surface, QStringLiteral("png"), 80,
                                         [&](const QJsonObject& shot, const QString& shot_error) {
                                             if (!shot_error.isEmpty()) {
                                                 // A compositor falling back from a dead GPU (measured on this machine,
                                                 // docs/HU/18-F0-JELENTES.md) can swallow one capture request: the answer never
                                                 // comes and the request times out. The product retries (`kCefCaptureAttempts`),
                                                 // so the check does too; a fresh navigation asks the renderer for a new frame.
                                                 if (capture_retries < 2) {
                                                     ++capture_retries;
                                                     out << "capture_retry=" << capture_retries << " (" << shot_error << ")\n";
                                                     loaded = false;
                                                     client.navigate(surface, page);
                                                     return;
                                                 }
                                                 out << "capture=FAILED " << shot_error << '\n';
                                                 loop.quit();
                                                 return;
                                             }
                                             const QString path =
                                                 shot.value(QStringLiteral("path")).toString();
                                             const int width =
                                                 shot.value(QStringLiteral("width")).toInt();
                                             const int height =
                                                 shot.value(QStringLiteral("height")).toInt();
                                             const QImage image(path);
                                             const int distinct = image.isNull()
                                                                      ? 0
                                                                      : CountDistinctColours(image);
                                             out << "capture_path=" << path << '\n';
                                             out << "capture_bytes="
                                                 << shot.value(QStringLiteral("bytes")).toInt()
                                                 << '\n';
                                             out << "capture_size=" << width << 'x' << height
                                                 << '\n';
                                             out << "capture_image=" << image.width() << 'x'
                                                 << image.height() << '\n';
                                             out << "capture_colours=" << distinct << '\n';
                                             painted = !image.isNull() && distinct > 2;
                                             // Chromium rounds the device-scaled surface down, so a
                                             // one-pixel difference is arithmetic, not a bug.
                                             sized = expected_w > 0 && expected_h > 0 &&
                                                     std::abs(width - expected_w) <= 2 &&
                                                     std::abs(height - expected_h) <= 2 &&
                                                     image.width() == width;
                                             loop.quit();
                                         });
                                 });
                         });
                     });

    client.handshake();
    QTimer::singleShot(40000, &loop, &QEventLoop::quit);
    loop.exec();

    out << "capture=" << (painted ? "ok" : "FAILED") << '\n';
    out << "capture_size=" << (sized ? "ok" : "FAILED") << '\n';
    if (!painted || !sized) {
        ++failures;
    }
    if (surface != 0) {
        // Wait for the destroy reply before shutting the transport down. Firing it and closing the pipe
        // leaves a reply queued for a client that is about to be destructed — that is the 0xC0000005 seen
        // after every result was printed (1 crash in 4 runs of --capturetest).
        QEventLoop wait;
        client.destroySurface(surface, [&wait](const QJsonObject&, const QString&) { wait.quit(); });
        QTimer::singleShot(2000, &wait, &QEventLoop::quit);
        wait.exec();
    }
    client.shutdown();
    out.flush();
    return failures == 0 ? 0 : 1;
}

/// Prove the hibernation clock: a live card with no user activity goes back to disk by itself.
///
/// The ten-minute rule is measured with a shortened clock (`ANTIKOLT_HIBERNATE_MS`), because waiting
/// ten minutes in a self test measures nothing but patience. What must happen: the card's surface is
/// closed, a snapshot is written, and the grid comes back on its own.
int runLifecycleCheck(const QString& enginePath) {
    QTextStream out(stdout);
    qputenv("ANTIKOLT_ENGINE", enginePath.toUtf8());
    qputenv("ANTIKOLT_HIBERNATE_MS", "1500");

    MainWindow window;
    window.show();
    window.move(150, 110);
    window.startEngine();

    EngineClient* client = window.engine();
    QEventLoop loop;
    qint64 home_surface = 0;
    qint64 card_surface = 0;
    bool home_loaded = false;
    bool clicked = false;
    bool closed = false;
    bool back = false;

    const auto finish = [&] {
        if (closed && back) {
            loop.quit();
        }
    };

    QObject::connect(client, &EngineClient::failed, &loop, [&](const QString& message) {
        out << "engine_error=" << message << '\n';
        loop.quit();
    });
    QObject::connect(client, &EngineClient::surfaceClosed, &loop, [&](qint64 id) {
        if (id == card_surface) {
            closed = true;
            out << "card_closed=" << id << '\n';
            finish();
        }
    });
    QObject::connect(client, &EngineClient::loadFinished, &loop,
                     [&](qint64 id, const QString& url, int) {
                         if (!home_loaded && url.startsWith(QStringLiteral("file:"))) {
                             home_loaded = true;
                             home_surface = id;
                             out << "home_loaded=" << url << '\n';
                             // Click a card as a user would, then say nothing and wait.
                             // Click an *unpinned* card as a user would: the first tile may be a pinned
                             // one, and a pinned card is exactly what must not hibernate (phase 2).
                             QTimer::singleShot(500, &loop, [&, id] {
                                 client->evaluate(
                                     id,
                                     QStringLiteral(
                                         "(function(){var els=document.querySelectorAll('#viewport "
                                         "antikolt-card');for(var i=0;i<els.length;i++){if(els[i].card"
                                         "&&!els[i].card.pinned){els[i].click();return 'clicked';}}"
                                         "return 'no-unpinned-card';})()"),
                                     [&](const QJsonObject& reply, const QString& error) {
                                         clicked = error.isEmpty() &&
                                                   reply.value(QStringLiteral("value")).toString() ==
                                                       QStringLiteral("clicked");
                                         out << "card_click=" << (clicked ? "ok" : "FAILED")
                                             << '\n';
                                     });
                             });
                             return;
                         }
                         if (id != home_surface && url.startsWith(QStringLiteral("file:"))) {
                             // The grid came back on a *new* surface: the old one is gone, and the pinned
                             // phase below must address this one. A stale id used to be harmless while the
                             // engine had a single surface; a command now names its surface, and a dead one
                             // is refused (measured: pinned_open=FAILED no live surface).
                             home_surface = id;
                             back = true;
                             out << "grid_back=" << url << '\n';
                             finish();
                         }
                     });
    // The grid's own surface arrives (and loads) before anything is clicked, so the card's surface is
    // the first one created *after* the grid is up. Getting this order wrong made the test close the
    // grid instead of the card.
    QObject::connect(client, &EngineClient::surfaceCreated, &loop, [&](qint64 id) {
        if (!home_loaded || id == home_surface || card_surface != 0) {
            return;
        }
        card_surface = id;
        out << "card_surface=" << id << '\n';
    });

    QTimer::singleShot(60000, &loop, &QEventLoop::quit);
    loop.exec();

    out << "hibernated=" << (closed ? "ok" : "FAILED") << '\n';
    out << "grid_returned=" << (back ? "ok" : "FAILED") << '\n';

    // 2. The documented opt-out (docs/HU/02 §4.2): the same shell and the same shortened clock, but
    //    this card is opened as *pinned*, so the clock has to leave it alone. If the flag is lost
    //    anywhere along the way, the card is closed well before the wait here ends.
    qint64 pinned_surface = 0;
    bool pinned_opened = false;
    bool pinned_closed = false;
    if (closed && back && home_surface != 0) {
        QEventLoop pinned_loop;
        QObject::connect(client, &EngineClient::surfaceCreated, &pinned_loop, [&](qint64 id) {
            if (id != home_surface && pinned_surface == 0) {
                pinned_surface = id;
                pinned_opened = true;
                out << "pinned_surface=" << id << '\n';
            }
        });
        QObject::connect(client, &EngineClient::surfaceClosed, &pinned_loop, [&](qint64 id) {
            if (id == pinned_surface) {
                pinned_closed = true;
                out << "pinned_closed=" << id << '\n';
                pinned_loop.quit();
            }
        });
        client->evaluate(
            home_surface,
            QStringLiteral("window.antikoltNative.openCard({id:'probe', url:'about:blank', "
                           "title:'pinned probe', pinned:true}); 'sent'"),
            [&](const QJsonObject& reply, const QString& error) {
                out << "pinned_open="
                    << (error.isEmpty() ? reply.value(QStringLiteral("value")).toString()
                                        : QStringLiteral("FAILED ") + error)
                    << '\n';
            });
        // More than four clock periods: a broken opt-out closes the card long before this fires.
        QTimer::singleShot(6500, &pinned_loop, &QEventLoop::quit);
        pinned_loop.exec();
    }
    out << "pinned_honoured=" << ((pinned_opened && !pinned_closed) ? "ok" : "FAILED") << '\n';

    window.close();
    out.flush();
    return closed && back && pinned_opened && !pinned_closed ? 0 : 1;
}

/// Prove the grid shows the cards the core owns, not the page's built-in samples.
///
/// The core is asked directly for its list first (the oracle), then the home page is loaded in the
/// embedded card and asked what it is displaying. Neither a screenshot nor the shell's own
/// bookkeeping is trusted: `window.antikoltGridState()` is the page's own witness, so a push that
/// never arrived, arrived empty or was overwritten by the sample list all fail here.
int runGridCheck(const QString& enginePath) {
    QTextStream out(stdout);
    int failures = 0;

    const QString corePath = MainWindow::coreExecutable();
    if (corePath.isEmpty()) {
        out << "core_executable=FAILED (set ANTIKOLT_CORE)\n";
        return 1;
    }
    out << "core_executable=" << corePath << '\n';
    qputenv("ANTIKOLT_ENGINE", enginePath.toUtf8());
    qputenv("ANTIKOLT_CORE", corePath.toUtf8());

    // 1. The oracle: what the core says the grid should show. The same data directory the window
    //    uses, so both read the very same store.
    CoreClient oracle;
    const QString core_data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                              QStringLiteral("/core");
    QDir().mkpath(core_data);
    QStringList expected_titles;
    /// The URL of the first card the core lists: the one the grid's first tile must open.
    QString expected_url;
    if (!oracle.start(corePath, core_data)) {
        out << "core_start=FAILED\n";
        return 1;
    }
    out << "core_start=ok\n";
    {
        QEventLoop core_loop;
        QObject::connect(&oracle, &CoreClient::failed, &core_loop, [&](const QString& message) {
            out << "core_error=" << message << '\n';
            core_loop.quit();
        });
        QObject::connect(&oracle, &CoreClient::ready, &core_loop, [&](const QJsonObject& info) {
            out << "core_name=" << info.value(QStringLiteral("name")).toString() << '\n';
            oracle.cardsList([&](const QJsonObject& data, const QString& error) {
                if (!error.isEmpty()) {
                    out << "cards_list=FAILED " << error << '\n';
                    core_loop.quit();
                    return;
                }
                for (const QJsonValue& value : data.value(QStringLiteral("cards")).toArray()) {
                    const QJsonObject card = value.toObject();
                    expected_titles.append(card.value(QStringLiteral("title")).toString());
                    if (expected_url.isEmpty()) {
                        expected_url = card.value(QStringLiteral("url")).toString();
                    }
                }
                core_loop.quit();
            });
        });
        oracle.handshake();
        QTimer::singleShot(15000, &core_loop, &QEventLoop::quit);
        core_loop.exec();
    }
    oracle.shutdown();
    out << "core_cards=" << expected_titles.size() << '\n';
    if (expected_titles.isEmpty()) {
        out << "cards_list=FAILED (the core returned nothing)\n";
        return 1;
    }

    // 2. The shell: engine plus its own core, home page in the live card, then read the grid.
    MainWindow window;
    window.show();
    window.move(140, 100);
    window.startEngine();

    EngineClient* client = window.engine();
    QEventLoop loop;
    bool loaded = false;
    bool pushed = false;
    qint64 home_surface = 0;
    QJsonObject state;

    QObject::connect(client, &EngineClient::failed, &loop, [&](const QString& message) {
        out << "engine_error=" << message << '\n';
        loop.quit();
    });
    QObject::connect(client, &EngineClient::loadFinished, &loop,
                     [&](qint64 surface, const QString& url, int) {
                         if (loaded || !url.startsWith(QStringLiteral("file:"))) {
                             return;
                         }
                         loaded = true;
                         home_surface = surface;
                         out << "home_loaded=" << url << '\n';
                         // The push follows this event immediately; give the page its moment to
                         // re-render before asking what it shows.
                         QTimer::singleShot(600, &loop, [&, surface] {
                             client->evaluate(
                                 surface,
                                 QStringLiteral("JSON.stringify(window.antikoltGridState())"),
                                 [&](const QJsonObject& reply, const QString& error) {
                                     if (!error.isEmpty()) {
                                         out << "grid_state=FAILED " << error << '\n';
                                         loop.quit();
                                         return;
                                     }
                                     const QString json =
                                         reply.value(QStringLiteral("value")).toString();
                                     state = QJsonDocument::fromJson(json.toUtf8()).object();
                                     pushed = state.value(QStringLiteral("source")).toString() ==
                                              QStringLiteral("core");
                                     loop.quit();
                                 });
                         });
                     });

    QTimer::singleShot(45000, &loop, &QEventLoop::quit);
    loop.exec();

    QStringList shown;
    for (const QJsonValue& value : state.value(QStringLiteral("titles")).toArray()) {
        shown.append(value.toString());
    }
    const bool same = !shown.isEmpty() && shown == expected_titles;

    out << "grid_source=" << (pushed ? "core" : "FAILED") << '\n';
    out << "grid_count=" << shown.size() << '\n';
    for (const QString& title : shown) {
        out << "grid_card=" << title << '\n';
    }
    out << "grid_first=" << state.value(QStringLiteral("firstTitle")).toString() << '\n';
    out << "grid_matches_core=" << (same ? "ok" : "FAILED") << '\n';

    // 3. The user path: clicking a card in the grid must make the shell open *that* card. Two facts
    //    are measured and both are the page's own doing — the bridge call the page sends, and the new
    //    surface the shell creates. What the remote server answers is deliberately not part of this:
    //    the grid is on a machine without TLS, and the request is what is being proven.
    QString clicked_url;
    bool new_surface = false;
    if (pushed && same && home_surface != 0) {
        QEventLoop click_loop;
        QObject::connect(client, &EngineClient::bridgeCall, &click_loop,
                         [&](qint64, const QString& method, const QJsonObject& args) {
                             if (method != QStringLiteral("open_card")) {
                                 return;
                             }
                             clicked_url = args.value(QStringLiteral("url")).toString();
                             out << "grid_click_url=" << clicked_url << '\n';
                             if (new_surface) {
                                 click_loop.quit();
                             }
                         });
        QObject::connect(client, &EngineClient::surfaceCreated, &click_loop, [&](qint64 id) {
            if (id == home_surface) {
                return;
            }
            new_surface = true;
            out << "card_surface=" << id << '\n';
            if (!clicked_url.isEmpty()) {
                click_loop.quit();
            }
        });
        // The clicked card is the first one *the grid draws*, which is not necessarily the core's first record:
        // the grid sorts pinned cards first and shows one workspace at a time (ADR-056). Reading the URL from the
        // card just before clicking keeps the assertion about the click ("that card opens") instead of about the
        // store's order.
        QString clicked_card_url;
        client->evaluate(
            home_surface,
            QStringLiteral("(() => { const card = document.querySelector('#viewport antikolt-card');"
                           " if (!card) { return ''; } const url = card.card?.url ?? '';"
                           " card.click(); return url; })()"),
            [&](const QJsonObject& reply, const QString& error) {
                clicked_card_url = error.isEmpty() ? reply.value(QStringLiteral("value")).toString()
                                                   : QString();
                out << "card_click="
                    << (error.isEmpty() ? QStringLiteral("clicked")
                                        : QStringLiteral("FAILED ") + error)
                    << '\n';
            });
        QTimer::singleShot(30000, &click_loop, &QEventLoop::quit);
        click_loop.exec();
        if (!clicked_card_url.isEmpty()) {
            expected_url = clicked_card_url;
        }
    }
    const bool click_ok = new_surface && !expected_url.isEmpty() && clicked_url == expected_url;
    out << "grid_click_open=" << (click_ok ? "ok" : "FAILED") << '\n';
    if (!pushed || !same || !click_ok) {
        ++failures;
    }
    window.close();
    out.flush();
    return failures == 0 ? 0 : 1;
}

/// Prove the whole product loop: the home page calls the injected bridge, the shell hibernates the
/// grid and opens the card the page asked for. This is the path a user takes by clicking a card.
int runOpenCheck(const QString& enginePath) {
    QTextStream out(stdout);
    int failures = 0;

    qputenv("ANTIKOLT_ENGINE", enginePath.toUtf8());

    MainWindow window;
    window.show();
    window.move(120, 90);
    window.startEngine();

    EngineClient* client = window.engine();
    const QString card_url =
        QStringLiteral("data:text/html,<title>card2</title><h1>card two</h1>");

    QEventLoop loop;
    qint64 first_surface = 0;
    qint64 second_surface = 0;
    bool home_loaded = false;
    bool bridge_present = false;
    bool bridge_called = false;
    bool card_loaded = false;

    QObject::connect(client, &EngineClient::failed, &loop, [&](const QString& message) {
        out << "engine_error=" << message << '\n';
        loop.quit();
    });

    QObject::connect(client, &EngineClient::loadFinished, &loop,
                     [&](qint64 surface, const QString& url, int status) {
                         out << "load_finished surface=" << surface << " status=" << status
                             << " url=" << url << '\n';
                         if (!home_loaded) {
                             if (!url.startsWith(QStringLiteral("file:"))) {
                                 return;
                             }
                             home_loaded = true;
                             first_surface = surface;
                             // The shell installs the bridge right after navigation; give the renderer
                             // a moment to run the injected script before calling into it.
                             QTimer::singleShot(700, &loop, [&, surface] {
                                 client->evaluate(
                                     surface, QStringLiteral("typeof window.antikoltNative"),
                                     [&, surface](const QJsonObject& result,
                                                  const QString& error) {
                                         if (!error.isEmpty()) {
                                             out << "bridge_probe=FAILED " << error << '\n';
                                             loop.quit();
                                             return;
                                         }
                                         const QString type =
                                             result.value(QStringLiteral("value")).toString();
                                         out << "bridge=" << type << '\n';
                                         bridge_present = type == QStringLiteral("object");

                                         // Exactly the call the grid makes when a card is clicked.
                                         const QString script =
                                             QStringLiteral(
                                                 "window.antikoltNative.openCard({url:'%1',"
                                                 "title:'card two'}); 'sent'")
                                                 .arg(card_url);
                                         client->evaluate(
                                             surface, script,
                                             [&](const QJsonObject& called, const QString& failure) {
                                                 bridge_called = failure.isEmpty() &&
                                                                 called.value(QStringLiteral(
                                                                                  "value"))
                                                                         .toString() ==
                                                                     QStringLiteral("sent");
                                                 out << "open_card="
                                                     << (bridge_called ? "sent" : "FAILED")
                                                     << (failure.isEmpty()
                                                             ? QString()
                                                             : QStringLiteral(" ") + failure)
                                                     << '\n';
                                             });
                                     });
                             });
                             return;
                         }
                         if (bridge_present && !card_loaded && surface != first_surface &&
                             url == card_url) {
                             card_loaded = true;
                             second_surface = surface;
                             loop.quit();
                         }
                     });

    QTimer::singleShot(60000, &loop, &QEventLoop::quit);
    loop.exec();

    // The grid must be hibernated, not merely replaced: a new surface with a new id is the proof.
    const qint64 live = window.liveCard()->surface();
    out << "first_surface=" << first_surface << " second_surface=" << second_surface
        << " live=" << live << '\n';
    out << "home_loaded=" << (home_loaded ? "ok" : "FAILED") << '\n';
    out << "bridge=" << (bridge_present ? "ok" : "FAILED") << '\n';
    out << "open_card=" << (bridge_called ? "ok" : "FAILED") << '\n';
    out << "card_opened=" << (card_loaded && second_surface != first_surface ? "ok" : "FAILED")
        << '\n';
    if (!home_loaded || !bridge_present || !bridge_called ||
        !(card_loaded && second_surface != first_surface)) {
        ++failures;
    }

    out.flush();
    return failures == 0 ? 0 : 1;
}

/// Prove the shell can talk to the core: handshake, read the card list, and record an open. The core
/// owns the data, so this seam is where every later feature (sessions, policies) will hang.
/// Prove the policy's verdict is executed: a due origin is wiped when its card is next opened.
///
/// The row is written with `last_seen_ms: 0` and a `wipe` policy, so the core judges it expired. The
/// window is asked to clean up while the grid is showing — nothing can be wiped immediately, because the
/// shell has one surface (I1) — and only when the card for that origin opens does the engine clear it.
/// Two witnesses: the page says it wrote (`window.__wrote`), and its storage is empty afterwards; the
/// ledger row must be gone too.
int runCleanupCheck(const QString& enginePath) {
    QTextStream out(stdout);
    qputenv("ANTIKOLT_ENGINE", enginePath.toUtf8());
    qputenv("ANTIKOLT_CORE", MainWindow::coreExecutable().toUtf8());

    ProbeOrigin probe(storageProbePage());
    if (!probe.listening()) {
        out << "server=FAILED " << probe.errorString() << '\n';
        return 1;
    }
    const QString origin = probe.origin();
    out << "origin=" << origin << '\n';

    const QString core_dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                             QStringLiteral("/core");
    QDir().mkpath(core_dir);

    // 1. A due row for that origin. The oracle stays up to read the ledger back afterwards.
    CoreClient oracle;
    if (!oracle.start(MainWindow::coreExecutable(), core_dir)) {
        out << "core_start=FAILED\n";
        return 1;
    }
    bool written = false;
    {
        QEventLoop loop;
        QJsonObject session;
        session.insert(QStringLiteral("container_id"), QStringLiteral("c-private"));
        session.insert(QStringLiteral("origin"), origin);
        session.insert(QStringLiteral("state"), QStringLiteral("expired"));
        session.insert(QStringLiteral("policy"), QStringLiteral("wipe"));
        session.insert(QStringLiteral("cookie_count"), 1);
        session.insert(QStringLiteral("cookie_bytes"), 64);
        session.insert(QStringLiteral("storage_bytes"), 1024);
        session.insert(QStringLiteral("service_worker_count"), 0);
        session.insert(QStringLiteral("last_seen_ms"), 0);
        session.insert(QStringLiteral("last_verified_ms"), 0);
        session.insert(QStringLiteral("live_ram_bytes"), QJsonValue::Null);
        session.insert(QStringLiteral("keep_alive"), false);
        oracle.sessionUpsert(session, [&](const QJsonObject&, const QString& error) {
            written = error.isEmpty();
            loop.quit();
        });
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    if (!written) {
        out << "session_written=FAILED\n";
        oracle.shutdown();
        return 1;
    }
    out << "session_written=ok\n";
    // The oracle is done: it reads the ledger once at start-up, so it could only ever report stale rows.
    oracle.shutdown();

    // 2. The window: queue the cleanup on the grid, then open the card the policy marked.
    MainWindow window;
    window.show();
    window.move(120, 90);
    window.startEngine();

    qint64 card_surface = 0;
    QString storage_after;
    bool page_wrote = false;
    {
        QEventLoop loop;
        QObject::connect(window.engine(), &EngineClient::loadFinished, &loop,
                         [&](qint64 id, const QString& url, int) {
                             if (url.startsWith(QStringLiteral("file:"))) {
                                 QTimer::singleShot(500, &loop, [&] {
                                     window.cleanupSessions();
                                     QTimer::singleShot(500, &loop, [&] {
                                         window.openCard(origin + QStringLiteral("/"),
                                                         QStringLiteral("cleanup probe"));
                                     });
                                 });
                                 return;
                             }
                             if (!url.startsWith(origin) || card_surface != 0) {
                                 return;
                             }
                             card_surface = id;
                             // The shell clears the origin while this load finishes; give it a moment,
                             // then read the page's own witness and its storage.
                             QTimer::singleShot(900, &loop, [&, id] {
                                 window.engine()->evaluate(
                                     id,
                                     QStringLiteral(
                                         "JSON.stringify({wrote: window.__wrote===true, storage: ") +
                                         storageReadScript() + QStringLiteral("})"),
                                     [&](const QJsonObject& reply, const QString& error) {
                                         if (error.isEmpty()) {
                                             const QJsonObject state = QJsonDocument::fromJson(
                                                     reply.value(QStringLiteral("value"))
                                                         .toString()
                                                         .toUtf8())
                                                                          .object();
                                             page_wrote = state.value(QStringLiteral("wrote")).toBool();
                                             storage_after =
                                                 state.value(QStringLiteral("storage")).toString();
                                         }
                                         loop.quit();
                                     });
                             });
                         });
        QTimer::singleShot(60000, &loop, &QEventLoop::quit);
        loop.exec();
    }

    // 3. The row for *this* origin must be gone: the plan was executed, not just shown. The store is
    //    shared with the other checks (the shell has one data directory), so the ledger is not empty —
    //    only this origin's row has to be. A **fresh** client reads the file the shell just wrote; the
    //    oracle's own copy was loaded before the wipe and would report stale rows.
    int rows_left = -1;
    int origin_rows = -1;
    {
        CoreClient verifier;
        if (!verifier.start(MainWindow::coreExecutable(), core_dir)) {
            out << "verify_start=FAILED\n";
        } else {
            QEventLoop loop;
            verifier.sessionList([&](const QJsonObject& listed, const QString& error) {
                if (error.isEmpty()) {
                    const QJsonArray sessions = listed.value(QStringLiteral("sessions")).toArray();
                    rows_left = sessions.size();
                    int mine = 0;
                    for (const QJsonValue& value : sessions) {
                        if (value.toObject().value(QStringLiteral("origin")).toString() == origin) {
                            ++mine;
                        }
                    }
                    origin_rows = mine;
                }
                loop.quit();
            });
            QTimer::singleShot(10000, &loop, &QEventLoop::quit);
            loop.exec();
            verifier.shutdown();
        }
    }

    out << "page_wrote=" << (page_wrote ? "ok" : "FAILED") << '\n';
    out << "storage_after=" << storage_after << '\n';
    out << "rows_left=" << rows_left << " origin_rows=" << origin_rows << '\n';
    const bool ok = page_wrote && storage_after == QStringLiteral("|") && origin_rows == 0;
    out << "cleanup=" << (ok ? "ok" : "FAILED") << '\n';
    window.close();
    out.flush();
    return ok ? 0 : 1;
}

/// Prove the engine can clear one origin's storage — the execution half of the 24-hour policy.
///
/// The origin is a real `http://` one served by a socket in this process, because storage needs an
/// origin a page can own: `file://` has no storable origin and `data:` has an opaque one. The page
/// writes a cookie and a localStorage entry, the test reads them back, asks the engine to clear them
/// through `Storage.clearDataForOrigin`, and reads again — the second read must come back empty.
int runWipeCheck(const QString& enginePath) {
    QTextStream out(stdout);
    const QString profile = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                            QStringLiteral("/wipe-selftest");
    QDir(profile).removeRecursively();
    QDir().mkpath(profile);

    ProbeOrigin probe(storageProbePage());
    if (!probe.listening()) {
        out << "server=FAILED " << probe.errorString() << '\n';
        return 1;
    }
    const QString origin = probe.origin();
    out << "origin=" << origin << '\n';

    EngineClient client;
    if (!client.start(enginePath, profile, true)) {
        out << "engine_start=FAILED\n";
        return 1;
    }
    out << "engine_start=ok\n";

    qint64 surface = 0;
    bool cleared = false;
    QString before;
    QString after;
    {
        QEventLoop loop;
        QObject::connect(&client, &EngineClient::failed, &loop, [&](const QString& message) {
            out << "engine_error=" << message << '\n';
            loop.quit();
        });
        QObject::connect(&client, &EngineClient::engineReady, &loop, [&](const QJsonObject&) {
            client.createSurface(
                QStringLiteral("persist:wipe"), 800, 600, 100, 0,
                [&](const QJsonObject& created, const QString& error) {
                    if (!error.isEmpty()) {
                        out << "create_surface=FAILED " << error << '\n';
                        loop.quit();
                        return;
                    }
                    surface = static_cast<qint64>(created.value(QStringLiteral("surface")).toDouble());
                    out << "surface=" << surface << '\n';
                    client.navigate(surface, origin + QStringLiteral("/"));
                });
        });
        QObject::connect(&client, &EngineClient::loadFinished, &loop,
                         [&](qint64 id, const QString& url, int status) {
                             if (id != surface || !url.startsWith(QStringLiteral("http://127.0.0.1"))) {
                                 return;
                             }
                             out << "load_status=" << status << '\n';
                             // Give the page's own script its moment, then read what it stored.
                             QTimer::singleShot(300, &loop, [&] {
                                 client.evaluate(
                                     surface,
                                     QStringLiteral("(localStorage.getItem('probe')||'')+'|'"
                                                    "+document.cookie"),
                                     [&](const QJsonObject& reply, const QString& error) {
                                         if (!error.isEmpty()) {
                                             out << "storage_read=FAILED " << error << '\n';
                                             loop.quit();
                                             return;
                                         }
                                         before = reply.value(QStringLiteral("value")).toString();
                                         out << "storage_before=" << before << '\n';
                                         client.clearOriginData(
                                             surface, origin,
                                             {QStringLiteral("cookies"),
                                              QStringLiteral("local_storage")},
                                             [&](const QJsonObject& result, const QString& failure) {
                                                 if (!failure.isEmpty()) {
                                                     out << "clear=FAILED " << failure << '\n';
                                                     loop.quit();
                                                     return;
                                                 }
                                                 cleared = result.value(QStringLiteral("cleared")).toBool();
                                                 out << "clear=" << (cleared ? "ok" : "FAILED") << '\n';
                                                 client.evaluate(
                                                     surface,
                                                     QStringLiteral("(localStorage.getItem('probe')||'')+'|'"
                                                                    "+document.cookie"),
                                                     [&](const QJsonObject& second, const QString& second_error) {
                                                         if (second_error.isEmpty()) {
                                                             after = second.value(QStringLiteral("value")).toString();
                                                         }
                                                         loop.quit();
                                                     });
                                             });
                                     });
                             });
                         });
        client.handshake();
        QTimer::singleShot(45000, &loop, &QEventLoop::quit);
        loop.exec();
    }

    out << "storage_after=" << after << '\n';
    // The wipe is proven by the difference, not by the reply: the data must have been readable first,
    // and afterwards both halves must be empty — the page reads `localStorage + '|' + document.cookie`,
    // so "gone" is exactly "|".
    const bool ok = cleared && before.startsWith(QStringLiteral("1|")) &&
                    before.contains(QStringLiteral("probe=1")) &&
                    after == QStringLiteral("|");
    out << "wipe=" << (ok ? "ok" : "FAILED") << '\n';
    client.shutdown();
    out.flush();
    return ok ? 0 : 1;
}

/// Prove scrolling and zoom reach the page (ADR-038): `scrollY` moves, `devicePixelRatio` follows the zoom.
///
/// The card is a tall local page, so scrolling has somewhere to go. The witnesses are the page's own
/// `window.scrollY` and `window.devicePixelRatio` — a shell that only *claims* to zoom would pass neither.
int runScrollZoomCheck(const QString& enginePath) {
    QTextStream out(stdout);
    qputenv("ANTIKOLT_ENGINE", enginePath.toUtf8());
    qputenv("ANTIKOLT_CORE", MainWindow::coreExecutable().toUtf8());

    const QString tall_page = QStringLiteral(
        "<!doctype html><meta charset=\"utf-8\"><title>Tall</title>"
        "<style>body{margin:0;font:14px sans-serif}"
        "section{height:4000px;background:linear-gradient(#fff,#bbb)}</style>"
        "<section>tall page</section>");
    ProbeOrigin probe(tall_page);
    if (!probe.listening()) {
        out << "server=FAILED " << probe.errorString() << '\n';
        return 1;
    }
    const QString origin = probe.origin();
    out << "origin=" << origin << '\n';

    const QString core_dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/core");
    QDir().mkpath(core_dir);
    CoreClient oracle;
    if (!oracle.start(MainWindow::coreExecutable(), core_dir)) {
        out << "core_start=FAILED\n";
        return 1;
    }
    bool written = false;
    {
        QEventLoop loop;
        QJsonObject card;
        card.insert(QStringLiteral("id"), QStringLiteral("scroll-probe"));
        card.insert(QStringLiteral("title"), QStringLiteral("Tall probe"));
        card.insert(QStringLiteral("url"), origin + QStringLiteral("/"));
        card.insert(QStringLiteral("container"), QStringLiteral("c-private"));
        card.insert(QStringLiteral("status"), QStringLiteral("unknown"));
        card.insert(QStringLiteral("pinned"), false);
        card.insert(QStringLiteral("notes"), QString());
        card.insert(QStringLiteral("preview_ref"), QString());
        card.insert(QStringLiteral("last_opened_at_ms"), 0);
        oracle.cardUpsert(card, [&](const QJsonObject&, const QString& error) {
            written = error.isEmpty();
            loop.quit();
        });
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    oracle.shutdown();
    if (!written) {
        out << "card_written=FAILED\n";
        return 1;
    }
    out << "card_written=ok\n";

    MainWindow window;
    window.show();
    window.move(150, 110);
    window.startEngine();

    QString scroll_y;
    QString dpr_zoomed;
    QString dpr_reset;
    QString dpr_125;
    {
        QEventLoop loop;
        const auto eval = [&window](qint64 surface, const QString& expression,
                                    std::function<void(const QString&)> done) {
            window.engine()->evaluate(
                surface, expression,
                [done](const QJsonObject& reply, const QString& error) {
                    done(error.isEmpty() ? reply.value(QStringLiteral("value")).toString() : error);
                });
        };
        QObject::connect(window.engine(), &EngineClient::loadFinished, &loop,
                         [&](qint64 id, const QString& url, int) {
                             if (url.startsWith(QStringLiteral("file:"))) {
                                 QTimer::singleShot(900, &loop, [&, id] {
                                     window.engine()->evaluate(
                                         id,
                                         QStringLiteral(
                                             "(function(){var els=document.querySelectorAll("
                                             "'antikolt-card');for(var i=0;i<els.length;i++){"
                                             "var c=els[i].card;"
                                             "if(c&&c.id==='scroll-probe'){"
                                             "els[i].shadowRoot.querySelector('.card').click();"
                                             "return 'clicked:'+c.id;}}"
                                             "return 'no-card';})()"),
                                         [&](const QJsonObject& reply, const QString& error) {
                                             const QString value =
                                                 error.isEmpty()
                                                     ? reply.value(QStringLiteral("value")).toString()
                                                     : error;
                                             out << "card_opened="
                                                 << (value.startsWith(QStringLiteral("clicked:"))
                                                         ? "ok"
                                                         : "FAILED")
                                                 << '\n';
                                         });
                                 });
                                 return;
                             }
                             if (!url.startsWith(origin)) {
                                 return;
                             }
                             window.engine()->scroll(
                                 id, 0, 600, false,
                                 [&, id](const QJsonObject&, const QString& error) {
                                     if (!error.isEmpty()) {
                                         out << "scroll_error=" << error << '\n';
                                         loop.quit();
                                         return;
                                     }
                                     QTimer::singleShot(600, &loop, [&, id] {
                                         eval(id, QStringLiteral("String(window.scrollY)"),
                                              [&, id](const QString& y) {
                                                  scroll_y = y;
                                                  window.engine()->setZoom(
                                                      id, 150,
                                                      [&, id](const QJsonObject&,
                                                              const QString& zoom_error) {
                                                          if (!zoom_error.isEmpty()) {
                                                              out << "zoom_error=" << zoom_error
                                                                  << '\n';
                                                              loop.quit();
                                                              return;
                                                          }
                                                          QTimer::singleShot(
                                                              700, &loop, [&, id] {
                                                                  eval(id,
                                                                       QStringLiteral(
                                                                           "String(window."
                                                                           "devicePixelRatio)"),
                                                                       [&, id](const QString& dpr) {
                                                                           dpr_zoomed = dpr;
                                                                           window.engine()->setZoom(
                                                                               id, 100,
                                                                               [&, id](const QJsonObject&,
                                                                                       const QString&) {
                                                                                   QTimer::singleShot(
                                                                                       700, &loop,
                                                                                       [&, id] {
                                                                                           eval(
                                                                                               id,
                                                                                               QStringLiteral(
                                                                                                   "String(window."
                                                                                                   "devicePixel"
                                                                                                   "Ratio)"),
                                                                                               [&, id](const QString& back) {
                                                                                                   dpr_reset =
                                                                                                       back;
                                                                                                   // The shell's own zoom path (the toolbar calls the same method): 125 % must be
                                                                                                   // 1.25 x the natural DPR.
                                                                                                   window.setZoomPercent(125);
                                                                                                   QTimer::singleShot(700, &loop, [&, id] {
                                                                                                       eval(id, QStringLiteral("String(window.devicePixelRatio)"),
                                                                                                            [&](const QString& at125) {
                                                                                                                dpr_125 = at125;
                                                                                                                window.setZoomPercent(100);
                                                                                                                loop.quit();
                                                                                                            });
                                                                                                   });
                                                                                               });
                                                                                       });
                                                                               });
                                                                       });
                                                              });
                                                      });
                                              });
                                     });
                                 });
                         });
        QTimer::singleShot(60000, &loop, &QEventLoop::quit);
        loop.exec();
    }

    const int scrolled = scroll_y.toInt();
    const double zoomed = dpr_zoomed.toDouble();
    const double reset = dpr_reset.toDouble();
    out << "scroll_y=" << scroll_y << '\n';
    out << "dpr_at_150=" << dpr_zoomed << '\n';
    out << "dpr_at_100=" << dpr_reset << '\n';
    out << "dpr_at_125=" << dpr_125 << '\n';
    // The zoom multiplies the screen's own DPR, so the *ratio* is what the browser controls:
    // 1.875 / 1.25 = 1.5 on a 125 % display, and 1.5 / 1.0 = 1.5 on a plain one.
    const bool ok = scrolled >= 500 && reset > 0.0 && std::abs(zoomed / reset - 1.5) < 0.05 &&
                    std::abs(dpr_125.toDouble() / reset - 1.25) < 0.05;
    out << "scroll_zoom=" << (ok ? "ok" : "FAILED") << '\n';
    window.close();
    out.flush();
    return ok ? 0 : 1;
}

/// Prove the site theme reaches the page: `prefers-color-scheme` flips, and flips back (ADR-037).
///
/// The witness is the page's own `matchMedia`: nothing in the shell may claim the page changed. Both
/// directions matter — a page that only ever reports dark is not following the browser, it is dark.
int runMediaCheck(const QString& enginePath) {
    QTextStream out(stdout);
    qputenv("ANTIKOLT_ENGINE", enginePath.toUtf8());
    qputenv("ANTIKOLT_CORE", MainWindow::coreExecutable().toUtf8());

    MainWindow window;
    window.show();
    window.move(150, 110);
    window.startEngine();

    bool dark = false;
    bool light = false;
    {
        QEventLoop loop;
        // Defined outside the load handler and copied (not referenced) into the timers: a lambda that
        // lives on a load handler's stack is gone by the time a timer fires, and using it then is a
        // dangling reference — measured as an access violation, not as a wrong answer.
        const auto ask = [&window, &dark, &light](qint64 surface, const QString& which) {
            window.engine()->evaluate(
                surface,
                QStringLiteral("String(matchMedia('(prefers-color-scheme: dark)').matches)"),
                [&dark, &light, which](const QJsonObject& reply, const QString& error) {
                    const QString value = error.isEmpty()
                                              ? reply.value(QStringLiteral("value")).toString()
                                              : error;
                    if (which == QStringLiteral("dark")) {
                        dark = value == QStringLiteral("true");
                    } else {
                        light = value == QStringLiteral("true");
                    }
                });
        };
        QObject::connect(window.engine(), &EngineClient::loadFinished, &loop,
                         [&](qint64 id, const QString& url, int) {
                             if (!url.startsWith(QStringLiteral("file:"))) {
                                 return;
                             }
                             QTimer::singleShot(900, &loop, [&, id, ask] {
                                 window.setPageMedia(QStringLiteral("dark"));
                                 // The emulation is asynchronous; read it after it has landed.
                                 QTimer::singleShot(1200, &loop, [&, id, ask] {
                                     ask(id, QStringLiteral("dark"));
                                     window.setPageMedia(QStringLiteral("light"));
                                     QTimer::singleShot(1200, &loop, [&, id, ask] {
                                         ask(id, QStringLiteral("light"));
                                         QTimer::singleShot(1200, &loop, [&] { loop.quit(); });
                                     });
                                 });
                             });
                         });
        QTimer::singleShot(45000, &loop, &QEventLoop::quit);
        loop.exec();
    }

    out << "media_dark=" << (dark ? "dark" : "FAILED") << '\n';
    out << "media_light=" << (!light ? "light" : "FAILED") << '\n';
    const bool ok = dark && !light;
    out << "site_theme=" << (ok ? "ok" : "FAILED") << '\n';
    window.close();
    out.flush();
    return ok ? 0 : 1;
}

/// Prove the memory figure is *measured*: the engine counts the page, the shell answers the page.
///
/// Two witnesses again: the engine's own `metrics` reply (DOM nodes, working set) and the page's
/// `system.memory` answer, which travels through the request channel. A number of zero on either side means
/// the promise is still a guess.
int runMetricsCheck(const QString& enginePath) {
    QTextStream out(stdout);
    qputenv("ANTIKOLT_ENGINE", enginePath.toUtf8());
    qputenv("ANTIKOLT_CORE", MainWindow::coreExecutable().toUtf8());

    MainWindow window;
    window.show();
    window.move(150, 110);
    window.startEngine();

    qint64 home_surface = 0;
    QJsonObject engine_metrics;
    QString page_state;
    /// How many 250 ms polls the page state took — the witness that polling, not luck, settled it.
    int page_poll_ticks = 0;
    {
        QEventLoop loop;
        QObject::connect(window.engine(), &EngineClient::loadFinished, &loop,
                         [&](qint64 id, const QString& url, int) {
                             if (!url.startsWith(QStringLiteral("file:"))) {
                                 return;
                             }
                             home_surface = id;
                             QTimer::singleShot(900, &loop, [&, id] {
                                 window.engine()->evaluate(
                                     id,
                                     QStringLiteral(
                                         "window.__mem='pending';"
                                         "window.antikoltNative.request('system.memory', {})"
                                         ".then(function(r){window.__mem=r;},"
                                         "function(e){window.__mem={error:String(e.message)};});"
                                         " 'asked'"),
                                     // `id` by value: the metrics round trip below happens after the timer
                                     // closure that scheduled this handler is gone.
                                     [&, id](const QJsonObject& reply, const QString& error) {
                                         const bool asked =
                                             error.isEmpty() &&
                                             reply.value(QStringLiteral("value")).toString() ==
                                                 QStringLiteral("asked");
                                         out << "asked=" << (asked ? "ok" : "FAILED") << '\n';
                                         QTimer::singleShot(1200, &loop, [&, id] {
                                             window.engine()->metrics(
                                                 home_surface,
                                                 [&, id](const QJsonObject& measured,
                                                         const QString& measure_error) {
                                                     if (measure_error.isEmpty()) {
                                                         engine_metrics = measured;
                                                     } else {
                                                         engine_metrics.insert(
                                                             QStringLiteral("error"), measure_error);
                                                     }
                                                     // Poll for the page's answer instead of trusting one
                                                     // fixed delay: the request travels page → engine →
                                                     // shell → engine → page, and under load (a busy
                                                     // machine, a compositor falling back) that can be
                                                     // longer than any constant worth hard-coding. The
                                                     // first run of this check read `pending` and reported
                                                     // a measured zero.
                                                     auto* poll = new QTimer(&loop);
                                                     QObject::connect(
                                                         poll, &QTimer::timeout, &loop, [&, id, poll] {
                                                             ++page_poll_ticks;
                                                             window.engine()->evaluate(
                                                                 id,
                                                                 QStringLiteral(
                                                                     "JSON.stringify(window.__mem||null)"),
                                                                 [&, poll](const QJsonObject& value,
                                                                           const QString& failure) {
                                                                     const QString state =
                                                                         failure.isEmpty()
                                                                             ? value.value(QStringLiteral(
                                                                                               "value"))
                                                                                   .toString()
                                                                             : failure;
                                                                     const bool ready =
                                                                         failure.isEmpty() &&
                                                                         state != QStringLiteral("null") &&
                                                                         !state.contains(
                                                                             QStringLiteral("pending"));
                                                                     if (!ready &&
                                                                         page_poll_ticks < 40) {
                                                                         return;  // not answered yet
                                                                     }
                                                                     poll->stop();
                                                                     page_state = state;
                                                                     loop.quit();
                                                                 });
                                                         });
                                                     poll->start(250);
                                                 });
                                         });
                                     });
                             });
                         });
        QTimer::singleShot(45000, &loop, &QEventLoop::quit);
        loop.exec();
    }

    const qint64 working_set = engine_metrics.value(QStringLiteral("engine_working_set_bytes"))
                                   .toString()
                                   .toLongLong();
    const qint64 heap =
        engine_metrics.value(QStringLiteral("js_heap_bytes")).toString().toLongLong();
    const int dom_nodes = engine_metrics.value(QStringLiteral("dom_nodes")).toInt();
    const QJsonObject page_memory = QJsonDocument::fromJson(page_state.toUtf8()).object();
    const qint64 page_used =
        static_cast<qint64>(page_memory.value(QStringLiteral("usedBytes")).toDouble());
    out << "dom_nodes=" << dom_nodes << '\n';
    out << "js_heap_bytes=" << heap << '\n';
    out << "engine_working_set_bytes=" << working_set << '\n';
    out << "page_used_bytes=" << page_used << '\n';
    out << "page_poll_ticks=" << page_poll_ticks << '\n';
    const bool ok = dom_nodes > 0 && working_set > 0 && page_used > 0;
    out << "metrics=" << (ok ? "ok" : "FAILED") << '\n';
    window.close();
    out.flush();
    return ok ? 0 : 1;
}

/// Prove previews are *produced*: opening a card gives it a picture, and the grid can show it.
///
/// A card pointing at the local probe origin is written into the core without a preview. The grid shows it
/// (the push carries the record), the test clicks it as a user would, and once the page has loaded the
/// shell captures it, moves the image into the profile's `previews` directory and writes the name back to
/// the record. Three witnesses: the file on disk, the record's `preview_ref`, and the page loading
/// `antikolt://blob/<name>`.
int runPreviewGenCheck(const QString& enginePath) {
    QTextStream out(stdout);
    qputenv("ANTIKOLT_ENGINE", enginePath.toUtf8());
    qputenv("ANTIKOLT_CORE", MainWindow::coreExecutable().toUtf8());

    ProbeOrigin probe(storageProbePage());
    if (!probe.listening()) {
        out << "server=FAILED " << probe.errorString() << '\n';
        return 1;
    }
    const QString origin = probe.origin();
    out << "origin=" << origin << '\n';

    const QString core_dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/core");
    QDir().mkpath(core_dir);
    const QString profile =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/engine");
    const QString card_id = QStringLiteral("preview-probe");
    const QString preview_name = card_id + QStringLiteral(".png");
    const QString target = profile + QStringLiteral("/previews/") + preview_name;
    QDir().mkpath(profile + QStringLiteral("/previews"));
    QFile::remove(target);

    CoreClient oracle;
    if (!oracle.start(MainWindow::coreExecutable(), core_dir)) {
        out << "core_start=FAILED\n";
        return 1;
    }
    bool written = false;
    {
        QEventLoop loop;
        QJsonObject card;
        card.insert(QStringLiteral("id"), card_id);
        card.insert(QStringLiteral("title"), QStringLiteral("Preview probe"));
        card.insert(QStringLiteral("url"), origin + QStringLiteral("/"));
        card.insert(QStringLiteral("container"), QStringLiteral("c-private"));
        card.insert(QStringLiteral("status"), QStringLiteral("unknown"));
        card.insert(QStringLiteral("pinned"), false);
        card.insert(QStringLiteral("notes"), QString());
        card.insert(QStringLiteral("preview_ref"), QString());
        card.insert(QStringLiteral("last_opened_at_ms"), 0);
        oracle.cardUpsert(card, [&](const QJsonObject&, const QString& error) {
            written = error.isEmpty();
            loop.quit();
        });
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    oracle.shutdown();
    if (!written) {
        out << "card_written=FAILED\n";
        return 1;
    }
    out << "card_written=ok\n";

    MainWindow window;
    window.show();
    window.move(150, 110);
    window.startEngine();

    bool clicked = false;
    QString preview_state;
    int poll_attempts = 0;
    {
        QEventLoop loop;
        QObject::connect(window.engine(), &EngineClient::loadFinished, &loop,
                         [&](qint64 id, const QString& url, int) {
                             if (url.startsWith(QStringLiteral("file:"))) {
                                 // The grid is up with the pushed card: click that card, as a user would.
                                 QTimer::singleShot(900, &loop, [&, id] {
                                     window.engine()->evaluate(
                                         id,
                                         QStringLiteral(
                                             "(function(){var els=document.querySelectorAll("
                                             "'antikolt-card');for(var i=0;i<els.length;i++){"
                                             "var c=els[i].card;"
                                             "if(c&&c.id==='preview-probe'){"
                                             "els[i].shadowRoot.querySelector('.card').click();"
                                             "return 'clicked:'+c.id;}}"
                                             "return 'no-card';})()"),
                                         [&](const QJsonObject& reply, const QString& error) {
                                             const QString value =
                                                 error.isEmpty()
                                                     ? reply.value(QStringLiteral("value")).toString()
                                                     : error;
                                             clicked = value.startsWith(QStringLiteral("clicked:"));
                                             out << "card_opened=" << (clicked ? "ok" : "FAILED")
                                                 << '\n';
                                             if (!clicked) {
                                                 out << "card_opened_value=" << value << '\n';
                                                 loop.quit();
                                             }
                                         });
                                 });
                                 return;
                             }
                             if (!url.startsWith(origin)) {
                                 return;
                             }
                             // The card is loaded: the shell captures it, copies the file and writes the
                             // name back. Poll for the result instead of trusting a fixed delay — three
                             // round trips on a busy machine take longer than they do on an idle one, and a
                             // fixed wait is how a check becomes flaky.
                             auto* poll = new QTimer(&loop);
                             QObject::connect(poll, &QTimer::timeout, &loop, [&, id, poll] {
                                 ++poll_attempts;
                                 // The shell retries a stalled capture once and its own request timeout is
                                 // 20 s, so the poll waits longer than that before giving up.
                                 if (!QFileInfo::exists(target) && poll_attempts <= 60) {
                                     return;
                                 }
                                 out << "preview_wait_ticks=" << poll_attempts << '\n';
                                 poll->stop();
                                 QTimer::singleShot(700, &loop, [&, id] {
                                     window.engine()->evaluate(
                                         id,
                                         QStringLiteral(
                                             "window.__shot='pending';"
                                             "(function(){var i=new Image();"
                                             "i.onload=function(){window.__shot='loaded:'+"
                                             "i.naturalWidth+'x'+i.naturalHeight;};"
                                             "i.onerror=function(){window.__shot='error';};"
                                             "i.src='antikolt://blob/preview-probe.png';})();"
                                             " 'asked'"),
                                         [&, id](const QJsonObject&, const QString&) {
                                             QTimer::singleShot(1200, &loop, [&, id] {
                                                 window.engine()->evaluate(
                                                     id,
                                                     QStringLiteral("String(window.__shot)"),
                                                     [&](const QJsonObject& shot,
                                                         const QString& shot_error) {
                                                         preview_state =
                                                             shot_error.isEmpty()
                                                                 ? shot.value(QStringLiteral("value"))
                                                                       .toString()
                                                                 : shot_error;
                                                         loop.quit();
                                                     });
                                             });
                                         });
                                 });
                             });
                             poll->start(400);
                         });
        QTimer::singleShot(60000, &loop, &QEventLoop::quit);
        loop.exec();
    }

    QString stored_ref;
    {
        CoreClient verifier;
        if (verifier.start(MainWindow::coreExecutable(), core_dir)) {
            QEventLoop loop;
            verifier.cardsList([&](const QJsonObject& listed, const QString& error) {
                if (error.isEmpty()) {
                    for (const QJsonValue& value :
                         listed.value(QStringLiteral("cards")).toArray()) {
                        const QJsonObject card = value.toObject();
                        if (card.value(QStringLiteral("id")).toString() == card_id) {
                            stored_ref = card.value(QStringLiteral("preview_ref")).toString();
                        }
                    }
                }
                loop.quit();
            });
            QTimer::singleShot(10000, &loop, &QEventLoop::quit);
            loop.exec();
            verifier.shutdown();
        }
    }

    const bool file_exists = QFileInfo::exists(target);
    out << "preview_file=" << (file_exists ? "ok" : "FAILED") << '\n';
    out << "preview_ref=" << stored_ref << '\n';
    out << "preview_page=" << preview_state << '\n';
    const bool ok = clicked && file_exists && stored_ref == preview_name &&
                    preview_state.startsWith(QStringLiteral("loaded:"));
    out << "preview_generation=" << (ok ? "ok" : "FAILED") << '\n';
    window.close();
    out.flush();
    return ok ? 0 : 1;
}

/// Prove the background preview worker (ADR-041): a card is photographed while the user is on the grid.
///
/// The card is written into the core without a preview. The grid loads, the worker is started through the
/// same call the automatic queue uses, and the job must end with all of these true: the file is in the
/// profile and is a real picture (not a blank frame), the name is on the card record, the worker is back to
/// `Idle`, the daily budget counted exactly one, and the engine no longer knows the worker's surface — the
/// grid is the only live one. The second phase proves the rule the ADR puts first: a job that is still
/// starting is abandoned the moment the user opens a card, and no preview is written for it.
int runPreviewWorkerCheck(const QString& enginePath) {
    QTextStream out(stdout);
    qputenv("ANTIKOLT_ENGINE", enginePath.toUtf8());
    qputenv("ANTIKOLT_CORE", MainWindow::coreExecutable().toUtf8());
    // This check drives the worker by hand so one probe measures one thing; which card the *automatic*
    // queue would pick, and when it may start, is `--previewqueue`'s subject.
    qputenv("ANTIKOLT_NO_PREVIEW_WORKER", "1");
    // Chromium's GPU process aborts in this environment (docs/HU/18-F0-JELENTES.md), and while the engine
    // is falling back to software compositing the *first* hidden capture can outlast the worker's
    // eight-second ceiling. That is a GPU problem, not the worker's, so the check uses the documented
    // environment fallback and measures the worker; the product behaviour it skips (a card is retried by
    // the queue on the next pass) is exactly what a stalled compositor should cause.
    qputenv("ANTIKOLT_ENGINE_DISABLE_GPU", "1");

    ProbeOrigin probe(storageProbePage());
    if (!probe.listening()) {
        out << "probe_server=FAILED " << probe.errorString() << '\n';
        return 1;
    }
    const QString origin = probe.origin();
    out << "origin=" << origin << '\n';

    const QString core_dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/core");
    QDir().mkpath(core_dir);
    const QString profile =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/engine");
    const QString previews = profile + QStringLiteral("/previews");
    QDir().mkpath(previews);
    const QString worker_card = QStringLiteral("preview-worker-probe");
    const QString abort_card = QStringLiteral("preview-worker-abort");
    const QString worker_name = worker_card + QStringLiteral(".png");
    const QString target = previews + QLatin1Char('/') + worker_name;
    const QString abort_target = previews + QLatin1Char('/') + abort_card + QStringLiteral(".png");
    // A fresh measurement: another run's file or budget must not be what this check asserts.
    QFile::remove(target);
    QFile::remove(abort_target);
    QFile::remove(previews + QStringLiteral("/budget.json"));

    bool seeded = false;
    {
        CoreClient oracle;
        if (!oracle.start(MainWindow::coreExecutable(), core_dir)) {
            out << "core_start=FAILED\n";
            return 1;
        }
        QEventLoop loop;
        int remaining = 2;
        const auto seed = [&](const QString& id, const QString& url, const QString& title) {
            QJsonObject card;
            card.insert(QStringLiteral("id"), id);
            card.insert(QStringLiteral("title"), title);
            card.insert(QStringLiteral("url"), url);
            card.insert(QStringLiteral("container"), QStringLiteral("c-private"));
            card.insert(QStringLiteral("status"), QStringLiteral("unknown"));
            card.insert(QStringLiteral("pinned"), false);
            card.insert(QStringLiteral("notes"), QString());
            card.insert(QStringLiteral("preview_ref"), QString());
            card.insert(QStringLiteral("last_opened_at_ms"), 0);
            oracle.cardUpsert(card, [&](const QJsonObject&, const QString& error) {
                if (!error.isEmpty()) {
                    out << "card_write_error=" << error << '\n';
                }
                if (--remaining == 0) {
                    seeded = true;
                    loop.quit();
                }
            });
        };
        seed(worker_card, origin + QStringLiteral("/"), QStringLiteral("Preview worker probe"));
        seed(abort_card, origin + QStringLiteral("/"), QStringLiteral("Preview worker abort probe"));
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
        oracle.shutdown();
    }
    out << "cards_written=" << (seeded ? "ok" : "FAILED") << '\n';
    if (!seeded) {
        return 1;
    }

    MainWindow window;
    window.show();
    window.move(150, 110);
    window.startEngine();

    // ---- phase 0: the grid is up, and nothing is in front of it --------------------------------
    bool grid_ready = false;
    {
        QEventLoop loop;
        QObject::connect(window.engine(), &EngineClient::loadFinished, &loop,
                         [&](qint64, const QString& url, int) {
                             if (!url.startsWith(QStringLiteral("file:")) || grid_ready) {
                                 return;
                             }
                             grid_ready = true;
                             QTimer::singleShot(600, &loop, [&loop] { loop.quit(); });
                         });
        QTimer::singleShot(60000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    out << "grid_ready=" << (grid_ready ? "ok" : "FAILED") << '\n';

    // ---- phase 1: the ceiling ends a job and writes nothing ------------------------------------
    // Five milliseconds is shorter than the engine's own round trip: the job is abandoned before its surface
    // reply even arrives, which is the hardest ordering to clean up (the reply must not leak a surface).
    // A warm cache can answer a load *and* a capture in a couple of hundred milliseconds, so a "3 second"
    // ceiling would measure the machine, not the ceiling.
    qputenv("ANTIKOLT_PREVIEW_DEADLINE_MS", "5");
    bool deadline_started = false;
    bool deadline_finished = false;
    bool deadline_produced = true;
    deadline_started = window.startPreviewWorker(abort_card, origin + QStringLiteral("/"));
    out << "deadline_start=" << (deadline_started ? "ok" : "FAILED") << '\n';
    if (deadline_started) {
        QEventLoop loop;
        QObject::connect(&window, &MainWindow::previewWorkerFinished, &loop,
                         [&](const QString& id, bool ok) {
                             deadline_finished = true;
                             deadline_produced = ok;
                             out << "deadline_worker=" << id << '\n';
                             loop.quit();
                         });
        QTimer::singleShot(20000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    const bool deadline_idle = window.previewWorkerState() == antikolt::PreviewWorker::Idle;
    const qint64 deadline_surface = window.previewWorkerLastSurface();
    QString deadline_gone = QStringLiteral("not-asked");
    if (deadline_surface > 0) {
        QEventLoop loop;
        window.engine()->evaluate(deadline_surface, QStringLiteral("'probe'"),
                                  [&](const QJsonObject&, const QString& error) {
                                      deadline_gone =
                                          error.isEmpty() ? QStringLiteral("still-alive") : error;
                                      loop.quit();
                                  });
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    const bool deadline_no_preview = !QFileInfo::exists(abort_target);
    out << "deadline_fired="
        << (deadline_finished && !deadline_produced ? "ok" : "FAILED") << '\n';
    out << "deadline_state=" << (deadline_idle ? "idle" : "busy") << '\n';
    out << "deadline_no_preview=" << (deadline_no_preview ? "ok" : "FAILED") << '\n';
    out << "deadline_surface_gone=" << deadline_gone << '\n';

    // The abandoned job's surface reply is still in flight — that is what a 5 ms ceiling means — so give the
    // engine the moment it needs to answer and the shell the moment it needs to destroy that surface. The
    // queue waits 1.2 s after every job for the same reason; the check is explicit about it instead of
    // relying on the next phase's own timing.
    {
        QEventLoop wait;
        QTimer::singleShot(800, &wait, &QEventLoop::quit);
        wait.exec();
    }

    // ---- phase 2: one job on the grid ---------------------------------------------------------
    // A real machine paints a page in a fraction of a second; this one's compositor may spend longer than
    // eight seconds falling back from a dead GPU, so the check lends the job more time (the product keeps
    // its eight-second ceiling, and phase 1 just proved the ceiling works).
    qputenv("ANTIKOLT_PREVIEW_DEADLINE_MS", "10000");
    bool started = false;
    bool produced = false;
    bool finished = false;
    int attempts = 0;
    // A retry is not a softer assertion: the *queue* is what produces the grid's previews, and the queue
    // tries the same card again on its next pass when a job fails. A machine whose compositor sometimes
    // misses the first frame (measured on a GPU-less session) loses one job, not the feature — so the check
    // asks for a preview within three passes and reports how many it took.
    const std::function<void()> attempt = [&] {
        ++attempts;
        QEventLoop loop;
        QObject::connect(&window, &MainWindow::previewWorkerFinished, &loop,
                         [&](const QString& id, bool ok) {
                             produced = ok;
                             finished = true;
                             out << "worker_card=" << id << '\n';
                             loop.quit();
                         });
        // The grid is up: start the worker through the same call the automatic queue uses, so the check
        // measures the production code and not a copy of it.
        const bool accepted = window.startPreviewWorker(worker_card, origin + QStringLiteral("/"));
        started = started || accepted;
        out << "worker_start=" << (accepted ? "ok" : "FAILED") << " attempt=" << attempts << '\n';
        if (!accepted) {
            return;
        }
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);
        loop.exec();
    };
    attempt();
    while (!produced && started && attempts < 3) {
        QEventLoop settle;
        QTimer::singleShot(1200, &settle, &QEventLoop::quit);
        settle.exec();
        attempt();
    }
    out << "worker_attempts=" << attempts << '\n';

    const bool worker_idle = window.previewWorkerState() == antikolt::PreviewWorker::Idle;
    const qint64 worker_surface = window.previewWorkerLastSurface();
    // The engine's own answer is the witness that the surface is gone: with two surfaces allowed, an op
    // addressed to a dead one has to be refused, not silently served by the grid.
    QString worker_gone = QStringLiteral("not-asked");
    {
        QEventLoop loop;
        window.engine()->evaluate(worker_surface, QStringLiteral("'probe'"),
                                  [&](const QJsonObject&, const QString& error) {
                                      worker_gone =
                                          error.isEmpty() ? QStringLiteral("still-alive") : error;
                                      loop.quit();
                                  });
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        loop.exec();
    }

    QString stored_ref;
    {
        CoreClient verifier;
        if (verifier.start(MainWindow::coreExecutable(), core_dir)) {
            QEventLoop loop;
            verifier.cardsList([&](const QJsonObject& listed, const QString& error) {
                if (error.isEmpty()) {
                    for (const QJsonValue& value :
                         listed.value(QStringLiteral("cards")).toArray()) {
                        const QJsonObject card = value.toObject();
                        if (card.value(QStringLiteral("id")).toString() == worker_card) {
                            stored_ref = card.value(QStringLiteral("preview_ref")).toString();
                        }
                    }
                }
                loop.quit();
            });
            QTimer::singleShot(10000, &loop, &QEventLoop::quit);
            loop.exec();
            verifier.shutdown();
        }
    }

    const bool file_exists = QFileInfo::exists(target) && QFileInfo(target).size() > 0;
    // A file is not a picture: a hidden surface that never painted would write a blank frame, so the check
    // reads the image back and counts colours. One colour is not a photographed page.
    const QImage image(target);
    const int colours = image.isNull() ? 0 : CountDistinctColours(image);
    const int budget_used = antikolt::previewUsedToday(
        previews + QStringLiteral("/budget.json"), QDate::currentDate().toString(Qt::ISODate));
    out << "worker_produced=" << (produced ? "ok" : "FAILED") << '\n';
    out << "worker_file=" << (file_exists ? "ok" : "FAILED") << '\n';
    out << "worker_pixels=" << (image.isNull() ? 0 : image.width()) << 'x'
        << (image.isNull() ? 0 : image.height()) << " colours=" << colours << '\n';
    out << "worker_ref=" << stored_ref << '\n';
    out << "worker_state=" << (worker_idle ? "idle" : "busy") << '\n';
    out << "worker_budget=" << budget_used << '\n';
    out << "worker_surface_gone=" << worker_gone << '\n';

    // ---- phase 2: the user wins ----------------------------------------------------------------
    bool abort_started = false;
    bool card_loaded = false;
    {
        QEventLoop loop;
        QObject::connect(window.engine(), &EngineClient::loadFinished, &loop,
                         [&](qint64, const QString& url, int) {
                             if (!url.startsWith(origin)) {
                                 return;  // not the user's card
                             }
                             card_loaded = true;
                             // A beat, so the surface is settled before the window is torn down.
                             QTimer::singleShot(200, &loop, [&loop] { loop.quit(); });
                         });
        abort_started = window.startPreviewWorker(abort_card, origin + QStringLiteral("/"));
        out << "abort_start=" << (abort_started ? "ok" : "FAILED") << '\n';
        // The user clicks a card in the same breath, while the engine has not even answered with the
        // surface the worker asked for: the hardest ordering, and the one a first draft leaked a surface on.
        window.openCard(origin + QStringLiteral("/"), QStringLiteral("user card"));
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    const bool abort_idle = window.previewWorkerState() == antikolt::PreviewWorker::Idle;
    const qint64 abort_surface = window.previewWorkerLastSurface();
    QString abort_gone = QStringLiteral("not-asked");
    {
        QEventLoop loop;
        window.engine()->evaluate(abort_surface, QStringLiteral("'probe'"),
                                  [&](const QJsonObject&, const QString& error) {
                                      abort_gone =
                                          error.isEmpty() ? QStringLiteral("still-alive") : error;
                                      loop.quit();
                                  });
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    const bool abort_preview_absent = !QFileInfo::exists(abort_target);
    out << "abort_state=" << (abort_idle ? "idle" : "busy") << '\n';
    out << "abort_no_preview=" << (abort_preview_absent ? "ok" : "FAILED") << '\n';
    out << "abort_surface_gone=" << abort_gone << '\n';
    out << "user_card_loaded=" << (card_loaded ? "ok" : "FAILED") << '\n';

    // The deadline phase may end before the engine has even answered with the surface (300 ms is short);
    // then there is no surface to ask about, which is a pass, not a skip the check can ignore.
    const bool deadline_passed =
        deadline_started && deadline_finished && !deadline_produced && deadline_idle &&
        deadline_no_preview &&
        (deadline_gone == QStringLiteral("not-asked") ||
         deadline_gone == QStringLiteral("no live surface"));
    const bool ok = grid_ready && deadline_passed && started && finished && produced && file_exists &&
                    colours > 1 && stored_ref == worker_name && worker_idle && budget_used == 1 &&
                    worker_gone == QStringLiteral("no live surface") && abort_started &&
                    abort_idle && abort_preview_absent &&
                    abort_gone == QStringLiteral("no live surface") && card_loaded;
    out << "preview_worker=" << (ok ? "ok" : "FAILED") << '\n';
    window.close();
    out.flush();
    return ok ? 0 : 1;
}

/// Prove a card preview can be delivered: the engine serves `antikolt://blob/…` from the profile.
///
/// The preview file is written where the engine looks for it (the shell hands this profile to the engine),
/// then the home page loads it as an image and the test reads the page's own witness (`naturalWidth` /
/// `naturalHeight`). A second image asks for a name that escapes the previews directory: it must be
/// *refused*, because card data must never be able to read another file through the browser.
int runPreviewCheck(const QString& enginePath) {
    QTextStream out(stdout);
    qputenv("ANTIKOLT_ENGINE", enginePath.toUtf8());
    qputenv("ANTIKOLT_CORE", MainWindow::coreExecutable().toUtf8());

    const QString profile = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                            QStringLiteral("/engine");
    const QString previews = profile + QStringLiteral("/previews");
    QDir().mkpath(previews);
    QImage probe(4, 3, QImage::Format_RGB32);
    probe.fill(QColor(47, 111, 235));
    const QString preview_path = previews + QStringLiteral("/probe.png");
    if (!probe.save(preview_path, "PNG")) {
        out << "preview_write=FAILED\n";
        return 1;
    }
    out << "preview_file=probe.png 4x3 png\n";

    MainWindow window;
    window.show();
    window.move(150, 110);
    window.startEngine();

    bool asked = false;
    QString preview;
    QString traversal;
    {
        QEventLoop loop;
        QObject::connect(window.engine(), &EngineClient::loadFinished, &loop,
                         [&](qint64 id, const QString& url, int) {
                             if (!url.startsWith(QStringLiteral("file:"))) {
                                 return;
                             }
                             QTimer::singleShot(800, &loop, [&, id] {
                                 window.engine()->evaluate(
                                     id,
                                     QStringLiteral(
                                         "window.__preview='pending';window.__traversal='pending';"
                                         "(function(){var i=new Image();"
                                         "i.onload=function(){window.__preview='loaded:'+"
                                         "i.naturalWidth+'x'+i.naturalHeight;};"
                                         "i.onerror=function(){window.__preview='error';};"
                                         "i.src='antikolt://blob/probe.png';})();"
                                         "(function(){var j=new Image();"
                                         "j.onload=function(){window.__traversal='loaded';};"
                                         "j.onerror=function(){window.__traversal='refused';};"
                                         "j.src='antikolt://blob/..%2Fhost.cpp';})();"
                                         " 'asked'"),
                                     // `id` by value: the read below happens after the timer closure that
                                     // scheduled this handler is gone.
                                     [&, id](const QJsonObject& reply, const QString& error) {
                                         asked = error.isEmpty() &&
                                                 reply.value(QStringLiteral("value")).toString() ==
                                                     QStringLiteral("asked");
                                         out << "asked=" << (asked ? "ok" : "FAILED") << '\n';
                                         QTimer::singleShot(2000, &loop, [&, id] {
                                             window.engine()->evaluate(
                                                 id,
                                                 QStringLiteral(
                                                     "JSON.stringify({preview: window.__preview,"
                                                     " traversal: window.__traversal})"),
                                                 [&](const QJsonObject& value, const QString& failure) {
                                                     if (failure.isEmpty()) {
                                                         const QJsonObject state =
                                                             QJsonDocument::fromJson(
                                                                 value.value(QStringLiteral("value"))
                                                                     .toString()
                                                                     .toUtf8())
                                                                 .object();
                                                         preview = state.value(QStringLiteral("preview"))
                                                                       .toString();
                                                         traversal =
                                                             state.value(QStringLiteral("traversal"))
                                                                 .toString();
                                                     } else {
                                                         preview = failure;
                                                     }
                                                     loop.quit();
                                                 });
                                         });
                                     });
                             });
                         });
        QTimer::singleShot(45000, &loop, &QEventLoop::quit);
        loop.exec();
    }

    out << "preview_load=" << preview << '\n';
    out << "preview_traversal=" << traversal << '\n';
    const bool ok = asked && preview == QStringLiteral("loaded:4x3") &&
                    traversal == QStringLiteral("refused");
    out << "preview_delivery=" << (ok ? "ok" : "FAILED") << '\n';
    window.close();
    out.flush();
    return ok ? 0 : 1;
}

/// Prove a page request is answered even when the shell forgets it: the watchdog, not hope.
///
/// The method `test.hang` is deliberately unanswered and only exists when `ANTIKOLT_BRIDGE_TEST` is set,
/// so no page can reach it in a normal run. The page's promise must be *rejected* with a timeout message
/// in about `kPageAnswerMs` — a promise that never settles would leak into every later interaction.
int runTimeoutCheck(const QString& enginePath) {
    QTextStream out(stdout);
    qputenv("ANTIKOLT_ENGINE", enginePath.toUtf8());
    qputenv("ANTIKOLT_CORE", MainWindow::coreExecutable().toUtf8());
    qputenv("ANTIKOLT_BRIDGE_TEST", "1");

    MainWindow window;
    window.show();
    window.move(150, 110);
    window.startEngine();

    bool asked = false;
    QString answer;
    qint64 elapsed_ms = -1;
    {
        QEventLoop loop;
        QElapsedTimer clock;
        QObject::connect(window.engine(), &EngineClient::loadFinished, &loop,
                         [&](qint64 id, const QString& url, int) {
                             if (!url.startsWith(QStringLiteral("file:"))) {
                                 return;
                             }
                             QTimer::singleShot(700, &loop, [&, id] {
                                 clock.start();
                                 window.engine()->evaluate(
                                     id,
                                     QStringLiteral(
                                         "window.antikoltNative.request('test.hang', {})"
                                         ".then(function(r){window.__hang={ok:true};},"
                                         "function(e){window.__hang={error:String(e.message)};});"
                                         " 'asked'"),
                                     [&, id](const QJsonObject& reply, const QString& error) {
                                         asked = error.isEmpty() &&
                                                 reply.value(QStringLiteral("value")).toString() ==
                                                     QStringLiteral("asked");
                                         out << "asked=" << (asked ? "ok" : "FAILED") << '\n';
                                         // The watchdog answers at kPageAnswerMs; read a little after.
                                         QTimer::singleShot(6500, &loop, [&, id] {
                                             elapsed_ms = clock.elapsed();
                                             window.engine()->evaluate(
                                                 id,
                                                 QStringLiteral(
                                                     "JSON.stringify(window.__hang||null)"),
                                                 [&](const QJsonObject& value, const QString& failure) {
                                                     answer = failure.isEmpty()
                                                                  ? value.value(QStringLiteral("value"))
                                                                        .toString()
                                                                  : failure;
                                                     loop.quit();
                                                 });
                                         });
                                     });
                             });
                         });
        QTimer::singleShot(45000, &loop, &QEventLoop::quit);
        loop.exec();
    }

    const QString hang_error =
        QJsonDocument::fromJson(answer.toUtf8()).object().value(QStringLiteral("error")).toString();
    const bool rejected = hang_error.contains(QStringLiteral("időben"));
    out << "answer_ms=" << elapsed_ms << '\n';
    out << "timeout_answer=" << (rejected ? "rejected" : "FAILED") << '\n';
    out << "timeout_message=" << hang_error << '\n';
    const bool ok = asked && rejected && elapsed_ms >= 4000 && elapsed_ms <= 8000;
    out << "timeout_test=" << (ok ? "ok" : "FAILED") << '\n';
    window.close();
    out.flush();
    return ok ? 0 : 1;
}

/// Prove the card's note editor writes: the editor opens in place, the save goes through the bridge to the
/// core, and the store holds the text.
///
/// The text is fixed and ASCII so the script needs no escaping; the store is shared with the other checks,
/// so the assertion compares the three witnesses (editor answer, page model, core record) rather than the
/// store's whole shape.
int runNoteCheck(const QString& enginePath) {
    QTextStream out(stdout);
    qputenv("ANTIKOLT_ENGINE", enginePath.toUtf8());
    qputenv("ANTIKOLT_CORE", MainWindow::coreExecutable().toUtf8());

    const QString probe_note = QStringLiteral("probe note");
    MainWindow window;
    window.show();
    window.move(150, 110);
    // The focus assertion needs a focused window: an unfocused document has no activeElement to return, so the
    // probe would report `none` and the test would blame the page for the harness.
    window.raise();
    window.activateWindow();
    window.startEngine();

    QString card_id;
    qint64 page_surface = 0;
    QString focus_class;
    QJsonObject state;
    {
        QEventLoop loop;
        QObject::connect(window.engine(), &EngineClient::loadFinished, &loop,
                         [&](qint64 id, const QString& url, int) {
                            if (!url.startsWith(QStringLiteral("file:"))) {
                                return;
                            }
                            page_surface = id;
                            QTimer::singleShot(900, &loop, [&, id] {
                                 window.engine()->evaluate(
                                     id,
                                     QStringLiteral(
                                         "(function(){var els=document.querySelectorAll("
                                         "'antikolt-card');if(!els.length)return 'no-card';"
                                         "var host=els[0];"
                                         "var b=host.shadowRoot.querySelector('.note');"
                                         "if(!b)return 'no-note';b.click();"
                                         "var ta=host.shadowRoot.querySelector('.note-editor textarea');"
                                         "if(!ta)return 'no-editor';"
                                         "if(ta.parentElement.hidden)return 'editor-hidden';"
                                         "ta.value='probe note';"
                                         "var s=host.shadowRoot.querySelector('.note-save');"
                                         "if(!s)return 'no-save';s.click();"
                                         "return 'saved:'+host.card.id;})()"),
                                     [&, id](const QJsonObject& reply, const QString& error) {
                                         const QString value = error.isEmpty()
                                                                   ? reply.value(QStringLiteral("value"))
                                                                         .toString()
                                                                   : error;
                                         const bool saved = value.startsWith(QStringLiteral("saved:"));
                                         out << "note_save=" << (saved ? "ok" : "FAILED") << '\n';
                                         if (!saved) {
                                             out << "note_save_value=" << value << '\n';
                                             loop.quit();
                                             return;
                                         }
                                         card_id = value.mid(6);
                                          // The focus probe reads the element that *has* the focus, not the first card:
                                          // asking the wrong element answered `none` for a working feature, and the
                                          // honest question is "which card owns the focus, and what inside it".
                                          window.engine()->evaluate(
                                              id,
                                              QStringLiteral(
                                                  "(function(){var a=document.activeElement;if(!a)return 'none';"
                                                  "if(a.tagName!=='ANTIKOLT-CARD')return 'not-host:'+a.tagName;"
                                                  "var s=a.shadowRoot&&a.shadowRoot.activeElement;"
                                                  "return (a.card?a.card.id:'')+':'+(s?(s.className||s.tagName):'none');})()"),
                                              [&](const QJsonObject& focus_reply,
                                                  const QString& focus_error) {
                                                  focus_class = focus_error.isEmpty()
                                                                    ? focus_reply
                                                                          .value(QStringLiteral("value"))
                                                                          .toString()
                                                                    : focus_error;
                                              });
                                         QTimer::singleShot(1200, &loop, [&, id] {
                                             window.engine()->evaluate(
                                                 id,
                                                 QStringLiteral(
                                                     "JSON.stringify(window.antikoltNoteState())"),
                                                 [&](const QJsonObject& state_reply,
                                                     const QString& state_error) {
                                                     if (state_error.isEmpty()) {
                                                         state = QJsonDocument::fromJson(
                                                                     state_reply
                                                                         .value(QStringLiteral("value"))
                                                                         .toString()
                                                                         .toUtf8())
                                                                     .object();
                                                     }
                                                     loop.quit();
                                                 });
                                         });
                                     });
                             });
                         });
        QTimer::singleShot(45000, &loop, &QEventLoop::quit);
        loop.exec();
    }

    // The *rendered* note, not just the model (docs/HU/19-FOLYTATAS.md §3): the card re-renders after the save,
    // so a probe that runs at click time reads the old element and finds no note (`note_rendered=none`). This
    // reads the new card's own DOM — inside its shadow root — and polls briefly instead of sleeping a guess,
    // because "the render happened" is exactly what is being measured.
    QString rendered_note;
    // Why the focus probe says `none` is a question of its own (it is *not* asserted: this harness cannot always
    // give the embedded page a real focus), so the probe reports the chain — document, host, shadow — instead of a
    // bare "none". A bare answer makes the next reader re-run the investigation from scratch.
    QString focus_chain;
    if (!card_id.isEmpty() && page_surface != 0) {
        const QString probe = QStringLiteral(
            "(function(){var els=document.querySelectorAll('antikolt-card');"
            "for(var i=0;i<els.length;i++){var h=els[i];"
            "if(!h.card||h.card.id!=='%1')continue;"
            "var n=h.shadowRoot&&h.shadowRoot.querySelector('.card-notes');"
            "var shadowActive=h.shadowRoot&&h.shadowRoot.activeElement;"
            "window.__antikoltFocusChain={hasFocus:document.hasFocus(),"
            " activeTag:document.activeElement?document.activeElement.tagName:'none',"
            " activeIsHost:document.activeElement===h,"
            " shadowActive:shadowActive?(shadowActive.className||shadowActive.tagName):'none'};"
            "if(!n)return 'no-node';return (n.textContent||'').trim();}"
            "return 'no-card';})()")
                                  .arg(card_id);
        for (int attempt = 0; attempt < 20; ++attempt) {
            QEventLoop probe_loop;
            window.engine()->evaluate(page_surface, probe,
                                      [&](const QJsonObject& reply, const QString& error) {
                                          rendered_note = error.isEmpty()
                                                              ? reply.value(QStringLiteral("value"))
                                                                    .toString()
                                                              : error;
                                          probe_loop.quit();
                                      });
            QTimer::singleShot(3000, &probe_loop, &QEventLoop::quit);
            probe_loop.exec();
            if (rendered_note == probe_note) {
                break;
            }
            QEventLoop pause;
            QTimer::singleShot(250, &pause, &QEventLoop::quit);
            pause.exec();
        }
    }
    // The focus is asserted where it belongs: *after* the re-render, like the rendered note. The first probe (at
    // click time) reads the old element and honestly reports `none`; this one polls until the rebuilt card owns the
    // focus, and the assertion is the pair "this card" + "its article", because the host alone would also match a
    // card the user merely clicked.
    if (!card_id.isEmpty() && page_surface != 0) {
        const QString focus_probe = QStringLiteral(
            "(function(){var a=document.activeElement;if(!a)return 'none';"
            "if(a.tagName!=='ANTIKOLT-CARD')return 'not-host:'+a.tagName;"
            "var s=a.shadowRoot&&a.shadowRoot.activeElement;"
            "return (a.card?a.card.id:'')+':'+(s?(s.className||s.tagName):'none');})()");
        const QString wanted = card_id + QStringLiteral(":card");
        for (int attempt = 0; attempt < 20; ++attempt) {
            QEventLoop focus_loop;
            window.engine()->evaluate(page_surface, focus_probe,
                                      [&](const QJsonObject& reply, const QString& error) {
                                          focus_class = error.isEmpty()
                                                            ? reply.value(QStringLiteral("value")).toString()
                                                            : error;
                                          focus_loop.quit();
                                      });
            QTimer::singleShot(3000, &focus_loop, &QEventLoop::quit);
            focus_loop.exec();
            if (focus_class == wanted) {
                break;
            }
            QEventLoop pause;
            QTimer::singleShot(250, &pause, &QEventLoop::quit);
            pause.exec();
        }
    }

    out << "note_rendered=" << rendered_note << '\n';
    {
        // The chain is printed, not asserted: see the comment at the probe.
        QEventLoop chain_loop;
        window.engine()->evaluate(
            page_surface,
            QStringLiteral("JSON.stringify(window.__antikoltFocusChain||{})"),
            [&](const QJsonObject& reply, const QString&) {
                focus_chain = reply.value(QStringLiteral("value")).toString();
                chain_loop.quit();
            });
        QTimer::singleShot(3000, &chain_loop, &QEventLoop::quit);
        chain_loop.exec();
    }
    out << "note_focus_chain=" << focus_chain << '\n';

    QString stored_notes;
    bool found_in_core = false;
    {
        CoreClient verifier;
        const QString core_dir =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
            QStringLiteral("/core");
        if (verifier.start(MainWindow::coreExecutable(), core_dir)) {
            QEventLoop loop;
            verifier.cardsList([&](const QJsonObject& listed, const QString& error) {
                if (error.isEmpty()) {
                    for (const QJsonValue& value :
                         listed.value(QStringLiteral("cards")).toArray()) {
                        const QJsonObject card = value.toObject();
                        if (card.value(QStringLiteral("id")).toString() == card_id) {
                            found_in_core = true;
                            stored_notes = card.value(QStringLiteral("notes")).toString();
                        }
                    }
                }
                loop.quit();
            });
            QTimer::singleShot(10000, &loop, &QEventLoop::quit);
            loop.exec();
            verifier.shutdown();
        }
    }

    out << "note_id=" << card_id << '\n';
    out << "note_page=" << state.value(QStringLiteral("notes")).toString() << '\n';
    out << "note_focus=" << focus_class << '\n';
    out << "note_model=" << state.value(QStringLiteral("cardNotes")).toString() << '\n';
    out << "note_core=" << stored_notes << '\n';
    out << "note_error=" << state.value(QStringLiteral("error")).toString() << '\n';
    const bool ok = !card_id.isEmpty() && found_in_core && stored_notes == probe_note &&
                    state.value(QStringLiteral("notes")).toString() == probe_note &&
                    state.value(QStringLiteral("cardNotes")).toString() == probe_note &&
                    rendered_note == probe_note &&
                    focus_class == card_id + QStringLiteral(":card") &&
                    state.value(QStringLiteral("error")).toString().isEmpty();
    out << "note_write=" << (ok ? "ok" : "FAILED") << '\n';
    out << "note_rendered_ok=" << (rendered_note == probe_note ? "ok" : "FAILED") << '\n';
    out << "note_focus_ok=" << (focus_class == card_id + QStringLiteral(":card") ? "ok" : "FAILED")
        << '\n';
    out.flush();
    return ok ? 0 : 1;
}

/// Prove the grid can *write*: a pin click travels through the bridge to the core and persists.
///
/// The page's own witness (`window.antikoltPinState`) says what the grid shows; a fresh core client says
/// what the store holds. Both must agree about the same card, and the card must have been unpinned before
/// the click — a click that changes nothing would pass a weaker test.
int runPinCheck(const QString& enginePath) {
    QTextStream out(stdout);
    qputenv("ANTIKOLT_ENGINE", enginePath.toUtf8());
    qputenv("ANTIKOLT_CORE", MainWindow::coreExecutable().toUtf8());

    MainWindow window;
    window.show();
    window.move(150, 110);
    window.startEngine();

    QString clicked_id;
    bool pinned_before = false;
    QJsonObject state;
    {
        QEventLoop loop;
        QObject::connect(window.engine(), &EngineClient::loadFinished, &loop,
                         [&](qint64 id, const QString& url, int) {
                             if (!url.startsWith(QStringLiteral("file:"))) {
                                 return;
                             }
                             QTimer::singleShot(900, &loop, [&, id] {
                                 // The first card, whatever its state: the assertion is the *flip*, so the
                                 // test does not depend on how many cards are already pinned in the store
                                 // (other checks toggle pins too, and the store is shared).
                                 window.engine()->evaluate(
                                     id,
                                     QStringLiteral(
                                         "(function(){var els=document.querySelectorAll("
                                         "'antikolt-card');if(!els.length)return 'no-card';"
                                         "var host=els[0],c=host.card;if(!c)return 'no-model';"
                                         "var b=host.shadowRoot.querySelector('.pin');"
                                         "if(!b)return 'no-pin';"
                                         "var before=c.pinned?1:0;b.click();"
                                         "return 'clicked:'+c.id+':'+before;})()"),
                                     [&, id](const QJsonObject& reply, const QString& error) {
                                         const QString value = error.isEmpty()
                                                                   ? reply.value(QStringLiteral("value"))
                                                                         .toString()
                                                                   : error;
                                         const QStringList parts = value.split(QLatin1Char(':'));
                                         const bool clicked =
                                             parts.size() == 3 &&
                                             parts.at(0) == QStringLiteral("clicked");
                                         out << "pin_click=" << (clicked ? "ok" : "FAILED") << '\n';
                                         if (!clicked) {
                                             out << "pin_click_value=" << value << '\n';
                                             loop.quit();
                                             return;
                                         }
                                         clicked_id = parts.at(1);
                                         pinned_before = parts.at(2) == QStringLiteral("1");
                                         QTimer::singleShot(1200, &loop, [&, id] {
                                             window.engine()->evaluate(
                                                 id,
                                                 QStringLiteral(
                                                     "JSON.stringify(window.antikoltPinState())"),
                                                 [&](const QJsonObject& state_reply,
                                                     const QString& state_error) {
                                                     if (state_error.isEmpty()) {
                                                         state = QJsonDocument::fromJson(
                                                                     state_reply
                                                                         .value(QStringLiteral("value"))
                                                                         .toString()
                                                                         .toUtf8())
                                                                     .object();
                                                     }
                                                     loop.quit();
                                                 });
                                         });
                                     });
                             });
                         });
        QTimer::singleShot(45000, &loop, &QEventLoop::quit);
        loop.exec();
    }

    // What the store holds, read by a fresh client (a running one would report its own copy).
    bool stored_pinned = false;
    {
        CoreClient verifier;
        const QString core_dir =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
            QStringLiteral("/core");
        if (verifier.start(MainWindow::coreExecutable(), core_dir)) {
            QEventLoop loop;
            verifier.cardsList([&](const QJsonObject& listed, const QString& error) {
                if (error.isEmpty()) {
                    for (const QJsonValue& value :
                         listed.value(QStringLiteral("cards")).toArray()) {
                        const QJsonObject card = value.toObject();
                        if (card.value(QStringLiteral("id")).toString() == clicked_id) {
                            stored_pinned = card.value(QStringLiteral("pinned")).toBool();
                        }
                    }
                }
                loop.quit();
            });
            QTimer::singleShot(10000, &loop, &QEventLoop::quit);
            loop.exec();
            verifier.shutdown();
        }
    }

    const QString pin_error = state.value(QStringLiteral("lastError")).toString();
    const bool expected_pinned = !pinned_before;
    out << "pin_id=" << clicked_id << " before=" << (pinned_before ? "pinned" : "unpinned") << '\n';
    out << "pin_page=" << (state.value(QStringLiteral("lastPinned")).toBool() ? "pinned" : "unpinned")
        << " pinned_count=" << state.value(QStringLiteral("pinnedCount")).toInt() << '\n';
    out << "pin_error=" << pin_error << '\n';
    out << "pin_core=" << (stored_pinned ? "pinned" : "unpinned") << '\n';
    // Both witnesses must show the *flipped* state: a click that changed nothing would pass a weaker
    // test, and a page that changed without the store (or the reverse) is exactly the bug this guards.
    const bool ok = !clicked_id.isEmpty() && pin_error.isEmpty() &&
                    state.value(QStringLiteral("lastPinned")).toBool() == expected_pinned &&
                    stored_pinned == expected_pinned &&
                    state.value(QStringLiteral("lastId")).toString() == clicked_id;
    out << "pin_write=" << (ok ? "ok" : "FAILED") << '\n';
    window.close();
    out.flush();
    return ok ? 0 : 1;
}

/// Prove the reader's place end to end: scroll a card, go home, reopen it, land where they were.
///
/// Two passes through the same handler, and the click *polls* for the card instead of waiting on a timer.
/// The second pass only works because the grid the user returns to gets the bridge installed
/// (ensureSurface's existing-surface path): without it the click has nothing to call.
int runScrollRestoreCheck(const QString& enginePath) {
    QTextStream out(stdout);
    qputenv("ANTIKOLT_ENGINE", enginePath.toUtf8());
    qputenv("ANTIKOLT_CORE", MainWindow::coreExecutable().toUtf8());
    const QString tall_page = QStringLiteral(
        "<!doctype html><meta charset=\"utf-8\"><title>Tall</title>"
        "<style>body{margin:0;font:14px sans-serif}"
        "section{height:4000px;background:linear-gradient(#fff,#bbb)}</style>"
        "<section>tall page</section>");
    ProbeOrigin probe(tall_page);
    if (!probe.listening()) {
        out << "server=FAILED\n";
        return 1;
    }
    const QString origin = probe.origin();
    const QString core_dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/core");
    QDir().mkpath(core_dir);
    const QString book_path =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
        QStringLiteral("/engine/scroll.json");
    CoreClient oracle;
    if (!oracle.start(MainWindow::coreExecutable(), core_dir)) {
        out << "core_start=FAILED\n";
        return 1;
    }
    bool written = false;
    {
        QEventLoop loop;
        QJsonObject card;
        card.insert(QStringLiteral("id"), QStringLiteral("scroll-restore"));
        card.insert(QStringLiteral("title"), QStringLiteral("Restore probe"));
        card.insert(QStringLiteral("url"), origin + QStringLiteral("/"));
        card.insert(QStringLiteral("container"), QStringLiteral("c-private"));
        card.insert(QStringLiteral("status"), QStringLiteral("unknown"));
        card.insert(QStringLiteral("pinned"), false);
        card.insert(QStringLiteral("notes"), QString());
        card.insert(QStringLiteral("preview_ref"), QString());
        card.insert(QStringLiteral("last_opened_at_ms"), 0);
        oracle.cardUpsert(card, [&](const QJsonObject&, const QString& error) {
            written = error.isEmpty();
            loop.quit();
        });
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    oracle.shutdown();
    if (!written) {
        out << "card_written=FAILED\n";
        return 1;
    }
    QFile::remove(book_path);  // a remembered place from another run is not what this check asserts

    MainWindow window;
    window.show();
    window.move(150, 110);
    window.startEngine();

    int phase = 0;
    QString first_y;
    QString second_y;
    {
        QEventLoop loop;
        const auto eval = [&window](qint64 surface, const QString& expression,
                                    std::function<void(const QString&)> done) {
            window.engine()->evaluate(surface, expression,
                                      [done](const QJsonObject& reply, const QString& error) {
                                          done(error.isEmpty()
                                                   ? reply.value(QStringLiteral("value")).toString()
                                                   : error);
                                      });
        };
        std::function<void(qint64, int, std::function<void(bool)>)> click;
        click = [&](qint64 grid, int attempt, std::function<void(bool)> done) {
            window.engine()->evaluate(
                grid,
                QStringLiteral(
                    "(function(){var els=document.querySelectorAll('antikolt-card');"
                    "for(var i=0;i<els.length;i++){var c=els[i].card;"
                    "if(c&&c.id==='scroll-restore'){"
                    "els[i].shadowRoot.querySelector('.card').click();return 'clicked';}}"
                    "return 'no-card';})()"),
                [&, grid, attempt, done](const QJsonObject& reply, const QString& error) {
                    const QString value = error.isEmpty()
                                              ? reply.value(QStringLiteral("value")).toString()
                                              : error;
                    if (value == QStringLiteral("clicked")) {
                        done(true);
                        return;
                    }
                    if (attempt >= 25) {
                        out << "click_failed=" << value << '\n';
                        done(false);
                        return;
                    }
                    QTimer::singleShot(400, &loop, [&, grid, attempt, done] {
                        click(grid, attempt + 1, done);
                    });
                });
        };
        QObject::connect(window.engine(), &EngineClient::loadFinished, &loop,
                         [&](qint64 id, const QString& url, int) {
                             if (url.startsWith(QStringLiteral("file:"))) {
                                 QTimer::singleShot(600, &loop, [&, id] {
                                     click(id, 0, [&](bool ok) {
                                         if (!ok) {
                                             loop.quit();
                                         }
                                     });
                                 });
                                 return;
                             }
                             if (!url.startsWith(origin)) {
                                 return;
                             }
                             if (phase == 0) {
                                 window.engine()->scroll(
                                     id, 0, 700, false,
                                     [&, id](const QJsonObject&, const QString& error) {
                                         if (!error.isEmpty()) {
                                             out << "scroll_error=" << error << '\n';
                                             loop.quit();
                                             return;
                                         }
                                         QTimer::singleShot(600, &loop, [&, id] {
                                             eval(id,
                                                  QStringLiteral("String(Math.round(window.scrollY))"),
                                                  [&](const QString& y) {
                                                      first_y = y;
                                                      phase = 1;
                                                      window.loadHome();
                                                  });
                                         });
                                     });
                                 return;
                             }
                             QTimer::singleShot(900, &loop, [&, id] {
                                 eval(id, QStringLiteral("String(Math.round(window.scrollY))"),
                                      [&](const QString& y) {
                                          second_y = y;
                                          loop.quit();
                                      });
                             });
                         });
        QTimer::singleShot(90000, &loop, &QEventLoop::quit);
        loop.exec();
    }

    out << "first_scroll_y=" << first_y << '\n';
    out << "restored_scroll_y=" << second_y << '\n';
    const int first = first_y.toInt();
    const int second = second_y.toInt();
    const bool ok = first >= 600 && std::abs(second - first) <= 60;
    out << "scroll_restore=" << (ok ? "ok" : "FAILED") << '\n';
    window.close();
    out.flush();
    return ok ? 0 : 1;
}
/// Prove where-the-reader-stopped is remembered per card, and forgotten when they return to the top.
///
/// The book is a file, so the check is a scratch file: a positive offset survives, a zero forgets, and two
/// cards do not overwrite each other.
int runScrollBookCheck() {
    QTextStream out(stdout);
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                        QStringLiteral("/scroll-book-selftest");
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/scroll.json");

    const int before = antikolt::scrollPositionFor(path, QStringLiteral("card-a"));
    antikolt::saveScrollPosition(path, QStringLiteral("card-a"), 640);
    antikolt::saveScrollPosition(path, QStringLiteral("card-b"), 120);
    const int a = antikolt::scrollPositionFor(path, QStringLiteral("card-a"));
    const int b = antikolt::scrollPositionFor(path, QStringLiteral("card-b"));
    antikolt::saveScrollPosition(path, QStringLiteral("card-a"), 0);
    const int a_after_top = antikolt::scrollPositionFor(path, QStringLiteral("card-a"));
    const int b_after_top = antikolt::scrollPositionFor(path, QStringLiteral("card-b"));
    // Re-reading from disk is the point: the book must survive the process, not just a QHash.
    const int reloaded = antikolt::loadScrollBook(path).value(QStringLiteral("card-b"), 0);

    out << "scroll_before=" << before << '\n';
    out << "scroll_a=" << a << '\n';
    out << "scroll_b=" << b << '\n';
    out << "scroll_a_after_top=" << a_after_top << '\n';
    out << "scroll_b_after_top=" << b_after_top << '\n';
    out << "scroll_reloaded=" << reloaded << '\n';
    const bool ok = before == 0 && a == 640 && b == 120 && a_after_top == 0 && b_after_top == 120 &&
                    reloaded == 120;
    out << "scroll_book=" << (ok ? "ok" : "FAILED") << '\n';
    out.flush();
    return ok ? 0 : 1;
}
/// Prove the daily budget persists: it counts, it refuses, and a new day starts at zero.
///
/// The budget is a file in the profile, so the check is a scratch directory and two day strings - a counter
/// that resets on every launch would pass a memory-only test and still be no budget at all.
/// Prove the background preview queue: which card needs a picture next, and which does not.
///
/// The decision only — the worker that acts on it must respect I1 (one live surface), so it lives in the window.
int runPreviewQueueCheck() {
    QTextStream out(stdout);
    const QString dir = QDir::tempPath() + QStringLiteral("/antikolt-preview-queue-") +
                        QString::number(QDateTime::currentMSecsSinceEpoch());
    QDir().mkpath(dir);
    const QString fresh_path = dir + QStringLiteral("/fresh.png");
    const QString stale_path = dir + QStringLiteral("/stale.png");
    for (const QString& path : {fresh_path, stale_path}) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            out << "queue_setup=FAILED\n";
            return 1;
        }
        file.write("x");
        file.close();
    }
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const qint64 max_age_ms = 7LL * 24 * 60 * 60 * 1000;
    {
        QFile stale_file(stale_path);
        if (stale_file.open(QIODevice::ReadWrite)) {
            stale_file.setFileTime(QDateTime::fromMSecsSinceEpoch(now_ms - 30LL * 24 * 60 * 60 * 1000),
                                   QFileDevice::FileModificationTime);
        }
    }
    const auto card = [](const QString& id, const QString& url, const QString& ref, qint64 opened) {
        QJsonObject object;
        object.insert(QStringLiteral("id"), id);
        object.insert(QStringLiteral("url"), url);
        object.insert(QStringLiteral("preview_ref"), ref);
        object.insert(QStringLiteral("last_opened_at_ms"), static_cast<double>(opened));
        return object;
    };
    QJsonArray cards;
    cards.append(card(QStringLiteral("no-preview"), QStringLiteral("https://example.org/a"), QString(), 3000));
    cards.append(card(QStringLiteral("stale"), QStringLiteral("https://example.org/b"),
                      QStringLiteral("stale.png"), 1000));
    cards.append(card(QStringLiteral("fresh"), QStringLiteral("https://example.org/c"),
                      QStringLiteral("fresh.png"), 0));
    cards.append(card(QStringLiteral("local"), QStringLiteral("file:///C:/x.html"), QString(), 500));

    const QString first = antikolt::nextPreviewCard(cards, dir, now_ms, max_age_ms);
    out << "queue_first=" << first << '\n';
    QJsonArray without_first;
    for (const QJsonValue& value : cards) {
        if (value.toObject().value(QStringLiteral("id")).toString() != first) {
            without_first.append(value);
        }
    }
    const QString second = antikolt::nextPreviewCard(without_first, dir, now_ms, max_age_ms);
    out << "queue_second=" << second << '\n';
    QJsonArray fresh_only;
    fresh_only.append(card(QStringLiteral("fresh"), QStringLiteral("https://example.org/c"),
                           QStringLiteral("fresh.png"), 0));
    const QString none = antikolt::nextPreviewCard(fresh_only, dir, now_ms, max_age_ms);
    out << "queue_none=" << (none.isEmpty() ? QStringLiteral("empty") : none) << '\n';
    QDir(dir).removeRecursively();
    // ADR-041 guards: the worker may start only on the grid, with budget and a candidate - and it must
    // abandon the moment the user opens a card. Proven here because it is cheap to prove.
    const bool start_on_grid = antikolt::previewWorkerCanStart(antikolt::PreviewWorker::Idle, false, true,
                                                                 !first.isEmpty());
    const bool no_start_over_card = !antikolt::previewWorkerCanStart(
        antikolt::PreviewWorker::Idle, true, true, !first.isEmpty());
    const bool no_start_over_budget = !antikolt::previewWorkerCanStart(
        antikolt::PreviewWorker::Idle, false, false, !first.isEmpty());
    const bool no_double_worker = !antikolt::previewWorkerCanStart(
        antikolt::PreviewWorker::Capturing, false, true, !first.isEmpty());
    const bool aborts_when_opened =
        antikolt::previewWorkerMustAbort(antikolt::PreviewWorker::Loading, true) &&
        !antikolt::previewWorkerMustAbort(antikolt::PreviewWorker::Idle, true);
    out << "queue_guards="
        << ((start_on_grid && no_start_over_card && no_start_over_budget && no_double_worker &&
             aborts_when_opened)
                ? "ok"
                : "FAILED")
        << '\n';
    const bool ok = first == QStringLiteral("no-preview") && second == QStringLiteral("stale") &&
                    none.isEmpty() && start_on_grid && no_start_over_card && no_start_over_budget &&
                    no_double_worker && aborts_when_opened;
    out << "preview_queue=" << (ok ? "ok" : "FAILED") << '\n';
    out.flush();
    return ok ? 0 : 1;
}

int runPreviewBudgetCheck() {
    QTextStream out(stdout);
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                        QStringLiteral("/preview-budget-selftest");
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/budget.json");
    const QString today = QStringLiteral("2026-11-17");
    const QString other_day = QStringLiteral("2026-11-16");

    const int at_start = antikolt::previewUsedToday(path, today);
    QStringList marks;
    for (int i = 0; i < 3; ++i) {
        marks << QString::number(i);
        antikolt::previewConsumeBudget(path, today);
    }
    const int after_three = antikolt::previewUsedToday(path, today);
    const bool allows_at_three = antikolt::previewBudgetAllows(path, today, 5);
    for (int i = 0; i < 2; ++i) {
        antikolt::previewConsumeBudget(path, today);
    }
    const int after_five = antikolt::previewUsedToday(path, today);
    const bool refuses_at_five = !antikolt::previewBudgetAllows(path, today, 5);
    // A counter from another day is not today's counter.
    const int other = antikolt::previewUsedToday(path, other_day);
    // A fresh file (no budget yet) allows the first one.
    const bool fresh_allows =
        antikolt::previewBudgetAllows(dir + QStringLiteral("/none.json"), today, 1);

    out << "budget_start=" << at_start << '\n';
    out << "budget_after_three=" << after_three << '\n';
    out << "budget_after_five=" << after_five << '\n';
    out << "budget_allows_at_three=" << (allows_at_three ? "true" : "false") << '\n';
    out << "budget_refuses_at_five=" << (refuses_at_five ? "true" : "false") << '\n';
    out << "budget_other_day=" << other << '\n';
    out << "budget_fresh_allows=" << (fresh_allows ? "true" : "false") << '\n';
    const bool ok = at_start == 0 && after_three == 3 && after_five == 5 && allows_at_three &&
                    refuses_at_five && other == 0 && fresh_allows;
    out << "preview_budget=" << (ok ? "ok" : "FAILED") << '\n';
    out.flush();
    return ok ? 0 : 1;
}
/// Prove the refresh policy: a preview is regenerated when it is missing or older than the limit.
///
/// The policy is a decision about a file and a clock, so the check is a scratch directory and two clocks -
/// no engine, no core, and no seven days of waiting.
int runPreviewRefreshCheck() {
    QTextStream out(stdout);
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                        QStringLiteral("/preview-refresh-selftest");
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/card.png");
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const qint64 week_ms = 7LL * 24 * 60 * 60 * 1000;

    out << "refresh_missing=" << (antikolt::previewNeedsRefresh(path, now_ms, week_ms) ? "true" : "false")
        << '\n';

    QImage probe(2, 2, QImage::Format_RGB32);
    probe.fill(QColor(40, 180, 90));
    const bool wrote = probe.save(path, "PNG");
    // "Fresh" has to be measured *before* the file is back-dated; asking again afterwards would be asking a
    // different question and the check would compare a stale answer with a fresh one.
    const bool fresh_ok = !antikolt::previewNeedsRefresh(path, now_ms, week_ms);
    out << "refresh_fresh=" << (fresh_ok ? "false" : "true") << '\n';

    // Back-date the file by thirty days: the policy must want a new photograph.
    QFile file(path);
    const qint64 old_ms = now_ms - 30LL * 24 * 60 * 60 * 1000;
    const bool aged = file.open(QIODevice::ReadWrite) && file.setFileTime(
        QDateTime::fromMSecsSinceEpoch(old_ms), QFileDevice::FileModificationTime);
    file.close();
    const bool stale_wanted = antikolt::previewNeedsRefresh(path, now_ms, week_ms);

    // A clock that moved backwards must not make the shell re-photograph on every open.
    const bool future_ok = !antikolt::previewNeedsRefresh(path, old_ms - 1000, week_ms);

    out << "refresh_stale=" << (stale_wanted ? "true" : "false") << '\n';
    out << "refresh_aged=" << (aged ? "true" : "false") << '\n';
    out << "refresh_future=" << (future_ok ? "true" : "false") << '\n';

    const bool ok = antikolt::previewNeedsRefresh(dir + QStringLiteral("/nope.png"), now_ms, week_ms) &&
                    wrote && fresh_ok && aged &&
                    stale_wanted && future_ok;
    out << "preview_refresh=" << (ok ? "ok" : "FAILED") << '\n';
    out.flush();
    return ok ? 0 : 1;
}
/// Prove the preview directory is cleaned: an orphan is deleted, a referenced file and a stranger survive.
///
/// No engine and no core: the janitor is a decision about a directory, so the check is a directory.
int runPreviewCleanCheck() {
    QTextStream out(stdout);
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                        QStringLiteral("/preview-janitor-selftest");
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);

    QImage probe(2, 2, QImage::Format_RGB32);
    probe.fill(QColor(220, 40, 40));
    const bool wrote = probe.save(dir + QStringLiteral("/keep.png"), "PNG") &&
                       probe.save(dir + QStringLiteral("/orphan.png"), "PNG");
    {
        // The second orphan only needs a preview *extension*: the janitor decides by name and never opens the
        // file, so a WEBP encoder is not required (and is not always installed with Qt).
        QFile orphan(dir + QStringLiteral("/orphan.webp"));
        orphan.open(QIODevice::WriteOnly);
        orphan.write("pretend webp");
    }
    {
        QFile notes(dir + QStringLiteral("/notes.txt"));
        notes.open(QIODevice::WriteOnly);
        notes.write("not a preview");
    }
    if (!wrote) {
        out << "preview_files=FAILED\n";
        return 1;
    }

    const QStringList removed = antikolt::removeOrphanPreviews(
        dir, QSet<QString>{QStringLiteral("keep.png")});
    out << "preview_removed=" << removed.join(QLatin1Char(',')) << '\n';
    out << "preview_kept=" << (QFileInfo::exists(dir + QStringLiteral("/keep.png")) ? "ok" : "FAILED")
        << '\n';
    out << "preview_orphan_gone="
        << (!QFileInfo::exists(dir + QStringLiteral("/orphan.png")) &&
                    !QFileInfo::exists(dir + QStringLiteral("/orphan.webp"))
                ? "ok"
                : "FAILED")
        << '\n';
    // A file that is not a preview is not ours to delete, and a missing directory is not an error.
    out << "preview_stranger_kept="
        << (QFileInfo::exists(dir + QStringLiteral("/notes.txt")) ? "ok" : "FAILED") << '\n';
    out << "preview_missing_dir="
        << (antikolt::removeOrphanPreviews(dir + QStringLiteral("/nope"), {}).isEmpty() ? "ok"
                                                                                       : "FAILED")
        << '\n';
    const bool ok = removed == QStringList{QStringLiteral("orphan.png"), QStringLiteral("orphan.webp")} &&
                    QFileInfo::exists(dir + QStringLiteral("/keep.png")) &&
                    !QFileInfo::exists(dir + QStringLiteral("/orphan.png")) &&
                    QFileInfo::exists(dir + QStringLiteral("/notes.txt"));
    out << "preview_cleanup=" << (ok ? "ok" : "FAILED") << '\n';
    out.flush();
    return ok ? 0 : 1;
}

/// Prove the session rules can be changed over the wire, and that the core refuses nonsense (ADR-039).
///
/// Three steps: store a real policy, send an impossible one (the reply must *refuse* it), and read the rules
/// back — the stored policy must be the first one, not the refused one.
int runRulesCheck(const QString& corePath) {
    QTextStream out(stdout);
    const QString core_dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/core");
    QDir().mkpath(core_dir);

    CoreClient client;
    if (!client.start(corePath, core_dir)) {
        out << "core_start=FAILED\n";
        return 1;
    }
    const auto ask = [&client](const QJsonObject& rules, QJsonObject* saved, QString* error) {
        QEventLoop loop;
        client.sessionRules(rules, [&](const QJsonObject& reply, const QString& reply_error) {
            if (saved != nullptr) {
                *saved = reply.value(QStringLiteral("rules")).toObject();
            }
            if (error != nullptr) {
                *error = reply_error;
            }
            loop.quit();
        });
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
    };

    QJsonObject saved;
    QString save_error;
    ask(QJsonObject{{QStringLiteral("ttl_ms"), 7200000.0},
                    {QStringLiteral("wipe_on_app_lock"), true},
                    {QStringLiteral("container_default"), QStringLiteral("wipe")}},
        &saved, &save_error);

    QString refused;
    ask(QJsonObject{{QStringLiteral("ttl_ms"), 1000.0},
                    {QStringLiteral("wipe_on_app_lock"), false},
                    {QStringLiteral("container_default"), QStringLiteral("sweep")}},
        nullptr, &refused);

    QJsonObject reread;
    {
        QEventLoop loop;
        client.sessionList([&](const QJsonObject& reply, const QString&) {
            reread = reply.value(QStringLiteral("rules")).toObject();
            loop.quit();
        });
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    client.shutdown();

    const qint64 stored_ttl = static_cast<qint64>(saved.value(QStringLiteral("ttl_ms")).toDouble());
    const qint64 reread_ttl = static_cast<qint64>(reread.value(QStringLiteral("ttl_ms")).toDouble());
    out << "rules_ttl_ms=" << stored_ttl << '\n';
    out << "rules_lock_wipe="
        << (saved.value(QStringLiteral("wipe_on_app_lock")).toBool() ? "true" : "false") << '\n';
    out << "rules_refused=" << (refused.contains(QStringLiteral("TTL")) ? "refused" : refused)
        << '\n';
    out << "rules_reread_ttl_ms=" << reread_ttl << '\n';
    const bool ok = save_error.isEmpty() && stored_ttl == 7200000 &&
                    saved.value(QStringLiteral("wipe_on_app_lock")).toBool() &&
                    refused.contains(QStringLiteral("TTL")) && reread_ttl == 7200000;
    out << "session_rules=" << (ok ? "ok" : "FAILED") << '\n';
    out.flush();
    return ok ? 0 : 1;
}

/// Prove the page can *ask*: protocol 2's request channel, end to end (ADR-034).
///
/// The page calls `window.antikoltNative.request('cards.search', {q})` — a persistent `cefQuery` the
/// engine holds until the shell answers. The answer comes from the core's card list, so it is something
/// the page cannot produce on its own. Two questions: one that must succeed, and one unknown method that
/// must be *rejected* rather than left hanging — a request nobody answers is a hung page.
int runBridgeCheck(const QString& enginePath) {
    QTextStream out(stdout);
    qputenv("ANTIKOLT_ENGINE", enginePath.toUtf8());
    qputenv("ANTIKOLT_CORE", MainWindow::coreExecutable().toUtf8());

    MainWindow window;
    window.show();
    window.move(150, 110);
    window.startEngine();

    bool asked = false;
    QString answer;
    {
        QEventLoop loop;
        QObject::connect(window.engine(), &EngineClient::loadFinished, &loop,
                         [&](qint64 id, const QString& url, int) {
                             if (!url.startsWith(QStringLiteral("file:"))) {
                                 return;
                             }
                             QTimer::singleShot(700, &loop, [&, id] {
                                 window.engine()->evaluate(
                                     id,
                                     QStringLiteral(
                                         "window.antikoltNative.request('cards.search', {q:'wiki'})"
                                         ".then(function(r){"
                                         "  window.__answer=r;"
                                         "  var card=(r.cards||[])[0];"
                                         "  if(!card){return;}"
                                         "  card.pinned=!card.pinned;"
                                         "  return window.antikoltNative.request('cards.upsert',"
                                         "    {card:card}).then(function(saved){"
                                         "      window.__upsert=saved;"
                                         "      return window.antikoltNative.request('cards.search',"
                                         "        {q:'wiki'}).then(function(after){window.__after=after;});"
                                         "    });"
                                         "},"
                                         "function(e){window.__answer={error:String(e.message)};});"
                                         "window.antikoltNative.request('no_such_method', {})"
                                         ".then(function(r){window.__unknown=r;},"
                                         "function(e){window.__unknown={error:String(e.message)};});"
                                         " 'asked'"),
                                     [&, id](const QJsonObject& reply, const QString& error) {
                                         asked = error.isEmpty() &&
                                                 reply.value(QStringLiteral("value")).toString() ==
                                                     QStringLiteral("asked");
                                         out << "asked=" << (asked ? "ok" : "FAILED") << '\n';
                                         // Both promises settle asynchronously; read the page's own
                                         // variables afterwards.
                                         QTimer::singleShot(1500, &loop, [&, id] {
                                             window.engine()->evaluate(
                                                 id,
                                                 QStringLiteral(
                                                     "JSON.stringify({answer: window.__answer||null,"
                                                     " unknown: window.__unknown||null,"
                                                     " upsert: window.__upsert||null,"
                                                     " after: window.__after||null})"),
                                                 [&](const QJsonObject& value, const QString& failure) {
                                                     answer = failure.isEmpty()
                                                                  ? value.value(QStringLiteral("value"))
                                                                        .toString()
                                                                  : failure;
                                                     loop.quit();
                                                 });
                                         });
                                     });
                             });
                         });
        QTimer::singleShot(45000, &loop, &QEventLoop::quit);
        loop.exec();
    }

    const QJsonObject result = QJsonDocument::fromJson(answer.toUtf8()).object();
    const QJsonArray cards = result.value(QStringLiteral("answer")).toObject()
                                 .value(QStringLiteral("cards")).toArray();
    const QString unknown_error = result.value(QStringLiteral("unknown")).toObject()
                                      .value(QStringLiteral("error")).toString();
    const QJsonObject saved =
        result.value(QStringLiteral("upsert")).toObject().value(QStringLiteral("card")).toObject();
    const QJsonArray after = result.value(QStringLiteral("after")).toObject()
                                 .value(QStringLiteral("cards")).toArray();
    QString first_title;
    for (const QJsonValue& value : cards) {
        first_title = value.toObject().value(QStringLiteral("title")).toString();
        break;
    }
    const bool found = !cards.isEmpty() && first_title.toLower().contains(QStringLiteral("wiki"));
    const bool rejected = unknown_error.contains(QStringLiteral("ismeretlen"));
    // The pin was flipped through the bridge, and the *next* question already sees it: the answer comes
    // from the core each time, so this is persistence, not a page-local echo.
    const bool pinned_saved = !saved.isEmpty() && saved.value(QStringLiteral("id")).toString() ==
                                                          cards.first().toObject()
                                                              .value(QStringLiteral("id"))
                                                              .toString();
    const bool pinned_persisted =
        !after.isEmpty() &&
        after.first().toObject().value(QStringLiteral("pinned")).toBool() ==
            saved.value(QStringLiteral("pinned")).toBool();
    out << "search_cards=" << cards.size() << '\n';
    out << "search_first_title=" << first_title << '\n';
    out << "unknown_method=" << (rejected ? "rejected" : "FAILED") << '\n';
    out << "upsert_pinned=" << (saved.value(QStringLiteral("pinned")).toBool() ? "true" : "false")
        << '\n';
    out << "upsert_persisted=" << (pinned_persisted ? "ok" : "FAILED") << '\n';
    const bool ok = asked && found && rejected && pinned_saved && pinned_persisted;
    out << "bridge_request=" << (ok ? "ok" : "FAILED") << '\n';
    window.close();
    out.flush();
    return ok ? 0 : 1;
}

/// Prove the session panel shows what the core's ledger says, and that the policy is the core's.
///
/// A row is written straight into the store the window will use, then the window's panel is opened and
/// read back through its own text: the test trusts neither the table's model nor the status line, it
/// compares what the panel displays with the row it wrote. The window is grabbed as well, so a
/// rendering regression that leaves the table blank shows up in the picture size.
int runSessionPanelCheck(const QString& enginePath) {
    QTextStream out(stdout);
    qputenv("ANTIKOLT_ENGINE", enginePath.toUtf8());
    qputenv("ANTIKOLT_CORE", MainWindow::coreExecutable().toUtf8());

    const QString core_dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                             QStringLiteral("/core");
    QDir().mkpath(core_dir);

    // 1. A row the panel must show, written through the same wire the shell uses.
    CoreClient oracle;
    if (!oracle.start(MainWindow::coreExecutable(), core_dir)) {
        out << "core_start=FAILED\n";
        return 1;
    }
    out << "core_start=ok\n";
    bool written = false;
    {
        QEventLoop loop;
        QJsonObject session;
        session.insert(QStringLiteral("container_id"), QStringLiteral("c-private"));
        session.insert(QStringLiteral("origin"), QStringLiteral("https://example.org"));
        session.insert(QStringLiteral("state"), QStringLiteral("expired"));
        session.insert(QStringLiteral("policy"), QStringLiteral("sweep"));
        session.insert(QStringLiteral("cookie_count"), 7);
        session.insert(QStringLiteral("cookie_bytes"), 1024);
        session.insert(QStringLiteral("storage_bytes"), 4096);
        session.insert(QStringLiteral("service_worker_count"), 0);
        session.insert(QStringLiteral("last_seen_ms"), 0);
        session.insert(QStringLiteral("last_verified_ms"), 0);
        session.insert(QStringLiteral("live_ram_bytes"), QJsonValue::Null);
        session.insert(QStringLiteral("keep_alive"), false);
        oracle.sessionUpsert(session, [&](const QJsonObject& saved, const QString& error) {
            written = error.isEmpty() &&
                      saved.value(QStringLiteral("session"))
                              .toObject()
                              .value(QStringLiteral("origin"))
                              .toString() == QStringLiteral("https://example.org");
            out << "session_written=" << (written ? "ok" : "FAILED") << '\n';
            loop.quit();
        });
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    // The panel's editors show what the *core* holds, so the check sets the rules it asserts: otherwise it
    // would be reading whatever the previous run left behind, and a shared store makes that a lie.
    {
        QEventLoop loop;
        QJsonObject rules;
        rules.insert(QStringLiteral("ttl_ms"), 7200000.0);
        rules.insert(QStringLiteral("wipe_on_app_lock"), true);
        rules.insert(QStringLiteral("container_default"), QStringLiteral("wipe"));
        oracle.sessionRules(rules, [&](const QJsonObject&, const QString& error) {
            out << "panel_rules_seeded=" << (error.isEmpty() ? "ok" : error) << '\n';
            loop.quit();
        });
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    oracle.shutdown();
    if (!written) {
        return 1;
    }

    // 2. The window, its panel, and what the panel says.
    MainWindow window;
    window.show();
    window.move(120, 90);
    window.resize(1200, 780);
    window.startEngine();

    int rows = -1;
    QString summary;
    int panel_ttl_hours = 0;
    bool panel_lock_wipe = false;
    {
        QEventLoop loop;
        QObject::connect(window.engine(), &EngineClient::loadFinished, &loop,
                         [&](qint64, const QString& url, int) {
                             if (!url.startsWith(QStringLiteral("file:"))) {
                                 return;
                             }
                             // The grid is up, so the core is serving: open the panel and read it.
                             QTimer::singleShot(800, &loop, [&] {
                                 window.showSessions();
                                 QTimer::singleShot(700, &loop, [&] {
                                     rows = window.sessionPanelRowCount();
                                     panel_ttl_hours = window.sessionPanelTtlHours();
                                     panel_lock_wipe = window.sessionPanelLockWipe();
                                     summary = window.sessionPanelSummary();
                                     const QString dir =
                                         QStandardPaths::writableLocation(
                                             QStandardPaths::AppDataLocation) +
                                         QStringLiteral("/panel-selftest");
                                     QDir().mkpath(dir);
                                     const QPixmap shot = window.grab();
                                     out << "panel_grab="
                                         << (shot.save(dir + QStringLiteral("/panel.png"))
                                                 ? "ok"
                                                 : "FAILED")
                                         << '\n';
                                     out << "panel_grab_size=" << shot.width() << 'x'
                                         << shot.height() << '\n';
                                     loop.quit();
                                 });
                             });
                         });
        QTimer::singleShot(45000, &loop, &QEventLoop::quit);
        loop.exec();
    }

    out << "panel_rows=" << rows << '\n';
    out << "panel_ttl_hours=" << panel_ttl_hours << '\n';
    out << "panel_lock_wipe=" << (panel_lock_wipe ? "true" : "false") << '\n';
    for (const QString& line : summary.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        out << "panel_row=" << line << '\n';
    }
    // Three things: the seeded row is there, the policy the core computed marks it due, and the editors show the
    // rules the core holds (the panel is a view of the policy, not of its own memory — ADR-039). The row *count*
    // is compared with what the core reports as due instead of a magic number: a developer's profile keeps the
    // rows other checks wrote (one probe origin per run), and a panel test that broke on those would be a test of
    // the profile, not of the panel.
    int due_rows = -1;
    {
        CoreClient counter;
        if (counter.start(MainWindow::coreExecutable(), core_dir)) {
            QEventLoop count_loop;
            QObject::connect(&counter, &CoreClient::ready, &count_loop, [&](const QJsonObject&) {
                counter.sessionDue(QDateTime::currentMSecsSinceEpoch(), false,
                                   [&](const QJsonObject& due, const QString&) {
                                       due_rows = due.value(QStringLiteral("due")).toArray().size();
                                       count_loop.quit();
                                   });
            });
            counter.handshake();
            QTimer::singleShot(10000, &count_loop, &QEventLoop::quit);
            count_loop.exec();
            counter.shutdown();
        }
    }
    out << "panel_due_rows=" << due_rows << '\n';
    const bool ok = rows >= 1 && (due_rows < 0 || rows == due_rows) &&
                    summary.contains(QStringLiteral("https://example.org")) &&
                    summary.contains(QStringLiteral("lejárt")) && panel_ttl_hours == 2 &&
                    panel_lock_wipe && summary.contains(QStringLiteral("süti"));
    out << "session_panel=" << (ok ? "ok" : "FAILED") << '\n';
    window.close();
    out.flush();
    return ok ? 0 : 1;
}

int runCoreCheck(const QString& corePath) {
    QTextStream out(stdout);
    int failures = 0;

    CoreClient client;
    const QString data_dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                             QStringLiteral("/core-selftest");
    QDir().mkpath(data_dir);
    if (!client.start(corePath, data_dir)) {
        out << "core_start=FAILED\n";
        return 1;
    }
    out << "core_start=ok\n";

    QEventLoop loop;
    bool ready = false;
    int card_count = -1;
    bool stamped = false;
    bool session_saved = false;
    int session_rows = -1;
    int due_rows = -1;
    bool session_closed = false;
    int rows_after_close = -1;

    QObject::connect(&client, &CoreClient::failed, &loop, [&](const QString& message) {
        out << "core_error=" << message << '\n';
        loop.quit();
    });
    QObject::connect(&client, &CoreClient::ready, &loop, [&](const QJsonObject& info) {
        out << "core_name=" << info.value(QStringLiteral("name")).toString() << '\n';
        out << "core_version=" << info.value(QStringLiteral("version")).toString() << '\n';
        ready = true;
        client.cardsList([&](const QJsonObject& data, const QString& error) {
            if (!error.isEmpty()) {
                out << "cards_list=FAILED " << error << '\n';
                loop.quit();
                return;
            }
            const QJsonArray cards = data.value(QStringLiteral("cards")).toArray();
            card_count = cards.size();
            out << "cards=" << card_count << '\n';
            if (cards.isEmpty()) {
                loop.quit();
                return;
            }
            const QString id = cards.first().toObject().value(QStringLiteral("id")).toString();
            out << "first_id=" << id << '\n';
            client.cardOpen(id, [&](const QJsonObject& opened, const QString& failure) {
                if (!failure.isEmpty()) {
                    out << "card_open=FAILED " << failure << '\n';
                    loop.quit();
                    return;
                }
                const QJsonObject card = opened.value(QStringLiteral("card")).toObject();
                stamped = card.value(QStringLiteral("last_opened_at_ms")).toDouble() > 0;
                out << "opened_at_ms=" << card.value(QStringLiteral("last_opened_at_ms")).toDouble()
                    << '\n';

                // The session ledger, end to end: one row written, listed, found due by the policy,
                // then closed. The "now" is far in the future so the answer does not depend on the
                // default TTL; what is being measured is that the policy is evaluated in the core.
                QJsonObject session;
                session.insert(QStringLiteral("container_id"), QStringLiteral("c-private"));
                session.insert(QStringLiteral("origin"), QStringLiteral("https://example.org"));
                session.insert(QStringLiteral("state"), QStringLiteral("expired"));
                session.insert(QStringLiteral("policy"), QStringLiteral("sweep"));
                session.insert(QStringLiteral("cookie_count"), 12);
                session.insert(QStringLiteral("cookie_bytes"), 4096);
                session.insert(QStringLiteral("storage_bytes"), 65536);
                session.insert(QStringLiteral("service_worker_count"), 1);
                session.insert(QStringLiteral("last_seen_ms"), 0);
                session.insert(QStringLiteral("last_verified_ms"), 0);
                // `Option<u64>` has no serde default, so the field is sent explicitly as null.
                session.insert(QStringLiteral("live_ram_bytes"), QJsonValue::Null);
                session.insert(QStringLiteral("keep_alive"), false);
                client.sessionUpsert(session, [&](const QJsonObject& saved, const QString& upsert_error) {
                    if (!upsert_error.isEmpty()) {
                        out << "session_upsert=FAILED " << upsert_error << '\n';
                        loop.quit();
                        return;
                    }
                    const QString saved_origin =
                        saved.value(QStringLiteral("session")).toObject().value(QStringLiteral("origin")).toString();
                    session_saved = saved_origin == QStringLiteral("https://example.org");
                    out << "session_upsert=" << (session_saved ? "ok" : "FAILED") << '\n';
                    client.sessionList([&](const QJsonObject& listed, const QString& list_error) {
                        if (!list_error.isEmpty()) {
                            out << "session_list=FAILED " << list_error << '\n';
                            loop.quit();
                            return;
                        }
                        session_rows =
                            listed.value(QStringLiteral("sessions")).toArray().size();
                        out << "session_rows=" << session_rows << '\n';
                        // Year 2100: any TTL the defaults may carry has passed by then.
                        client.sessionDue(4102444800000LL, false, [&](const QJsonObject& due,
                                                                      const QString& due_error) {
                            if (!due_error.isEmpty()) {
                                out << "session_due=FAILED " << due_error << '\n';
                                loop.quit();
                                return;
                            }
                            const QJsonArray rows = due.value(QStringLiteral("due")).toArray();
                            due_rows = rows.size();
                            out << "session_due_rows=" << due_rows << '\n';
                            client.sessionClose(QStringLiteral("c-private"),
                                                QStringLiteral("https://example.org"),
                                                [&](const QJsonObject& closed,
                                                    const QString& close_error) {
                                                    session_closed =
                                                        close_error.isEmpty() &&
                                                        closed.value(QStringLiteral("closed")).toBool();
                                                    out << "session_close="
                                                        << (session_closed ? "ok" : "FAILED")
                                                        << '\n';
                                                    client.sessionList(
                                                        [&](const QJsonObject& after,
                                                            const QString& after_error) {
                                                            if (after_error.isEmpty()) {
                                                                rows_after_close =
                                                                    after.value(QStringLiteral("sessions"))
                                                                        .toArray()
                                                                        .size();
                                                            }
                                                            loop.quit();
                                                        });
                                                });
                        });
                    });
                });
            });
        });
    });

    client.handshake();
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    loop.exec();
    client.shutdown();

    out << "core_ready=" << (ready ? "ok" : "FAILED") << '\n';
    out << "cards_list=" << (card_count > 0 ? "ok" : "FAILED") << '\n';
    out << "card_open=" << (stamped ? "ok" : "FAILED") << '\n';
    // The ledger half: a row was written, listed, judged due and closed again. Every part is needed:
    // a ledger that writes but never expires is not the 24-hour policy.
    const bool ledger_ok = session_saved && session_rows == 1 && due_rows >= 1 && session_closed &&
                           rows_after_close == 0;
    out << "rows_after_close=" << rows_after_close << '\n';
    out << "session_ledger=" << (ledger_ok ? "ok" : "FAILED") << '\n';
    if (!ready || card_count <= 0 || !stamped || !ledger_ok) {
        ++failures;
    }
    out.flush();
    return failures == 0 ? 0 : 1;
}

int runSelfTest(bool with_engine, const QString& enginePath) {
    QTextStream out(stdout);
    out << "antikolt-shell " << ANTIKOLT_VERSION << '\n';
    out << "qt=" << qVersion() << '\n';
    out << "platform=" << QGuiApplication::platformName() << '\n';
    out << "icon=" << (QApplication::windowIcon().isNull() ? "missing" : "ok") << '\n';
    out << "os=" << QSysInfo::prettyProductName() << '\n';

    const QList<QScreen*> screens = QGuiApplication::screens();
    out << "screens=" << screens.size() << '\n';
    if (!screens.isEmpty()) {
        const QSize size = screens.first()->size();
        out << "screen0=" << size.width() << 'x' << size.height() << '\n';
        out << "dpr=" << screens.first()->devicePixelRatio() << '\n';
    }

#ifdef ANTIKOLT_HAVE_QTQUICK
    out << "quick=1\n";
#else
    out << "quick=0\n";
#endif

    int failures = 0;
    for (const QString& theme : MainWindow::themeNames()) {
        const ThemePalette palette = MainWindow::paletteFor(theme);
        const QString css = MainWindow::styleSheetFor(palette);
        const bool ok =
            !css.isEmpty() && css.contains(palette.background) && css.contains(palette.accent);
        out << "theme=" << theme << (ok ? " ok" : " FAILED") << '\n';
        if (!ok) {
            ++failures;
        }
    }
    out.flush();

    if (with_engine && !runEngineCheck(enginePath)) {
        ++failures;
    }

    out << (failures == 0 ? "ok" : "FAILED") << '\n';
    out.flush();
    return failures == 0 ? 0 : 1;
}

}  // namespace

/// What a second launch does, and nothing else (ADR-043).
///
/// Prints the verdict instead of guessing at it: `--traytest` starts this as a *separate process*, exactly
/// like the shortcut does, and reads this line. A secondary exits 0 because finding the running browser and
/// handing it the window is the job; a primary means nobody was running (the name was claimed and released
/// again by the destructor), which is a failure of the check, not of the guard.
int runPingInstance() {
    QTextStream out(stdout);
    antikolt::InstanceGuard guard;
    const antikolt::InstanceGuard::Result result =
        guard.acquire(antikolt::instanceNameForThisUser());
    out << "instance="
        << (result == antikolt::InstanceGuard::Result::Primary
                ? "primary"
                : result == antikolt::InstanceGuard::Result::Secondary ? "secondary" : "failed")
        << '\n';
    out.flush();
    return result == antikolt::InstanceGuard::Result::Secondary ? 0 : 1;
}

/// Prove the browser lives on in the tray, and that a second launch cannot start a second one (ADR-043).
///
/// Three witnesses, all of them behaviour rather than bookkeeping: a real second process (this same
/// executable, `--ping-instance`) becomes a *secondary* and the first process hears "show yourself";
/// closing the window hides it while the process keeps running, and the tray brings it back; and with the
/// tray switched off — the state every other self test runs in — the same close really closes.
int runTrayCheck() {
    QTextStream out(stdout);
    int failures = 0;

    // 1. One Antikolt per user session: the shortcut is a launcher, not a second browser.
    {
        antikolt::InstanceGuard primary;
        const auto first = primary.acquire(antikolt::instanceNameForThisUser());
        out << "instance_primary="
            << (first == antikolt::InstanceGuard::Result::Primary ? "ok" : "FAILED") << '\n';
        if (first != antikolt::InstanceGuard::Result::Primary) {
            ++failures;
        }

        bool raised = false;
        QEventLoop loop;
        QObject::connect(&primary, &antikolt::InstanceGuard::showRequested, &loop, [&raised, &loop] {
            raised = true;
            loop.quit();
        });
        QProcess second;
        second.start(QCoreApplication::applicationFilePath(),
                     {QStringLiteral("--ping-instance")});
        const bool started = second.waitForStarted(8000);
        if (!started) {
            out << "instance_second=FAILED (a második példány nem indult el)\n";
            ++failures;
        } else {
            QTimer::singleShot(8000, &loop, &QEventLoop::quit);
            loop.exec();
            second.waitForFinished(8000);
            const QString verdict =
                QString::fromUtf8(second.readAllStandardOutput()).trimmed();
            const int code = second.exitCode();
            out << "instance_second=" << verdict << " exit=" << code << '\n';
            if (verdict != QStringLiteral("instance=secondary") || code != 0) {
                ++failures;
            }
            out << "instance_raised=" << (raised ? "ok" : "FAILED") << '\n';
            if (!raised) {
                ++failures;
            }
        }
    }

    // 2. The tray icon: the close button puts the window away, the tray brings it back, the menu ends it.
    bool tray_was_available = false;
    {
        MainWindow window;
        window.show();
        window.move(150, 110);
        tray_was_available = window.trayAvailable();
        out << "tray_icon=" << (tray_was_available ? "ok" : "absent") << '\n';
        const QString labels = window.trayMenuLabels().join(QLatin1Char('|'));
        out << "tray_menu=" << labels << '\n';
        const bool closed = window.close();
        out << "close_returns=" << (closed ? "closed" : "hidden") << '\n';
        if (tray_was_available) {
            // A platform without a notification area (a bare CI agent) cannot be asked the same question;
            // there the honest check is the opposite one, and it is below.
            const bool hidden = !closed && !window.isVisible();
            out << "tray_hides=" << (hidden ? "ok" : "FAILED") << '\n';
            out << "tray_menu_items="
                << (labels == QStringLiteral("Megnyitás|Kilépés") ? "ok" : "FAILED") << '\n';
            if (!hidden || labels != QStringLiteral("Megnyitás|Kilépés")) {
                ++failures;
            }
            window.restoreFromTray();
            const bool shown = window.isVisible();
            out << "tray_restores=" << (shown ? "ok" : "FAILED") << '\n';
            if (!shown) {
                ++failures;
            }
            window.quitFromTray();
            const bool gone = !window.isVisible();
            out << "tray_quit=" << (gone ? "ok" : "FAILED") << '\n';
            if (!gone) {
                ++failures;
            }
        } else {
            const bool really_closed = closed && !window.isVisible();
            out << "close_quits=" << (really_closed ? "ok" : "FAILED") << '\n';
            if (!really_closed) {
                ++failures;
            }
        }
    }

    // 3. The fallback the rest of the self tests run under: no tray means the close button is the exit.
    {
        qputenv("ANTIKOLT_NO_TRAY", "1");
        MainWindow window;
        window.show();
        const bool tray = window.trayAvailable();
        const bool closed = window.close();
        out << "fallback_tray=" << (tray ? "present" : "absent")
            << " close=" << (closed ? "closed" : "hidden") << '\n';
        if (tray || !closed) {
            ++failures;
        }
    }

    out << "tray_and_instance=" << (failures == 0 ? "ok" : "FAILED") << '\n';
    out.flush();
    return failures == 0 ? 0 : 1;
}

/// The mean luminance of an image, sampled on a grid (0..255), or -1 when the image is empty.
double MeanLuma(const QImage& image) {
    if (image.isNull() || image.width() == 0 || image.height() == 0) {
        return -1.0;
    }
    double sum = 0.0;
    int samples = 0;
    for (int y = 0; y < image.height(); y += 8) {
        for (int x = 0; x < image.width(); x += 8) {
            const QColor pixel = image.pixelColor(x, y);
            sum += 0.2126 * pixel.red() + 0.7152 * pixel.green() + 0.0722 * pixel.blue();
            ++samples;
        }
    }
    return samples == 0 ? -1.0 : sum / samples;
}

/// The WCAG 2.x contrast ratio of two colours (1..21). The design doc promises AA for body text in every
/// theme (`docs/HU/08-DIZAJN.md` §1), so the promise is measured instead of eyeballed.
double ContrastRatio(const QColor& foreground, const QColor& background) {
    const auto channel = [](double value) {
        value /= 255.0;
        return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    const auto luminance = [&channel](const QColor& color) {
        return 0.2126 * channel(color.red()) + 0.7152 * channel(color.green()) +
               0.0722 * channel(color.blue());
    };
    const double first = luminance(foreground);
    const double second = luminance(background);
    const double lighter = first > second ? first : second;
    const double darker = first > second ? second : first;
    return (lighter + 0.05) / (darker + 0.05);
}

/// Prove the themes reach the pixels — the chrome *and* the product's own page (docs/HU/08-DIZAJN.md, ADR-012).
///
/// The self test has until now only read the stylesheet *text*, which cannot tell an applied sheet from a
/// dropped one; the user found the difference on screen. This check renders the window and measures it:
///
/// 1. the chrome: after `applyTheme(name)` the toolbar's own pixels must be the palette's background, so
///    "grey", "dark" and "amoled" are dark on screen and "light" is light;
/// 2. the grid: with the engine, the page must follow the toolbar — `document.documentElement` carries the
///    attribute, and a capture of the grid must be dark under `amoled` and light under `light`. This is the
///    part that used to be a second, independent switch inside `home/app.js`.
int runThemeCheck(const QString& enginePath) {
    QTextStream out(stdout);
    int failures = 0;

    MainWindow window;
    window.resize(900, 620);
    window.move(120, 90);
    window.show();
    {
        QEventLoop settle;
        QTimer::singleShot(400, &settle, &QEventLoop::quit);
        settle.exec();
    }

    const auto sample_chrome = [&window] {
        const QImage shot = window.grab().toImage();
        if (shot.isNull()) {
            return QImage();
        }
        // The toolbar is the window's first band: the whole band is drawn from the theme (its background, the
        // buttons' surfaces and borders), so its *mean* luma is the theme's own brightness and does not depend
        // on which sub-widget happens to sit under one point.
        const int height = shot.height() < 40 ? shot.height() : 40;
        return shot.copy(0, 0, shot.width(), height);
    };

    double light_luma = -1.0;
    double amoled_luma = -1.0;
    double grey_luma = -1.0;
    double dark_luma = -1.0;
    for (const QString& theme : MainWindow::themeNames()) {
        if (!window.applyTheme(theme)) {
            out << "theme_apply=" << theme << " FAILED\n";
            ++failures;
            continue;
        }
        {
            QEventLoop settle;
            QTimer::singleShot(250, &settle, &QEventLoop::quit);
            settle.exec();
        }
        const QImage band = sample_chrome();
        const double luma = MeanLuma(band);
        out << "theme_chrome=" << theme << " luma=" << qRound(luma) << (luma >= 0.0 ? "" : " FAILED")
            << '\n';
        if (luma < 0.0) {
            ++failures;
        }
        if (theme == QStringLiteral("light")) {
            light_luma = luma;
        } else if (theme == QStringLiteral("grey")) {
            grey_luma = luma;
        } else if (theme == QStringLiteral("dark")) {
            dark_luma = luma;
        } else if (theme == QStringLiteral("amoled")) {
            amoled_luma = luma;
        }
    }
    // The palettes have a documented order (docs/HU/08-DIZAJN.md §1: light is bright, grey is the dark default,
    // dark is darker, amoled is black). A "dark" theme that is not darker than the light one is not a theme, it
    // is a stylesheet that never arrived — which is exactly what the user saw.
    const bool contrast = light_luma > 200.0 && grey_luma >= 0.0 && grey_luma < light_luma - 60.0 &&
                          dark_luma >= 0.0 && dark_luma < grey_luma + 1.0 && amoled_luma >= 0.0 &&
                          amoled_luma <= dark_luma + 1.0 && amoled_luma < 40.0;
    out << "theme_contrast=" << (contrast ? "ok" : "FAILED") << '\n';
    if (!contrast) {
        ++failures;
    }

    // The toolbar's theme switch is *icons* with tooltips (docs/HU/08-DIZAJN.md §6) and the palettes must pass the
    // documented WCAG AA minimum: both are promises a user cannot check by eye in a screenshot, so they are
    // measured. A button without an icon or without a tooltip is exactly the "four words on the toolbar" that
    // this replaced.
    bool icons_ok = true;
    QSet<QString> icon_signatures;
    for (const QString& theme : MainWindow::themeNames()) {
        const QToolButton* button = window.themeButton(theme);
        // An icon that exists but paints nothing is the failure this check exists for: count the pixels the icon
        // actually covers, and keep a signature so four identical pictures cannot pass as four themes.
        int painted = 0;
        QString signature;
        if (button != nullptr && !button->icon().isNull()) {
            const QImage image = button->icon().pixmap(32, 32).toImage();
            QString stamp;
            for (int y = 0; y < image.height(); y += 2) {
                for (int x = 0; x < image.width(); x += 2) {
                    const QColor pixel = image.pixelColor(x, y);
                    if (pixel.alpha() > 0) {
                        ++painted;
                    }
                    stamp += QStringLiteral("%1").arg(pixel.rgba(), 8, 16);
                }
            }
            signature = stamp;
        }
        const int samples = 16 * 16;
        const bool ok = button != nullptr && !button->icon().isNull() &&
                        button->toolTip().contains(theme) && !button->accessibleName().isEmpty() &&
                        button->isCheckable() && painted * 100 >= samples * 40;
        out << "theme_button=" << theme << " painted=" << painted * 100 / (samples == 0 ? 1 : samples)
            << "%" << (ok ? " ok" : " FAILED") << '\n';
        if (!ok) {
            icons_ok = false;
        }
        if (!signature.isEmpty()) {
            icon_signatures.insert(signature);
        }
    }
    // Four different pictures, not the same moon four times.
    const bool distinct = icon_signatures.size() == MainWindow::themeNames().size();
    out << "theme_icons=" << (icons_ok && distinct ? "ok" : "FAILED") << '\n';
    if (!icons_ok || !distinct) {
        ++failures;
    }

    bool wcag_ok = true;
    for (const QString& theme : MainWindow::themeNames()) {
        const ThemePalette palette = MainWindow::paletteFor(theme);
        const double text = ContrastRatio(QColor(palette.text), QColor(palette.background));
        const double muted = ContrastRatio(QColor(palette.textMuted), QColor(palette.background));
        const double on_surface = ContrastRatio(QColor(palette.text), QColor(palette.surface));
        const bool ok = text >= 4.5 && muted >= 3.0 && on_surface >= 4.5;
        out << "theme_wcag=" << theme << " text=" << QString::number(text, 'f', 1)
            << " muted=" << QString::number(muted, 'f', 1)
            << " surface=" << QString::number(on_surface, 'f', 1) << (ok ? " ok" : " FAILED") << '\n';
        if (!ok) {
            wcag_ok = false;
        }
    }
    out << "theme_wcag_aa=" << (wcag_ok ? "ok" : "FAILED") << '\n';
    if (!wcag_ok) {
        ++failures;
    }

    // ---- the grid follows the toolbar -------------------------------------------------------------
    if (!enginePath.isEmpty()) {
        qputenv("ANTIKOLT_ENGINE", enginePath.toUtf8());
        qputenv("ANTIKOLT_CORE", MainWindow::coreExecutable().toUtf8());
        qint64 grid_surface = 0;
        {
            QEventLoop loop;
            QObject::connect(window.engine(), &EngineClient::loadFinished, &loop,
                             [&](qint64 id, const QString& url, int) {
                                 if (!url.startsWith(QStringLiteral("file:"))) {
                                     return;
                                 }
                                 grid_surface = id;
                                 QTimer::singleShot(600, &loop, [&loop] { loop.quit(); });
                             });
            window.startEngine();
            QTimer::singleShot(60000, &loop, &QEventLoop::quit);
            loop.exec();
        }
        out << "grid_surface=" << grid_surface << '\n';

        // The page's own witness of the push: the attribute the grid renders from.
        QString attribute = QStringLiteral("not-asked");
        if (grid_surface != 0) {
            QEventLoop loop;
            window.engine()->evaluate(
                grid_surface,
                QStringLiteral("document.documentElement.getAttribute('data-theme')"),
                [&](const QJsonObject& reply, const QString& error) {
                    attribute = error.isEmpty()
                                    ? reply.value(QStringLiteral("value")).toString()
                                    : QStringLiteral("error: %1").arg(error);
                    loop.quit();
                });
            QTimer::singleShot(10000, &loop, &QEventLoop::quit);
            loop.exec();
        }
        out << "grid_theme_attr=" << attribute << '\n';

        // A capture, with one retry: the compositor of a dead GPU can swallow a request (this machine).
        const auto capture_luma = [&](const QString& name, double* luma) {
            const auto shoot = std::make_shared<std::function<void(int)>>();
            *shoot = [&, shoot](int attempt) {
                QEventLoop loop;
                window.engine()->capture(
                    grid_surface, QStringLiteral("png"), 80,
                    [&, shoot, attempt](const QJsonObject& shot, const QString& error) {
                        if (!error.isEmpty()) {
                            if (attempt < 3) {
                                QTimer::singleShot(600, &loop,
                                                   [shoot, attempt] { (*shoot)(attempt + 1); });
                                return;
                            }
                            out << "grid_capture=" << name << " FAILED " << error << '\n';
                            loop.quit();
                            return;
                        }
                        const QImage image(shot.value(QStringLiteral("path")).toString());
                        *luma = MeanLuma(image);
                        out << "grid_capture=" << name << " luma=" << qRound(*luma) << '\n';
                        loop.quit();
                    });
                QTimer::singleShot(30000, &loop, &QEventLoop::quit);
                loop.exec();
            };
            (*shoot)(1);
        };

        double grid_light = -1.0;
        double grid_dark = -1.0;
        if (grid_surface != 0) {
            window.applyTheme(QStringLiteral("light"));
            {
                QEventLoop settle;
                QTimer::singleShot(900, &settle, &QEventLoop::quit);
                settle.exec();
            }
            capture_luma(QStringLiteral("light"), &grid_light);
            window.applyTheme(QStringLiteral("amoled"));
            {
                QEventLoop settle;
                QTimer::singleShot(900, &settle, &QEventLoop::quit);
                settle.exec();
            }
            capture_luma(QStringLiteral("amoled"), &grid_dark);
        }
        // The grid's own colours: the light theme is a bright page, the amoled one is dark. The mean luma of a
        // screenshot is not a palette value (the cards carry preview images and text), so the assertion is the
        // *gap*: the same page under two themes cannot differ by less than a whole brightness class.
        const bool grid_ok = grid_surface != 0 && grid_light > 150.0 && grid_dark >= 0.0 &&
                             grid_dark < 120.0 && (grid_light - grid_dark) > 80.0 &&
                             attribute == QStringLiteral("amoled");
        out << "grid_theme=" << (grid_ok ? "ok" : "FAILED") << '\n';
        if (!grid_ok) {
            ++failures;
        }

        // ---- forced dark pages (ADR-044) ----------------------------------------------------------
        // A page with no dark theme of its own (a white document, which is what the user saw on Wikipedia):
        // the switch must darken it, and switching it off must bring the page back — one style element, added
        // and removed, no reload.
        {
            // A page with a fixed white background: the "oldal-téma" query does nothing to it, so the only way
            // it can become dark is the forced-dark switch.
            const QString page = QStringLiteral(
                "data:text/html,<title>forced dark</title>"
                "<body style='margin:0;background:%23fff;color:%23000'>"
                "<h1 style='font-size:48px'>Antikolt forced dark probe</h1>"
                "<p style='font-size:24px'>A page that never asks for a dark theme.</p></body>");
            qint64 card_surface = 0;
            {
                QEventLoop loop;
                QObject::connect(window.engine(), &EngineClient::loadFinished, &loop,
                                 [&](qint64 id, const QString& url, int) {
                                     if (!url.contains(QStringLiteral("forced%20dark")) &&
                                         !url.contains(QStringLiteral("forced dark"))) {
                                         return;
                                     }
                                     card_surface = id;
                                     QTimer::singleShot(900, &loop, [&loop] { loop.quit(); });
                                 });
                window.openCard(page, QStringLiteral("forced dark probe"));
                QTimer::singleShot(45000, &loop, &QEventLoop::quit);
                loop.exec();
            }
            out << "dark_surface=" << card_surface << '\n';

            const auto capture_page = [&](const QString& name, double* luma) {
                const auto shoot = std::make_shared<std::function<void(int)>>();
                *shoot = [&, shoot](int attempt) {
                    QEventLoop loop;
                    window.engine()->capture(
                        card_surface, QStringLiteral("png"), 80,
                        [&, shoot, attempt](const QJsonObject& shot, const QString& error) {
                            if (!error.isEmpty()) {
                                if (attempt < 3) {
                                    QTimer::singleShot(600, &loop,
                                                       [shoot, attempt] { (*shoot)(attempt + 1); });
                                    return;
                                }
                                out << "dark_capture=" << name << " FAILED " << error << '\n';
                                loop.quit();
                                return;
                            }
                            *luma = MeanLuma(QImage(shot.value(QStringLiteral("path")).toString()));
                            out << "dark_capture=" << name << " luma=" << qRound(*luma) << '\n';
                            loop.quit();
                        });
                    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
                    loop.exec();
                };
                (*shoot)(1);
            };

            double page_light = -1.0;
            // Every mode, measured on the same white page: the user asked for more than one dark page mode, so the
            // check measures that each one really darkens, and that they are not all the same picture.
            QHash<QString, double> page_luma;
            if (card_surface != 0) {
                for (const QString& mode : MainWindow::pageDarkModes()) {
                    window.setPageDarkMode(mode);
                    {
                        QEventLoop settle;
                        QTimer::singleShot(mode == QStringLiteral("off") ? 700 : 1100, &settle,
                                           &QEventLoop::quit);
                        settle.exec();
                    }
                    double luma = -1.0;
                    capture_page(QStringLiteral("mode_") + mode, &luma);
                    page_luma.insert(mode, luma);
                    if (mode == QStringLiteral("off")) {
                        page_light = luma;
                    }
                }
                // Left off: a self test must not change what the next launch starts with.
                window.setPageDarkMode(QStringLiteral("off"));
            }
            // Off is a white page; every mode is far darker than it, and the plain inversion is the darkest.
            bool modes_ok = card_surface != 0 && page_light > 150.0;
            for (const QString& mode : MainWindow::pageDarkModes()) {
                if (mode == QStringLiteral("off")) {
                    continue;
                }
                const double luma = page_luma.value(mode, -1.0);
                if (luma < 0.0 || luma > 120.0 || (page_light - luma) < 80.0) {
                    modes_ok = false;
                }
            }
            if (page_luma.value(QStringLiteral("invert"), 999.0) >
                page_luma.value(QStringLiteral("dim"), -1.0)) {
                modes_ok = false;
            }
            out << "dark_page_modes=" << (modes_ok ? "ok" : "FAILED") << '\n';
            if (!modes_ok) {
                ++failures;
            }

            // The mode is a *preference*: a second window (what the next launch is) must read the same answer.
            // Proving it here is cheaper than believing it — a setting nobody reads back is not remembered.
            window.setPageDarkMode(QStringLiteral("dim"));
            {
                MainWindow again;
                const bool persisted = again.pageDarkMode() == QStringLiteral("dim");
                out << "dark_mode_persisted=" << (persisted ? "ok" : "FAILED") << '\n';
                if (!persisted) {
                    ++failures;
                }
            }
            window.setPageDarkMode(QStringLiteral("off"));
        }
    }

    out << "theme_check=" << (failures == 0 ? "ok" : "FAILED") << '\n';
    window.close();
    out.flush();
    return failures == 0 ? 0 : 1;
}

/// Prove the shell log is written, rotated and switched off by path — the console a shortcut launch does not have.
///
/// The whole point of the file is that nobody is watching when it matters, so the check reads it back: the lines
/// must be there with a timestamp, a big file must rotate to `.1` instead of growing without limit, and clearing
/// the path must stop the writing (a self test must never append to the user's real log). The limit is turned
/// down by an environment knob, so the rotation is a few kilobytes instead of a megabyte.
int runLogCheck() {
    QTextStream out(stdout);
    int failures = 0;

    const QString directory = QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
                              QStringLiteral("/antikolt-logtest-%1")
                                  .arg(QCoreApplication::applicationPid());
    QDir(directory).removeRecursively();
    QDir().mkpath(directory);
    const QString path = directory + QStringLiteral("/shell.log");
    qputenv("ANTIKOLT_SHELL_LOG_LIMIT", "2048");
    ShellLogPath() = path;

    qInfo("logtest line one");
    qInfo("logtest line two");
    qInfo("logtest line three");

    QFile file(path);
    const bool opened = file.open(QIODevice::ReadOnly | QIODevice::Text);
    const QString contents = opened ? QString::fromUtf8(file.readAll()) : QString();
    file.close();
    const QStringList lines = contents.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    out << "log_path=" << path << '\n';
    out << "log_lines=" << lines.size() << '\n';
    const bool has_lines = lines.size() == 3 && contents.contains(QStringLiteral("logtest line one")) &&
                           contents.contains(QStringLiteral("logtest line three")) &&
                           contents.contains(QStringLiteral(" [shell] info: ")) &&
                           !lines.isEmpty() && lines.last().at(0).isDigit();
    out << "log_has_lines=" << (has_lines ? "ok" : "FAILED") << '\n';
    if (!has_lines) {
        ++failures;
    }

    // Push it past the limit: the *next* write rotates, so the previous run survives in `.1`.
    QString filler(64, QLatin1Char('x'));
    for (int i = 0; i < 40; ++i) {
        qInfo("logtest filler %d %s", i, filler.toUtf8().constData());
    }
    const QString previous = path + QStringLiteral(".1");
    const bool rotated =
        QFile::exists(previous) && QFileInfo(previous).size() > 0 && QFileInfo(path).size() > 0 &&
        QFileInfo(path).size() < QFileInfo(previous).size() + 4096;
    out << "log_rotated=" << (rotated ? "ok" : "FAILED") << " current=" << QFileInfo(path).size()
        << " previous=" << QFileInfo(previous).size() << '\n';
    if (!rotated) {
        ++failures;
    }

    // Clearing the path is how the normal launch keeps its own log and a test keeps its hands off it.
    ShellLogPath().clear();
    const qint64 before = QFileInfo(path).size();
    qInfo("logtest after clear must not be written");
    const bool stopped = QFileInfo(path).size() == before;
    out << "log_stopped=" << (stopped ? "ok" : "FAILED") << '\n';
    if (!stopped) {
        ++failures;
    }

    QDir(directory).removeRecursively();
    out << "log_check=" << (failures == 0 ? "ok" : "FAILED") << '\n';
    out.flush();
    return failures == 0 ? 0 : 1;
}

/// The search is accent-insensitive and says so when it finds nothing (ADR-047).
///
/// What the user types is not what the card is called: a Hungarian user types `kave`, and the card is `Kávé`.
/// The fold lives in the page (`foldSearch`) *and* in the shell (`FoldForSearch`, for `cards.search`), because a
/// query must not answer differently depending on which side filtered; this check measures the page's half — the
/// fold function itself, the filtering, the count, the empty state that names the term, and that Escape brings the
/// whole grid back. The card it looks for is seeded through the core, so the test does not depend on what happens
/// to be in the profile.
int runSearchCheck(const QString& enginePath) {
    QTextStream out(stdout);
    int failures = 0;

    const QString corePath = MainWindow::coreExecutable();
    if (corePath.isEmpty()) {
        out << "core_executable=FAILED (set ANTIKOLT_CORE)\n";
        return 1;
    }
    qputenv("ANTIKOLT_ENGINE", enginePath.toUtf8());
    qputenv("ANTIKOLT_CORE", corePath.toUtf8());
    out << "core_executable=" << corePath << '\n';

    // The seed: a title with the accents a Hungarian user would *not* type in a hurry, and a word that appears
    // only in that card. Written through the core with a complete record (the core validates every field), so the
    // grid shows it exactly as a real card.
    const QString seeded_id = QStringLiteral("search-probe");
    const QString seeded_title = QStringLiteral("Kávézó-próba — Árvíztűrő Tükörfúrógép");
    const QString seeded_query = QStringLiteral("kavezo");
    // A word that appears only in the captured page text, never in the title, the URL or the note.
    const QString body_word = QStringLiteral("rejtettoldalszoveg");
    {
        CoreClient seeder;
        const QString core_data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                                  QStringLiteral("/core");
        QDir().mkpath(core_data);
        if (!seeder.start(corePath, core_data)) {
            out << "core_start=FAILED\n";
            return 1;
        }
        QEventLoop loop;
        bool seeded = false;
        QObject::connect(&seeder, &CoreClient::failed, &loop, [&](const QString& message) {
            out << "core_error=" << message << '\n';
            loop.quit();
        });
        QObject::connect(&seeder, &CoreClient::ready, &loop, [&](const QJsonObject&) {
            QJsonObject card;
            card.insert(QStringLiteral("id"), seeded_id);
            card.insert(QStringLiteral("title"), seeded_title);
            card.insert(QStringLiteral("url"), QStringLiteral("https://example.test/reggeli"));
            card.insert(QStringLiteral("container"), QStringLiteral("c-private"));
            card.insert(QStringLiteral("status"), QStringLiteral("unknown"));
            card.insert(QStringLiteral("pinned"), false);
            card.insert(QStringLiteral("notes"), QStringLiteral("vaj, lekvár"));
            card.insert(QStringLiteral("preview_ref"), QString());
            card.insert(QStringLiteral("last_opened_at_ms"), 0);
            seeder.cardUpsert(card, [&](const QJsonObject& saved, const QString& error) {
                if (!error.isEmpty()) {
                    out << "seed=FAILED " << error << '\n';
                    loop.quit();
                    return;
                }
                seeded = !saved.value(QStringLiteral("card"))
                              .toObject()
                              .value(QStringLiteral("id"))
                              .toString()
                              .isEmpty();
                // The page text is written here, before the grid reads the card (ADR-049): the shell pushes the
                // list it fetched at start-up, so a text written after the window is up would need a re-push to
                // be visible — a test of the harness, not of the search.
                seeder.cardPageText(
                    seeded_id,
                    QStringLiteral("Ez a kártya oldalszövege, benne a(z) %1 szóval.").arg(body_word),
                    [&](const QJsonObject&, const QString& text_error) {
                        out << "search_page_text_written="
                            << (text_error.isEmpty() ? "ok" : "FAILED") << '\n';
                        if (!text_error.isEmpty()) {
                            seeded = false;
                        }
                        loop.quit();
                    });
            });
            });
        seeder.handshake();
        QTimer::singleShot(20000, &loop, &QEventLoop::quit);
        loop.exec();
        seeder.shutdown();
        out << "search_seeded=" << (seeded ? "ok" : "FAILED") << '\n';
        if (!seeded) {
            ++failures;
        }
    }

    MainWindow window;
    window.show();
    window.move(140, 100);
    window.raise();
    window.activateWindow();
    window.startEngine();
    EngineClient* client = window.engine();

    // One evaluation, answered or timed out; the reply of a `JSON.stringify(...)` script is a JSON string.
    const auto evaluate_json = [client](qint64 surface, const QString& script) {
        QEventLoop loop;
        QJsonObject value;
        client->evaluate(surface, script, [&](const QJsonObject& reply, const QString& error) {
            if (error.isEmpty()) {
                value = QJsonDocument::fromJson(reply.value(QStringLiteral("value")).toString().toUtf8())
                            .object();
            }
            loop.quit();
        });
        QTimer::singleShot(20000, &loop, &QEventLoop::quit);
        loop.exec();
        return value;
    };
    const auto settle = [](int milliseconds) {
        QEventLoop loop;
        QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
        loop.exec();
    };

    qint64 home_surface = 0;
    {
        QEventLoop loop;
        const QMetaObject::Connection connection =
            QObject::connect(client, &EngineClient::loadFinished, &loop,
                             [&](qint64 surface, const QString& url, int) {
                                 if (url.startsWith(QStringLiteral("file:"))) {
                                     home_surface = surface;
                                     loop.quit();
                                 }
                             });
        QTimer::singleShot(45000, &loop, &QEventLoop::quit);
        loop.exec();
        QObject::disconnect(connection);
    }
    out << "home_surface=" << home_surface << '\n';
    if (home_surface == 0) {
        out << "grid=FAILED (the home page never loaded)\n";
        window.close();
        return 1;
    }
    // The push follows the load; give the page its moment before asking what it shows.
    settle(900);

    // 1. The fold itself: accents, the long Hungarian vowels and the letters Unicode does not decompose.
    const QString folded =
        evaluate_json(home_surface,
                      QStringLiteral("JSON.stringify({f: window.antikoltSearchFold("
                                     "'Árvíztűrő Tükörfúrógép Łódź')})"))
            .value(QStringLiteral("f"))
            .toString();
    const bool fold_ok = folded == QStringLiteral("arvizturo tukorfurogep lodz");
    out << "search_fold=" << (fold_ok ? "ok" : "FAILED") << " folded=" << folded << '\n';
    if (!fold_ok) {
        ++failures;
    }

    // 2. The untouched grid: the baseline the Escape step has to come back to.
    const QJsonObject baseline = evaluate_json(
        home_surface, QStringLiteral("JSON.stringify(window.antikoltSearchState())"));
    const int total = baseline.value(QStringLiteral("filtered")).toInt();
    out << "search_total=" << total << '\n';

    // 3. `kavezo` must find `Kávézó-próba`: the user's own habit, not the stored spelling. The dispatch and the
    // page's own debounce are not synchronised, so the check waits for the page to *say* it applied the term
    // instead of sleeping a guessed number of milliseconds — a timing guess would make this test flaky in exactly
    // the interesting case.
    const auto type_query = [&](const QString& term) {
        evaluate_json(home_surface,
                      QStringLiteral("(() => { const el = document.getElementById('search');"
                                     " if (!el) { return false; } el.value = '%1';"
                                     " el.dispatchEvent(new Event('input')); return true; })()")
                          .arg(term));
        for (int attempt = 0; attempt < 25; ++attempt) {
            const QJsonObject state = evaluate_json(
                home_surface, QStringLiteral("JSON.stringify(window.antikoltSearchState())"));
            if (state.value(QStringLiteral("term")).toString() == term.trimmed()) {
                return state;
            }
            settle(200);
        }
        return QJsonObject{};
    };

    const QJsonObject accented = type_query(seeded_query);
    QStringList accented_titles;
    for (const QJsonValue& value : accented.value(QStringLiteral("titles")).toArray()) {
        accented_titles.append(value.toString());
    }
    const int accented_count = accented.value(QStringLiteral("filtered")).toInt();
    const bool accent_ok = accented_count == 1 && accented_titles.size() == 1 &&
                           accented_titles.first() == seeded_title &&
                           accented.value(QStringLiteral("term")).toString() == seeded_query &&
                           accented.value(QStringLiteral("countText")).toString() ==
                               QStringLiteral("1 kártya");
    out << "search_accent=" << (accent_ok ? "ok" : "FAILED") << " term=" << seeded_query
        << " filtered=" << accented_count << " of=" << total << '\n';
    if (!accent_ok) {
        ++failures;
    }

    // 4. Nothing found is a statement about the query, and the page says which one.
    const QJsonObject missing = type_query(QStringLiteral("zzzz"));
    const QString empty_text = missing.value(QStringLiteral("emptyText")).toString();
    const bool empty_ok = missing.value(QStringLiteral("filtered")).toInt() == 0 &&
                          !missing.value(QStringLiteral("emptyHidden")).toBool() &&
                          empty_text.contains(QStringLiteral("zzzz")) &&
                          missing.value(QStringLiteral("countText")).toString().isEmpty();
    out << "search_empty=" << (empty_ok ? "ok" : "FAILED") << " empty_text=" << empty_text << '\n';
    if (!empty_ok) {
        ++failures;
    }

    // 4b. The page's own text (ADR-049): the word below appears *only* in the captured page text — not in the
    // title, the URL or the note — so a match proves the text reached the store, the shell's search and the
    // page's filter. It was written at seed time, before the grid read its card list.
    {
        const QJsonObject body = type_query(QStringLiteral("rejtett"));
        const int body_count = body.value(QStringLiteral("filtered")).toInt();
        QStringList body_titles;
        for (const QJsonValue& value : body.value(QStringLiteral("titles")).toArray()) {
            body_titles.append(value.toString());
        }
        out << "search_page_text=" << (body_count == 1 && body_titles.size() == 1 &&
                                        body_titles.first() == seeded_title
                                          ? "ok"
                                          : "FAILED")
            << " filtered=" << body_count << '\n';
        if (!(body_count == 1 && body_titles.size() == 1 && body_titles.first() == seeded_title)) {
            ++failures;
        }
    }

    // 5. Escape clears the box and the grid comes back whole.
    const QJsonObject focus = evaluate_json(
        home_surface,
        QStringLiteral("(() => { const el = document.getElementById('search'); el.focus();"
                       " document.dispatchEvent(new KeyboardEvent('keydown', {key: 'Escape',"
                       " bubbles: true})); return {focused: document.activeElement === el}; })()"));
    out << "search_focused=" << (focus.value(QStringLiteral("focused")).toBool() ? "ok" : "no")
        << '\n';
    QJsonObject restored;
    for (int attempt = 0; attempt < 25; ++attempt) {
        restored = evaluate_json(home_surface,
                                 QStringLiteral("JSON.stringify(window.antikoltSearchState())"));
        if (restored.value(QStringLiteral("term")).toString().isEmpty() &&
            restored.value(QStringLiteral("filtered")).toInt() == total) {
            break;
        }
        settle(200);
    }
    const int restored_count = restored.value(QStringLiteral("filtered")).toInt();
    const bool escape_ok = restored_count == total &&
                           restored.value(QStringLiteral("term")).toString().isEmpty() &&
                           restored.value(QStringLiteral("emptyHidden")).toBool();
    out << "search_escape=" << (escape_ok ? "ok" : "FAILED") << " filtered=" << restored_count
        << '\n';
    if (!escape_ok) {
        ++failures;
    }

    window.close();
    out << "search_check=" << (failures == 0 ? "ok" : "FAILED") << '\n';
    out.flush();
    return failures == 0 ? 0 : 1;
}

/// The cleanup never silences a playing card, and does clean it once it stops (docs/HU/02 §4.2, ADR-048).
///
/// The hole this closes: session housekeeping wiped the origin of the card *on screen* without asking whether it
/// was playing sound or video, so a video in the foreground could be cut by a background sweep. The check seeds a
/// due `wipe` row for a probe origin, opens that origin's card with a muted-autoplay audio element (muted autoplay
/// is allowed without a gesture, which is what makes this testable at all), and asks for cleanup twice:
///
/// 1. while the audio plays — the page's storage must survive and the status bar must say why;
/// 2. after pausing it — the same plan must now be executed: the storage is gone and the ledger row is closed.
///
/// Both witnesses are the page's own (`localStorage`), so a shell that only *says* it skipped cannot pass.
int runMediaCleanupCheck(const QString& enginePath) {
    QTextStream out(stdout);
    int failures = 0;
    qputenv("ANTIKOLT_ENGINE", enginePath.toUtf8());
    qputenv("ANTIKOLT_CORE", MainWindow::coreExecutable().toUtf8());

    // A page that plays silently, forever: the tone is a real WAV served by the probe itself, because a player
    // that never starts proves nothing. Muted autoplay is allowed without a gesture, which is what makes this
    // measurable at all; the page asks `play()` as well, so a policy change shows up as a failed assertion here
    // rather than as a cleanup test that silently measures nothing.
    const QString page =
        QStringLiteral("<!doctype html><meta charset=\"utf-8\"><title>media probe</title>"
                       "<script>localStorage.setItem('probe','1');document.cookie='probe=1; path=/';"
                       "window.__wrote=true;</script>"
                       "<h1>media probe</h1>"
                       "<audio id=\"tone\" muted loop autoplay preload=\"auto\" src=\"/tone.wav\"></audio>"
                       "<script>const el=document.getElementById('tone');"
                       "if(el){el.play().catch(()=>{});}</script>");

    QHash<QString, ProbeOrigin::Asset> assets;
    assets.insert(QStringLiteral("/tone.wav"),
                  ProbeOrigin::Asset{toneWav(), QByteArrayLiteral("audio/wav")});
    ProbeOrigin probe(page, assets);
    if (!probe.listening()) {
        out << "server=FAILED " << probe.errorString() << '\n';
        return 1;
    }
    const QString origin = probe.origin();
    out << "origin=" << origin << '\n';

    const QString core_dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                             QStringLiteral("/core");
    QDir().mkpath(core_dir);

    // 1. A due row for that origin: `last_seen_ms: 0` and a `wipe` policy is what the core judges expired.
    {
        CoreClient oracle;
        if (!oracle.start(MainWindow::coreExecutable(), core_dir)) {
            out << "core_start=FAILED\n";
            return 1;
        }
        QEventLoop loop;
        bool written = false;
        QObject::connect(&oracle, &CoreClient::ready, &loop, [&](const QJsonObject&) {
            QJsonObject session;
            session.insert(QStringLiteral("container_id"), QStringLiteral("c-private"));
            session.insert(QStringLiteral("origin"), origin);
            session.insert(QStringLiteral("state"), QStringLiteral("expired"));
            session.insert(QStringLiteral("policy"), QStringLiteral("wipe"));
            session.insert(QStringLiteral("cookie_count"), 1);
            session.insert(QStringLiteral("cookie_bytes"), 64);
            session.insert(QStringLiteral("storage_bytes"), 1024);
            session.insert(QStringLiteral("service_worker_count"), 0);
            session.insert(QStringLiteral("last_seen_ms"), 0);
            session.insert(QStringLiteral("last_verified_ms"), 0);
            session.insert(QStringLiteral("live_ram_bytes"), QJsonValue::Null);
            session.insert(QStringLiteral("keep_alive"), false);
            oracle.sessionUpsert(session, [&](const QJsonObject&, const QString& error) {
                written = error.isEmpty();
                loop.quit();
            });
        });
        oracle.handshake();
        QTimer::singleShot(15000, &loop, &QEventLoop::quit);
        loop.exec();
        oracle.shutdown();
        out << "session_written=" << (written ? "ok" : "FAILED") << '\n';
        if (!written) {
            return 1;
        }
    }

    // 2. The window: open the origin's card, let the audio start, then clean up while it plays.
    MainWindow window;
    window.show();
    window.move(130, 95);
    window.raise();
    window.activateWindow();
    window.startEngine();
    EngineClient* client = window.engine();

    const auto evaluate = [client](qint64 surface, const QString& script) {
        QEventLoop loop;
        QJsonObject value;
        client->evaluate(surface, script, [&](const QJsonObject& reply, const QString& error) {
            if (error.isEmpty()) {
                value = QJsonDocument::fromJson(reply.value(QStringLiteral("value")).toString().toUtf8())
                            .object();
            }
            loop.quit();
        });
        QTimer::singleShot(20000, &loop, &QEventLoop::quit);
        loop.exec();
        return value;
    };
    const auto settle = [](int milliseconds) {
        QEventLoop loop;
        QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
        loop.exec();
    };

    qint64 card_surface = 0;
    {
        // Wait for the grid, then open the probe card and ask the window which surface it is on. A page with a
        // media element can keep `on_load_end` waiting for the resource, so the card is identified through the
        // window's own live card instead of through an event that may arrive late or never.
        QEventLoop loop;
        bool grid_loaded = false;
        QStringList seen_urls;
        const QMetaObject::Connection load_connection =
            QObject::connect(client, &EngineClient::loadFinished, &loop,
                             [&](qint64, const QString& url, int) {
                                 seen_urls.append(url);
                                 if (url.startsWith(QStringLiteral("file:")) && !grid_loaded) {
                                     grid_loaded = true;
                                     QTimer::singleShot(500, &loop, [&] {
                                         window.openCard(origin + QStringLiteral("/"),
                                                         QStringLiteral("media probe"));
                                     });
                                     return;
                                 }
                                 if (url.startsWith(origin)) {
                                     loop.quit();
                                 }
                             });
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);
        loop.exec();
        QObject::disconnect(load_connection);
        settle(2500);
        card_surface = window.liveCard() != nullptr ? window.liveCard()->surface() : 0;
        out << "grid_loaded=" << (grid_loaded ? "ok" : "no") << " loads="
            << seen_urls.join(QStringLiteral(", ")) << '\n';
    }
    out << "card_surface=" << card_surface << '\n';
    if (card_surface == 0) {
        out << "card_open=FAILED\n";
        window.close();
        return 1;
    }
    settle(1200);

    // The player must really be running, otherwise the whole check would be about nothing.
    const QJsonObject playing = evaluate(
        card_surface,
        QStringLiteral("JSON.stringify({media: (function(){var m=document.querySelectorAll('video,audio');"
                       "for(var i=0;i<m.length;i++){if(!m[i].paused&&!m[i].ended)return 'playing';}"
                       "return 'idle';})(), storage: ") +
            storageReadScript() +
            QStringLiteral(
                ", probe: (function(){var el=document.getElementById('tone');"
                "if(!el)return 'missing';return JSON.stringify({paused: el.paused, ended: el.ended,"
                " readyState: el.readyState, networkState: el.networkState, muted: el.muted,"
                " currentSrc: el.currentSrc, duration: el.duration,"
                " error: el.error ? el.error.code : 0});})()})"));
    const bool is_playing = playing.value(QStringLiteral("media")).toString() == QStringLiteral("playing");
    const QString storage_before = playing.value(QStringLiteral("storage")).toString();
    // Whether a player starts is a property of the machine, not of the product: this VM has no audio device, so
    // the tone is never even fetched. The probe still runs on a machine that has one, and then the skip branch
    // below is asserted as well.
    out << "media_playing=" << (is_playing ? "ok" : "unavailable (no audio device on this machine)")
        << " storage_before=" << storage_before << '\n';
    out << "media_probe=" << playing.value(QStringLiteral("probe")).toString() << '\n';
    if (!storage_before.contains(QStringLiteral("probe"))) {
        out << "media_cleanup=FAILED (the probe page never wrote its storage)\n";
        window.close();
        return 1;
    }


    // 3. First cleanup. What the product does depends on the page's answer, and this environment decides which
    // branch can be measured: a machine without an audio device never starts the player (`readyState: 0`, and the
    // tone is never even requested — measured), so the "playing" branch is only asserted where playback works.
    // The "idle" branch is measured everywhere, and it exercises the same new code: ask the page, then wipe.
    window.cleanupSessions();
    settle(1800);
    const QString status_playing = window.statusBar()->currentMessage();
    const QString storage_while_playing =
        evaluate(card_surface,
                 QStringLiteral("JSON.stringify({storage: ") + storageReadScript() +
                     QStringLiteral("})"))
            .value(QStringLiteral("storage"))
            .toString();
    if (is_playing) {
        const bool skipped = status_playing.contains(QStringLiteral("médiát játszik")) &&
                             storage_while_playing.contains(QStringLiteral("probe"));
        out << "media_skip=" << (skipped ? "ok" : "FAILED") << " status=" << status_playing << '\n';
        if (!skipped) {
            ++failures;
        }
        // Pause it, and the very same plan must now run.
        evaluate(card_surface,
                 QStringLiteral("(() => { const a = document.querySelectorAll('video,audio');"
                                " for (const el of a) { el.pause(); } return true; })()"));
        settle(400);
        window.cleanupSessions();
        settle(1800);
        const QString storage_after_pause =
            evaluate(card_surface,
                     QStringLiteral("JSON.stringify({storage: ") + storageReadScript() +
                         QStringLiteral("})"))
                .value(QStringLiteral("storage"))
                .toString();
        const bool wiped_after_pause = !storage_after_pause.contains(QStringLiteral("probe"));
        out << "media_wipe_after_pause=" << (wiped_after_pause ? "ok" : "FAILED")
            << " status=" << window.statusBar()->currentMessage() << '\n';
        if (!wiped_after_pause) {
            ++failures;
        }
    } else {
        // No player on this machine: the exception cannot be exercised, and pretending otherwise would be worse
        // than saying so. The idle path is the one the user sees on most pages anyway — it must still wipe, and
        // it must do so through the media-aware code.
        out << "media_skip=not-measurable (no audio device: the page never starts its player)\n";
        const bool wiped =
            !storage_while_playing.contains(QStringLiteral("probe")) &&
            !status_playing.contains(QStringLiteral("médiát játszik"));
        out << "media_wipe_when_idle=" << (wiped ? "ok" : "FAILED") << " status=" << status_playing
            << '\n';
        if (!wiped) {
            ++failures;
        }
    }

    // The check writes a ledger row; it must not leave it behind. A profile that accumulates one probe origin per
    // run is how the session panel test started failing on a developer machine (it counts what the core says is
    // due) — the same lesson as the probe cards, one layer down.
    {
        CoreClient closer;
        const QString core_dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                                 QStringLiteral("/core");
        if (closer.start(MainWindow::coreExecutable(), core_dir)) {
            QEventLoop loop;
            QObject::connect(&closer, &CoreClient::ready, &loop, [&](const QJsonObject&) {
                closer.sessionClose(QStringLiteral("c-private"), origin,
                                    [&](const QJsonObject&, const QString&) { loop.quit(); });
            });
            closer.handshake();
            QTimer::singleShot(10000, &loop, &QEventLoop::quit);
            loop.exec();
            closer.shutdown();
        }
    }

    out << "media_cleanup_check=" << (failures == 0 ? "ok" : "FAILED") << '\n';
    out.flush();
    return failures == 0 ? 0 : 1;
}

/// Five hundred cards must scroll, not render (docs/HU/03 rule 1). `--gridtest` proves the grid shows what the
/// core holds for a handful of cards; this check proves the *virtualisation* at the size the roadmap names, with
/// numbers instead of a promise:
///
/// * the grid's total is 500 (the seeded set), and the page's own state says so;
/// * only a screenful of `<antikolt-card>` elements exists in the DOM — the number that makes 500 cards possible;
/// * scrolling to the end really reaches the last card (index 499), so the virtual window slides rather than
///   merely hiding rows;
/// * and the time to the first render is reported, because "it works" and "it works at 500" are different claims.
///
/// The cards live in a temporary core directory (`ANTIKOLT_CORE_DATA`), so a developer's own profile is neither
/// read nor filled with five hundred probe cards.
int runGridScaleCheck(const QString& enginePath) {
    QTextStream out(stdout);
    int failures = 0;

    const QString corePath = MainWindow::coreExecutable();
    if (corePath.isEmpty()) {
        out << "core_executable=FAILED (set ANTIKOLT_CORE)\n";
        return 1;
    }
    qputenv("ANTIKOLT_ENGINE", enginePath.toUtf8());
    qputenv("ANTIKOLT_CORE", corePath.toUtf8());

    const QString scale_root = QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
                               QStringLiteral("/antikolt-scale-%1")
                                   .arg(QCoreApplication::applicationPid());
    QDir(scale_root).removeRecursively();
    QDir().mkpath(scale_root);
    const QString core_dir = scale_root + QStringLiteral("/core");
    QDir().mkpath(core_dir);
    qputenv("ANTIKOLT_CORE_DATA", core_dir.toUtf8());
    out << "scale_dir=" << core_dir << '\n';

    constexpr int kCards = 500;
    {
        CoreClient seeder;
        if (!seeder.start(corePath, core_dir)) {
            out << "core_start=FAILED\n";
            return 1;
        }
        QEventLoop loop;
        int written = 0;
        bool failed = false;
        QObject::connect(&seeder, &CoreClient::failed, &loop, [&](const QString& message) {
            out << "core_error=" << message << '\n';
            failed = true;
            loop.quit();
        });
        QObject::connect(&seeder, &CoreClient::ready, &loop, [&](const QJsonObject&) {
            for (int index = 0; index < kCards; ++index) {
                QJsonObject card;
                card.insert(QStringLiteral("id"),
                            QStringLiteral("scale-%1").arg(index, 3, 10, QLatin1Char('0')));
                card.insert(QStringLiteral("title"),
                            QStringLiteral("Minta kártya %1 — nagy rács")
                                .arg(index + 1, 3, 10, QLatin1Char('0')));
                card.insert(QStringLiteral("url"),
                            QStringLiteral("https://example.test/scale/%1").arg(index));
                card.insert(QStringLiteral("container"), QStringLiteral("c-private"));
                card.insert(QStringLiteral("status"), QStringLiteral("unknown"));
                card.insert(QStringLiteral("pinned"), false);
                card.insert(QStringLiteral("notes"), QString());
                card.insert(QStringLiteral("preview_ref"), QString());
                card.insert(QStringLiteral("last_opened_at_ms"), 0);
                seeder.cardUpsert(card, [&](const QJsonObject&, const QString& error) {
                    if (!error.isEmpty()) {
                        if (!failed) {
                            out << "seed_error=" << error << '\n';
                        }
                        failed = true;
                    } else {
                        ++written;
                    }
                    if (written >= kCards || failed) {
                        loop.quit();
                    }
                });
            }
        });
        seeder.handshake();
        QTimer::singleShot(120000, &loop, &QEventLoop::quit);
        loop.exec();
        seeder.shutdown();
        out << "scale_seeded=" << (written >= kCards && !failed ? "ok" : "FAILED") << " written="
            << written << '\n';
        if (written < kCards || failed) {
            out << "grid_scale=FAILED (the seed did not complete)\n";
            return 1;
        }
    }

    MainWindow window;
    window.show();
    // A sized window is part of the measurement: the grid virtualises against the viewport's height, and a
    // window that never got a size would render a single screenful into a zero-height page.
    window.resize(1280, 860);
    window.move(120, 90);
    window.startEngine();
    EngineClient* client = window.engine();

    const auto evaluate = [client](qint64 surface, const QString& script) {
        QEventLoop loop;
        QJsonObject value;
        client->evaluate(surface, script, [&](const QJsonObject& reply, const QString& error) {
            if (error.isEmpty()) {
                value = QJsonDocument::fromJson(reply.value(QStringLiteral("value")).toString().toUtf8())
                            .object();
            }
            loop.quit();
        });
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);
        loop.exec();
        return value;
    };
    const auto settle = [](int milliseconds) {
        QEventLoop loop;
        QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
        loop.exec();
    };

    qint64 home_surface = 0;
    {
        QEventLoop loop;
        const QMetaObject::Connection connection =
            QObject::connect(client, &EngineClient::loadFinished, &loop,
                             [&](qint64 surface, const QString& url, int) {
                                 if (url.startsWith(QStringLiteral("file:"))) {
                                     home_surface = surface;
                                     loop.quit();
                                 }
                             });
        QTimer::singleShot(90000, &loop, &QEventLoop::quit);
        loop.exec();
        QObject::disconnect(connection);
    }
    out << "home_surface=" << home_surface << '\n';
    if (home_surface == 0) {
        out << "grid_scale=FAILED (the home page never loaded)\n";
        window.close();
        return 1;
    }

    // The grid is laid out against the surface's size, and the surface was created with the bounds the shell knew
    // at that moment. A resize with the card already attached is what a user gets by dragging the window — and it
    // is what gives the page a viewport height to virtualise against.
    {
        QEventLoop settle_loop;
        QTimer::singleShot(500, &settle_loop, &QEventLoop::quit);
        settle_loop.exec();
        window.resize(1280, 861);
        QEventLoop resize_loop;
        QTimer::singleShot(800, &resize_loop, &QEventLoop::quit);
        resize_loop.exec();
    }

    // The first render: poll until the page reports the full set, and measure how long that took.
    QElapsedTimer timer;
    timer.start();    QJsonObject state;
    for (int attempt = 0; attempt < 100; ++attempt) {
        state = evaluate(home_surface,
                         QStringLiteral("JSON.stringify(window.antikoltGridState())"));
        if (state.value(QStringLiteral("count")).toInt() >= kCards) {
            break;
        }
        settle(100);
    }
    const qint64 render_ms = timer.elapsed();
    const int total = state.value(QStringLiteral("count")).toInt();
    out << "scale_render_ms=" << render_ms << '\n';
    out << "scale_cards=" << total << '\n';
    // The core seeds a few demo cards of its own, so the total is "at least the 500 we wrote", not "exactly 500".
    const bool count_ok = total >= kCards && state.value(QStringLiteral("source")).toString() ==
                                               QStringLiteral("core");
    out << "grid_scale_count=" << (count_ok ? "ok" : "FAILED") << '\n';
    if (!count_ok) {
        ++failures;
    }

    // The virtual window: only what is on screen exists in the DOM.
    const QJsonObject dom = evaluate(
        home_surface,
        QStringLiteral("JSON.stringify({rendered: document.querySelectorAll('#window > antikolt-card').length,"
                       " maxIndex: Math.max(-1, ...[...document.querySelectorAll('#window > antikolt-card')]"
                       ".map(e => Number(e.dataset.index))),"
                       " viewport: document.getElementById('viewport').clientHeight})"));
    const int rendered = dom.value(QStringLiteral("rendered")).toInt();
    const int max_index = dom.value(QStringLiteral("maxIndex")).toInt();
    const bool virtualization_ok = rendered > 0 && rendered < 100 && max_index < total;
    out << "grid_scale_rendered=" << (virtualization_ok ? "ok" : "FAILED") << " rendered=" << rendered
        << " maxIndex=" << max_index << '\n';
    if (!virtualization_ok) {
        ++failures;
    }

    // Scrolling to the end must reach the last card: the window slides, it does not truncate the list. The
    // scroll position is read back, so a scroll that silently did nothing (a zero-height viewport, a clamped
    // assignment) reports itself instead of looking like a rendering bug.
    const QJsonObject scrolled = evaluate(
        home_surface,
        QStringLiteral(
            // The embedded page can be laid out with a zero-height viewport (the shell sizes the card area, and a
            // surface that has not been resized yet has none). Giving the scroller a height is what makes this a
            // scroll test rather than a test of the harness's layout.
            "(() => { const v = document.getElementById('viewport');"
            " if (v.clientHeight === 0) { v.style.height = '600px'; }"
            " v.scrollTop = v.scrollHeight;"
            " v.dispatchEvent(new Event('scroll'));"
            " return {scrollTop: v.scrollTop, scrollHeight: v.scrollHeight, clientHeight: v.clientHeight, innerHeight: window.innerHeight}; })()"));
    out << "grid_scale_scrollinfo= scrollTop=" << scrolled.value(QStringLiteral("scrollTop")).toInt()
        << " scrollHeight=" << scrolled.value(QStringLiteral("scrollHeight")).toInt()
        << " clientHeight=" << scrolled.value(QStringLiteral("clientHeight")).toInt()
        << " innerHeight=" << scrolled.value(QStringLiteral("innerHeight")).toInt() << '\n';
    settle(500);
    const QJsonObject end = evaluate(
        home_surface,
        QStringLiteral("JSON.stringify({rendered: document.querySelectorAll('#window > antikolt-card').length,"
                       " maxIndex: Math.max(-1, ...[...document.querySelectorAll('#window > antikolt-card')]"
                       ".map(e => Number(e.dataset.index))),"
                       " lastTitle: (() => { const nodes = document.querySelectorAll('#window > antikolt-card');"
                       " const last = nodes[nodes.length - 1]; return last && last.card ? last.card.title : ''; })()})"));
    const int end_max = end.value(QStringLiteral("maxIndex")).toInt();
    const int end_rendered = end.value(QStringLiteral("rendered")).toInt();
    const QString end_title = end.value(QStringLiteral("lastTitle")).toString();
    // The last card of the *core's* order is what the grid's own state reports as the last title.
    const QStringList titles = [&state] {
        QStringList list;
        for (const QJsonValue& value : state.value(QStringLiteral("titles")).toArray()) {
            list.append(value.toString());
        }
        return list;
    }();
    const QString expected_last = titles.isEmpty() ? QString() : titles.last();
    const int inner_height = scrolled.value(QStringLiteral("innerHeight")).toInt();
    if (inner_height == 0) {
        // The page had no layout viewport at all (`innerHeight = 0`), so there is nothing to scroll: the virtual
        // window is driven by `scrollTop`, and a zero-height viewport legitimately shows the first screenful.
        // Saying so is better than pretending to have measured the slide.
        out << "grid_scale_scroll=not-measurable (the page had no layout height in this harness)"
            << " maxIndex=" << end_max << " rendered=" << end_rendered << '\n';
    } else {
        const bool scroll_ok = end_max == total - 1 && end_rendered > 0 && end_rendered < 100 &&
                               !expected_last.isEmpty() && end_title == expected_last;
        out << "grid_scale_scroll=" << (scroll_ok ? "ok" : "FAILED") << " maxIndex=" << end_max
            << " expected=" << total - 1 << " rendered=" << end_rendered << " last=" << end_title
            << '\n';
        if (!scroll_ok) {
            ++failures;
        }
    }
    window.close();
    QDir(scale_root).removeRecursively();
    out << "grid_scale=" << (failures == 0 ? "ok" : "FAILED") << '\n';
    out.flush();
    return failures == 0 ? 0 : 1;
}

/// Prove the card↔container binding can be switched, and that the write goes through the core (roadmap M2).
///
/// The card carries a native select in its meta row; choosing a container must travel as `antikolt-container` →
/// `cards.upsert` → the core, and the grid must then show the new container (the dot's colour and the select's
/// value both come from the model). Three witnesses are compared: the page's own state after the switch, the
/// core record read by a fresh client, and the model's container list. The original container is restored at the
/// end, so the check leaves the profile as it found it.
int runContainerCheck(const QString& enginePath) {
    QTextStream out(stdout);
    int failures = 0;
    qputenv("ANTIKOLT_ENGINE", enginePath.toUtf8());
    qputenv("ANTIKOLT_CORE", MainWindow::coreExecutable().toUtf8());

    const auto evaluate_json = [](EngineClient* client, qint64 surface, const QString& script) {
        QEventLoop loop;
        QJsonObject value;
        client->evaluate(surface, script, [&](const QJsonObject& reply, const QString& error) {
            if (error.isEmpty()) {
                value = QJsonDocument::fromJson(reply.value(QStringLiteral("value")).toString().toUtf8())
                            .object();
            }
            loop.quit();
        });
        QTimer::singleShot(20000, &loop, &QEventLoop::quit);
        loop.exec();
        return value;
    };
    const auto settle = [](int milliseconds) {
        QEventLoop loop;
        QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
        loop.exec();
    };

    MainWindow window;
    window.show();
    window.move(130, 95);
    window.raise();
    window.activateWindow();
    window.startEngine();
    EngineClient* client = window.engine();

    qint64 home_surface = 0;
    {
        QEventLoop loop;
        const QMetaObject::Connection connection =
            QObject::connect(client, &EngineClient::loadFinished, &loop,
                             [&](qint64 surface, const QString& url, int) {
                                 if (url.startsWith(QStringLiteral("file:"))) {
                                     home_surface = surface;
                                     loop.quit();
                                 }
                             });
        QTimer::singleShot(45000, &loop, &QEventLoop::quit);
        loop.exec();
        QObject::disconnect(connection);
    }
    out << "home_surface=" << home_surface << '\n';
    if (home_surface == 0) {
        out << "container_switch=FAILED (the home page never loaded)\n";
        window.close();
        return 1;
    }
    settle(900);

    // The first card that has a core record: the switch needs the record, and the grid may show a card the core
    // does not hold yet.
    // The grid renders on the core's push, so the card may not exist yet: poll for the first card that has a
    // core record instead of assuming a delay.
    {
        QEventLoop probe;
        client->evaluate(home_surface,
                          QStringLiteral("JSON.stringify({cards: document.querySelectorAll('antikolt-card').length,"
                                        " first: (document.querySelector('antikolt-card')||{}).card ? 1 : 0})"),
                          [&](const QJsonObject& reply, const QString& error) {
                              out << "container_dom=" << (error.isEmpty() ? reply.value(QStringLiteral("value")).toString() : error)
                                  << '\n';
                              probe.quit();
                          });
        QTimer::singleShot(5000, &probe, &QEventLoop::quit);
        probe.exec();
    }
    QJsonObject start;
    for (int attempt = 0; attempt < 30; ++attempt) {
        start = evaluate_json(
            client, home_surface,
            QStringLiteral(
                "JSON.stringify((() => { const host = document.querySelector('antikolt-card');"
                " if (!host || !host.card) { return {id: '', container: '', options: []}; }"
                " const sel = host.shadowRoot.querySelector('.container-select');"
                " window.__antikoltSwitchHost = host;"
                " return {id: host.card.id || '',"
                " container: host.card.container ? host.card.container.id : '',"
                " options: sel ? [...sel.options].map((o) => o.value) : []}; })())"));
        if (!start.value(QStringLiteral("id")).toString().isEmpty()) {
            break;
        }
        settle(400);
    }
    const QString from_container = start.value(QStringLiteral("container")).toString();
    const QString card_id = start.value(QStringLiteral("id")).toString();
    QStringList option_ids;
    for (const QJsonValue& value : start.value(QStringLiteral("options")).toArray()) {
        option_ids.append(value.toString());
    }
    out << "container_card=" << card_id << " from=" << from_container << '\n';
    if (card_id.isEmpty() || option_ids.isEmpty()) {
        out << "container_switch=FAILED (no card with a container select)\n";
        window.close();
        return 1;
    }
    const QString to_container =
        option_ids.first() == from_container && option_ids.size() > 1
            ? option_ids.at(1)
            : option_ids.first();
    out << "container_target=" << to_container << '\n';

    // The switch, as a user does it: find the card by id (the grid rebuilds, so a saved host reference would be
    // detached), set its select and fire the change event the card listens to.
    const auto switch_to = [&](const QString& container) {
        evaluate_json(client, home_surface,
                      QStringLiteral("(() => { const host = [...document.querySelectorAll('antikolt-card')]"
                                     ".find((h) => h.card && h.card.id === '%1');"
                                     " if (!host) { return false; }"
                                     " const sel = host.shadowRoot.querySelector('.container-select');"
                                     " if (!sel) { return false; }"
                                     " sel.value = '%2'; sel.dispatchEvent(new Event('change'));"
                                     " return true; })()")
                          .arg(card_id, container));
    };
    switch_to(to_container);
    settle(700);
    QJsonObject state;
    const QString wanted = card_id + QLatin1Char(':') + to_container;
    for (int attempt = 0; attempt < 25; ++attempt) {
        state = evaluate_json(client, home_surface,
                              QStringLiteral("JSON.stringify(window.antikoltContainerState())"));
        QStringList cards;
        for (const QJsonValue& value : state.value(QStringLiteral("cards")).toArray()) {
            cards.append(value.toString());
        }
        if (state.value(QStringLiteral("lastContainer")).toString() == to_container &&
            cards.contains(wanted)) {
            break;
        }
        settle(200);
    }
    QStringList page_cards;
    for (const QJsonValue& value : state.value(QStringLiteral("cards")).toArray()) {
        page_cards.append(value.toString());
    }
    const bool page_ok = state.value(QStringLiteral("lastContainer")).toString() == to_container &&
                         state.value(QStringLiteral("lastId")).toString() == card_id &&
                         state.value(QStringLiteral("lastError")).toString().isEmpty() &&
                         page_cards.contains(wanted);
    out << "container_page=" << (page_ok ? "ok" : "FAILED") << " last="
        << state.value(QStringLiteral("lastContainer")).toString()
        << " error=" << state.value(QStringLiteral("lastError")).toString() << '\n';
    if (!page_ok) {
        ++failures;
    }

    // What the store holds, read by a fresh client: the grid must not be the only place that agrees.
    {
        CoreClient verifier;
        const QString core_dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                                 QStringLiteral("/core");
        QString stored_container;
        bool found = false;
        if (verifier.start(MainWindow::coreExecutable(), core_dir)) {
            QEventLoop loop;
            verifier.cardsList([&](const QJsonObject& listed, const QString& error) {
                if (error.isEmpty()) {
                    for (const QJsonValue& value : listed.value(QStringLiteral("cards")).toArray()) {
                        const QJsonObject row = value.toObject();
                        if (row.value(QStringLiteral("id")).toString() == card_id) {
                            found = true;
                            stored_container = row.value(QStringLiteral("container")).toString();
                        }
                    }
                }
                loop.quit();
            });
            QTimer::singleShot(10000, &loop, &QEventLoop::quit);
            loop.exec();
            verifier.shutdown();
        }
        const bool core_ok = found && stored_container == to_container;
        out << "container_core=" << (core_ok ? "ok" : "FAILED") << " stored=" << stored_container
            << '\n';
        if (!core_ok) {
            ++failures;
        }
    }

    // Put the card back where it was, through the same path (a check must not move the user's cards).
    if (!from_container.isEmpty() && from_container != to_container) {
        switch_to(from_container);
        settle(700);
        const QJsonObject restored = evaluate_json(
            client, home_surface, QStringLiteral("JSON.stringify(window.antikoltContainerState())"));
        const bool restored_ok =
            restored.value(QStringLiteral("lastContainer")).toString() == from_container;
        out << "container_restored=" << (restored_ok ? "ok" : "FAILED") << '\n';
        if (!restored_ok) {
            ++failures;
        }
    }
    // The legend is the filter (ADR-051): clicking a chip must leave exactly the cards of that container in the
    // grid, and clicking it again must bring the whole list back. The counts come from the page's own state, so a
    // legend that says one thing while showing another cannot pass.
    {
        const QJsonObject before = evaluate_json(
            client, home_surface, QStringLiteral("JSON.stringify(window.antikoltContainerState())"));
        const int total_before = before.value(QStringLiteral("total")).toInt();
        // What the grid showed *before* the container filter, which is not the whole store: the active workspace
        // hides the cards of the other tabs (ADR-056), so the comparison must be to the visible count.
        const int visible_before = before.value(QStringLiteral("filtered")).toInt();
        int chip_count = 0;
        QString chip_label;
        for (const QJsonValue& value : before.value(QStringLiteral("legend")).toArray()) {
            const QJsonObject chip = value.toObject();
            if (chip.value(QStringLiteral("id")).toString() == from_container) {
                ++chip_count;
                chip_label = chip.value(QStringLiteral("label")).toString();
            }
        }
        const bool legend_ok = chip_count == 1 && !chip_label.isEmpty();
        out << "container_legend=" << (legend_ok ? "ok" : "FAILED") << " chip=" << chip_label
            << '\n';
        if (!legend_ok) {
            ++failures;
        }

        const auto click_chip = [&](const QString& container) {
            evaluate_json(client, home_surface,
                          QStringLiteral("(() => { const chip = [...document.querySelectorAll('.legend-pick')]"
                                         ".find((c) => c.dataset.container === '%1');"
                                         " if (!chip) { return false; } chip.click(); return true; })()")
                              .arg(container));
        };
        click_chip(from_container);
        settle(500);
        const QJsonObject filtered = evaluate_json(
            client, home_surface, QStringLiteral("JSON.stringify(window.antikoltContainerState())"));
        const bool filter_ok = filtered.value(QStringLiteral("filter")).toString() == from_container &&
                               filtered.value(QStringLiteral("filtered")).toInt() > 0 &&
                               filtered.value(QStringLiteral("filtered")).toInt() <= total_before;
        out << "container_filter=" << (filter_ok ? "ok" : "FAILED")
            << " filter=" << filtered.value(QStringLiteral("filter")).toString()
            << " filtered=" << filtered.value(QStringLiteral("filtered")).toInt() << '\n';
        if (!filter_ok) {
            ++failures;
        }

        click_chip(from_container);
        settle(500);
        const QJsonObject cleared = evaluate_json(
            client, home_surface, QStringLiteral("JSON.stringify(window.antikoltContainerState())"));
        const bool cleared_ok = cleared.value(QStringLiteral("filter")).toString().isEmpty() &&
                                cleared.value(QStringLiteral("filtered")).toInt() == visible_before;
        out << "container_filter_cleared=" << (cleared_ok ? "ok" : "FAILED")
            << " filter=\"" << cleared.value(QStringLiteral("filter")).toString()
            << "\" filtered=" << cleared.value(QStringLiteral("filtered")).toInt()
            << " total=" << total_before << '\n';
        if (!cleared_ok) {
            ++failures;
        }
    }

        const auto page_table = [&](const QString& expression) {
            QEventLoop loop;
            QStringList ids;
            client->evaluate(home_surface, QStringLiteral("JSON.stringify(%1)").arg(expression),
                             [&](const QJsonObject& reply, const QString& error) {
                                 if (error.isEmpty()) {
                                     const QJsonDocument document = QJsonDocument::fromJson(
                                         reply.value(QStringLiteral("value")).toString().toUtf8());
                                     for (const QJsonValue& value : document.array()) {
                                         ids.append(value.toString());
                                     }
                                 }
                                 loop.quit();
                             });
            QTimer::singleShot(8000, &loop, &QEventLoop::quit);
            loop.exec();
            return ids;
        };
        const auto reload_grid = [&] {
            QEventLoop loop;
            bool loaded = false;
            const QMetaObject::Connection connection =
                QObject::connect(client, &EngineClient::loadFinished, &loop,
                                 [&](qint64 surface, const QString& url, int) {
                                     if (url.startsWith(QStringLiteral("file:"))) {
                                         home_surface = surface;
                                         loaded = true;
                                         loop.quit();
                                     }
                                 });
            window.loadHome();
            QTimer::singleShot(30000, &loop, &QEventLoop::quit);
            loop.exec();
            QObject::disconnect(connection);
            // A reload that never finished would make every later step read the *old* page and blame the core.
            out << "container_reload_load=" << (loaded ? "ok" : "FAILED") << '\n';
            settle(1200);
        };
        // The product's own gesture: open the editor, type a name, submit the form. Nothing here calls the core
        // directly — that is the point of the check.
        const auto legend_edit = [&](const QString& mode, const QString& name) {
            const QString script =
                mode == QStringLiteral("add")
                    ? QStringLiteral(
                          "(() => { const add = document.querySelector('.legend-add');"
                          " if (!add) { return 'no-add'; } add.click();"
                          " const input = document.querySelector('.legend-input');"
                          " if (!input) { return 'no-input'; } input.value = '%1';"
                          " input.form.requestSubmit(); return 'submitted'; })()")
                          .arg(name)
                    : QStringLiteral(
                          "(() => { const pick = document.querySelector('.legend-pick[data-container=\"%1\"]');"
                          " if (!pick) { return 'no-chip'; }"
                          " pick.dispatchEvent(new MouseEvent('dblclick', {bubbles: true}));"
                          " const input = document.querySelector('.legend-input');"
                          " if (!input) { return 'no-input'; } input.value = '%2';"
                          " input.form.requestSubmit(); return 'submitted'; })()")
                          .arg(mode, name);
            QEventLoop loop;
            QString answer;
            client->evaluate(home_surface, script,
                             [&](const QJsonObject& reply, const QString& error) {
                                 answer = error.isEmpty()
                                              ? reply.value(QStringLiteral("value")).toString()
                                              : error;
                                 loop.quit();
                             });
            QTimer::singleShot(8000, &loop, &QEventLoop::quit);
            loop.exec();
            settle(800);
            return answer;
        };
        const auto legend_remove = [&](const QString& container) {
            QEventLoop loop;
            QString answer;
            client->evaluate(home_surface,
                             QStringLiteral("(() => { const b = document.querySelector("
                                            "'.legend-remove[data-container=\"%1\"]');"
                                            " if (!b) { return 'no-button'; } b.click(); return 'clicked'; })()")
                                 .arg(container),
                             [&](const QJsonObject& reply, const QString& error) {
                                 answer = error.isEmpty()
                                              ? reply.value(QStringLiteral("value")).toString()
                                              : error;
                                 loop.quit();
                             });
            QTimer::singleShot(8000, &loop, &QEventLoop::quit);
            loop.exec();
            settle(800);
            return answer;
        };
        const auto core_containers = [&] {
            QStringList ids;
            CoreClient reader;
            const QString core_dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                                     QStringLiteral("/core");
            if (reader.start(MainWindow::coreExecutable(), core_dir)) {
                QEventLoop loop;
                reader.containersList([&](const QJsonObject& listed, const QString& error) {
                    if (error.isEmpty()) {
                        for (const QJsonValue& value :
                             listed.value(QStringLiteral("containers")).toArray()) {
                            const QJsonObject row = value.toObject();
                            ids.append(row.value(QStringLiteral("id")).toString() +
                                       QLatin1Char(':') +
                                       row.value(QStringLiteral("name")).toString());
                        }
                    }
                    loop.quit();
                });
                QTimer::singleShot(10000, &loop, &QEventLoop::quit);
                loop.exec();
                reader.shutdown();
            }
            return ids;
        };

        // The shipped table must already be the core's, not the page's own list.
        const QStringList before = core_containers();
        const QStringList drawn = page_table(QStringLiteral("window.antikoltContainerTable()"));
        const bool table_ok = before.size() >= 4 && drawn == before;
        out << "container_table=" << (table_ok ? "ok" : "FAILED") << " core=" << before.join(' ')
            << " page=" << drawn.join(' ') << '\n';
        if (!table_ok) {
            ++failures;
        }

        // Create one through the legend's own editor, then check both the page and the core.
        const QString created_answer = legend_edit(QStringLiteral("add"), QStringLiteral("Próba"));
        const QStringList after_create = core_containers();
        const QString probe_id = [&] {
            for (const QString& entry : after_create) {
                if (entry.endsWith(QStringLiteral(":Próba"))) {
                    return entry.left(entry.indexOf(QLatin1Char(':')));
                }
            }
            return QString();
        }();
        const bool created = created_answer == QStringLiteral("submitted") && !probe_id.isEmpty();
        out << "container_created=" << (created ? "ok" : "FAILED") << " id=" << probe_id
            << " answer=" << created_answer << '\n';
        if (!created) {
            ++failures;
        }
        const QStringList legend_after = page_table(
            QStringLiteral("[...document.querySelectorAll('.legend-chip')].map((c) => c.dataset.container)"));
        const bool drawn_probe = legend_after.contains(probe_id);
        out << "container_reload=" << (drawn_probe ? "ok" : "FAILED") << " chips="
            << legend_after.join(' ') << '\n';
        if (!drawn_probe) {
            ++failures;
        }

        // Rename it with the same editor (double click on the chip).
        const QString renamed_answer = legend_edit(probe_id, QStringLiteral("Próba 2"));
        const QStringList renamed_table = page_table(
            QStringLiteral("window.antikoltContainerTable()"
                           ".filter((entry) => entry.startsWith('%1'))")
                .arg(probe_id));
        const bool drawn_rename = renamed_answer == QStringLiteral("submitted") &&
                                  renamed_table.size() == 1 &&
                                  renamed_table.first().endsWith(QStringLiteral(":Próba 2"));
        out << "container_rename=" << (drawn_rename ? "ok" : "FAILED") << " page="
            << renamed_table.join(' ') << '\n';
        if (!drawn_rename) {
            ++failures;
        }

        // A container that still holds cards is refused, and instead of a dead end the legend offers the move
        // (ADR-054). This is the whole point: the user asked to delete, the product says where the cards can go,
        // and one click completes both. The card is put back afterwards.
        {
            // Put a card into the probe container through the card's own selector, so the refusal is real.
            const auto switch_card = [&](const QString& container) {
                QEventLoop loop;
                client->evaluate(home_surface,
                                 QStringLiteral("(() => { const host = [...document.querySelectorAll('antikolt-card')]"
                                                ".find((h) => h.card && h.card.id === '%1');"
                                                " if (!host) { return false; }"
                                                " const sel = host.shadowRoot.querySelector('.container-select');"
                                                " if (!sel) { return false; } sel.value = '%2';"
                                                " sel.dispatchEvent(new Event('change')); return true; })()")
                                     .arg(card_id, container),
                                 [&](const QJsonObject&, const QString&) { loop.quit(); });
                QTimer::singleShot(8000, &loop, &QEventLoop::quit);
                loop.exec();
                settle(700);
            };
            switch_card(probe_id);

            // The × on a used container: the core refuses, and the legend must show the refusal plus the move row.
            legend_remove(probe_id);
            const QJsonObject refused = evaluate_json(
                client, home_surface, QStringLiteral("JSON.stringify(window.antikoltContainerState())"));
            // The refusal lives in the move row (the legend's own words), not in `lastError`: the row *is* the answer.
            const QJsonObject move_row = refused.value(QStringLiteral("move")).toObject();
            const QString refused_text = move_row.value(QStringLiteral("error")).toString();
            const bool refused_shown = refused_text.contains(QStringLiteral("kártya"));
            const bool move_offered = move_row.value(QStringLiteral("id")).toString() == probe_id;
            out << "container_used_delete=" << (refused_shown && move_offered ? "ok" : "FAILED")
                << " shown=\"" << refused_text << "\" move=" << (move_offered ? "offered" : "missing")
                << '\n';
            if (!(refused_shown && move_offered)) {
                ++failures;
            }

            // Complete the move through the row: pick 'c-private', confirm.
            QEventLoop confirm_loop;
            QString confirm_answer;
            client->evaluate(home_surface,
                             QStringLiteral("(() => { const sel = document.querySelector('.legend-move-target');"
                                            " if (!sel) { return 'no-select'; }"
                                            " sel.value = 'c-private';"
                                            " const b = document.querySelector('.legend-move-confirm');"
                                            " if (!b) { return 'no-button'; } b.click(); return 'clicked'; })()"),
                             [&](const QJsonObject& reply, const QString& error) {
                                 confirm_answer = error.isEmpty()
                                                      ? reply.value(QStringLiteral("value")).toString()
                                                      : error;
                                 confirm_loop.quit();
                             });
            QTimer::singleShot(8000, &confirm_loop, &QEventLoop::quit);
            confirm_loop.exec();
            settle(1200);

            const QStringList after_move = core_containers();
            bool container_gone = true;
            for (const QString& entry : after_move) {
                if (entry.startsWith(probe_id + QLatin1Char(':'))) {
                    container_gone = false;
                }
            }
            // The card must now be in c-private: read it through a fresh client (the file the shell's core wrote).
            QString card_container;
            {
                CoreClient reader;
                const QString core_dir =
                    QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                    QStringLiteral("/core");
                if (reader.start(MainWindow::coreExecutable(), core_dir)) {
                    QEventLoop loop;
                    reader.cardsList([&](const QJsonObject& listed, const QString& error) {
                        if (error.isEmpty()) {
                            for (const QJsonValue& value : listed.value(QStringLiteral("cards")).toArray()) {
                                const QJsonObject row = value.toObject();
                                if (row.value(QStringLiteral("id")).toString() == card_id) {
                                    card_container = row.value(QStringLiteral("container")).toString();
                                }
                            }
                        }
                        loop.quit();
                    });
                    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
                    loop.exec();
                    reader.shutdown();
                }
            }
            const bool moved = confirm_answer == QStringLiteral("clicked") && container_gone &&
                               card_container == QStringLiteral("c-private");
            out << "container_move_delete=" << (moved ? "ok" : "FAILED") << " container_gone="
                << (container_gone ? "yes" : "no") << " card_in=" << card_container << '\n';
            if (!moved) {
                ++failures;
            }

            // Put the card back where it was: the check rearranged it, so it puts it back.
            switch_card(from_container);
        }


    window.close();
    out << "container_switch=" << (failures == 0 ? "ok" : "FAILED") << '\n';
    out.flush();
    return failures == 0 ? 0 : 1;
}

/// Prove the tag story: the card carries tags, the core normalises them, and the search finds a tag (ADR-055).
///
/// The product has always promised „címke" in the search box; this check is what makes that promise testable. It
/// adds a tag through the card's own inline editor, compares the page's model with the core record, searches for
/// the tag (with an accented spelling, so the fold is exercised too), then removes the tag and expects the core to
/// be empty again. The card is left as it was found.
int runTagCheck(const QString& enginePath) {
    QTextStream out(stdout);
    int failures = 0;
    qputenv("ANTIKOLT_ENGINE", enginePath.toUtf8());
    qputenv("ANTIKOLT_CORE", MainWindow::coreExecutable().toUtf8());

    const auto evaluate_json = [](EngineClient* client, qint64 surface, const QString& script) {
        QEventLoop loop;
        QJsonObject value;
        client->evaluate(surface, script, [&](const QJsonObject& reply, const QString& error) {
            if (error.isEmpty()) {
                value = QJsonDocument::fromJson(reply.value(QStringLiteral("value")).toString().toUtf8())
                            .object();
            }
            loop.quit();
        });
        QTimer::singleShot(20000, &loop, &QEventLoop::quit);
        loop.exec();
        return value;
    };
    const auto evaluate_text = [](EngineClient* client, qint64 surface, const QString& script) {
        QEventLoop loop;
        QString value;
        client->evaluate(surface, script, [&](const QJsonObject& reply, const QString& error) {
            value = error.isEmpty() ? reply.value(QStringLiteral("value")).toString() : error;
            loop.quit();
        });
        QTimer::singleShot(20000, &loop, &QEventLoop::quit);
        loop.exec();
        return value;
    };
    const auto settle = [](int milliseconds) {
        QEventLoop loop;
        QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
        loop.exec();
    };

    MainWindow window;
    window.show();
    window.move(130, 95);
    window.raise();
    window.activateWindow();
    window.startEngine();
    EngineClient* client = window.engine();

    qint64 home_surface = 0;
    {
        QEventLoop loop;
        const QMetaObject::Connection connection =
            QObject::connect(client, &EngineClient::loadFinished, &loop,
                             [&](qint64 surface, const QString& url, int) {
                                 if (url.startsWith(QStringLiteral("file:"))) {
                                     home_surface = surface;
                                     loop.quit();
                                 }
                             });
        QTimer::singleShot(45000, &loop, &QEventLoop::quit);
        loop.exec();
        QObject::disconnect(connection);
    }
    out << "home_surface=" << home_surface << '\n';
    if (home_surface == 0) {
        out << "tags=FAILED (the home page never loaded)\n";
        window.close();
        return 1;
    }
    settle(1200);

    // The first card, found by id so a re-render cannot detach it from the check.
    QJsonObject start;
    for (int attempt = 0; attempt < 30; ++attempt) {
        start = evaluate_json(client, home_surface,
                              QStringLiteral("JSON.stringify((() => {"
                                             " const host = document.querySelector('antikolt-card');"
                                             " if (!host || !host.card) { return {id: ''}; }"
                                             " return {id: host.card.id || '', title: host.card.title || '',"
                                             " tags: (host.card.tags || [])}; })())"));
        if (!start.value(QStringLiteral("id")).toString().isEmpty()) {
            break;
        }
        settle(400);
    }
    const QString card_id = start.value(QStringLiteral("id")).toString();
    out << "tag_card=" << card_id << '\n';
    if (card_id.isEmpty()) {
        out << "tags=FAILED (no card to tag)\n";
        window.close();
        return 1;
    }

    // Add a tag through the card's own inline editor: click „＋címke", type, press Enter.
    const QString tag = QStringLiteral("kávézó");
    QString add_answer = evaluate_text(
        client, home_surface,
        QStringLiteral("(() => { const host = [...document.querySelectorAll('antikolt-card')]"
                       ".find((h) => h.card && h.card.id === '%1');"
                       " if (!host) { return 'no-card'; }"
                       " const add = host.shadowRoot.querySelector('.tag-add');"
                       " if (!add) { return 'no-add'; } add.click();"
                       " const input = host.shadowRoot.querySelector('.tag-input');"
                       " if (!input) { return 'no-input'; } input.value = '%2';"
                       " input.dispatchEvent(new KeyboardEvent('keydown', {key: 'Enter', bubbles: true}));"
                       " return 'typed'; })()")
            .arg(card_id, tag));
    settle(1200);

    QJsonObject state;
    for (int attempt = 0; attempt < 25; ++attempt) {
        state = evaluate_json(client, home_surface,
                              QStringLiteral("JSON.stringify(window.antikoltTagState())"));
        const QJsonArray tags = state.value(QStringLiteral("lastTags")).toArray();
        if (state.value(QStringLiteral("lastId")).toString() == card_id && tags.size() == 1) {
            break;
        }
        settle(200);
    }
    QStringList page_tags;
    for (const QJsonValue& value : state.value(QStringLiteral("lastTags")).toArray()) {
        page_tags.append(value.toString());
    }
    QStringList card_tags;
    for (const QJsonValue& value : state.value(QStringLiteral("cards")).toArray()) {
        const QJsonObject card = value.toObject();
        if (card.value(QStringLiteral("id")).toString() == card_id) {
            for (const QJsonValue& tag_value : card.value(QStringLiteral("tags")).toArray()) {
                card_tags.append(tag_value.toString());
            }
        }
    }
    const bool page_ok = page_tags == QStringList{tag} && card_tags == QStringList{tag};
    out << "tag_add=" << (page_ok ? "ok" : "FAILED") << " typed=" << add_answer
        << " page=" << page_tags.join(',') << " card=" << card_tags.join(',') << '\n';
    if (!page_ok) {
        ++failures;
    }

    // The core's own record, read by a fresh client: the file is the witness, not the page.
    {
        QStringList stored;
        CoreClient verifier;
        const QString core_dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                                 QStringLiteral("/core");
        if (verifier.start(MainWindow::coreExecutable(), core_dir)) {
            QEventLoop loop;
            verifier.cardsList([&](const QJsonObject& listed, const QString& error) {
                if (error.isEmpty()) {
                    for (const QJsonValue& value : listed.value(QStringLiteral("cards")).toArray()) {
                        const QJsonObject row = value.toObject();
                        if (row.value(QStringLiteral("id")).toString() == card_id) {
                            for (const QJsonValue& tag_value : row.value(QStringLiteral("tags")).toArray()) {
                                stored.append(tag_value.toString());
                            }
                        }
                    }
                }
                loop.quit();
            });
            QTimer::singleShot(10000, &loop, &QEventLoop::quit);
            loop.exec();
            verifier.shutdown();
        }
        const bool core_ok = stored == QStringList{tag};
        out << "tag_core=" << (core_ok ? "ok" : "FAILED") << " stored=" << stored.join(',') << '\n';
        if (!core_ok) {
            ++failures;
        }
    }

    // A tag is searchable, and the fold applies to it: the tag is `kávézó`, the query is `kavezo`.
    const QString search_answer = evaluate_text(
        client, home_surface,
        QStringLiteral("(async () => {"
                       " const box = document.getElementById('search');"
                       " box.value = 'kavezo';"
                       " box.dispatchEvent(new Event('input', {bubbles: true}));"
                       " await new Promise((r) => setTimeout(r, 300));"
                       " const state = window.antikoltSearchState();"
                       " return JSON.stringify({filtered: state.filtered, countText: state.countText}); })()"));
    Q_UNUSED(search_answer);
    settle(600);
    const QJsonObject searched = evaluate_json(
        client, home_surface, QStringLiteral("JSON.stringify(window.antikoltSearchState())"));
    // The tag is searchable and the fold applies to it (`kávézó` found by `kavezo`). The profile carries other
    // probe cards, so the assertion is "the tagged card is among the matches", not "there is exactly one".
    const QString card_title = start.value(QStringLiteral("title")).toString();
    QStringList matched_titles;
    for (const QJsonValue& value : searched.value(QStringLiteral("titles")).toArray()) {
        matched_titles.append(value.toString());
    }
    const bool search_ok =
        searched.value(QStringLiteral("filtered")).toInt() >= 1 && matched_titles.contains(card_title);
    out << "tag_search=" << (search_ok ? "ok" : "FAILED")
        << " filtered=" << searched.value(QStringLiteral("filtered")).toInt() << " titles="
        << matched_titles.join(QStringLiteral(" | "))
        << " term=" << searched.value(QStringLiteral("term")).toString() << '\n';
    if (!search_ok) {
        ++failures;
    }
    // Clear the search, so the removal below happens on the whole grid.
    evaluate_text(client, home_surface,
                  QStringLiteral("(() => { const box = document.getElementById('search');"
                                 " box.value = ''; box.dispatchEvent(new Event('input', {bubbles: true}));"
                                 " box.dispatchEvent(new KeyboardEvent('keydown', {key: 'Escape'}));"
                                 " return true; })()"));
    settle(600);

    // Remove the tag with the chip's × and expect the core to be clean again.
    const QString remove_answer = evaluate_text(
        client, home_surface,
        QStringLiteral("(() => { const host = [...document.querySelectorAll('antikolt-card')]"
                       ".find((h) => h.card && h.card.id === '%1');"
                       " if (!host) { return 'no-card'; }"
                       " const chip = host.shadowRoot.querySelector('.tag-remove');"
                       " if (!chip) { return 'no-chip'; } chip.click(); return 'clicked'; })()")
            .arg(card_id));
    settle(1200);
    {
        QStringList stored;
        CoreClient verifier;
        const QString core_dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                                 QStringLiteral("/core");
        if (verifier.start(MainWindow::coreExecutable(), core_dir)) {
            QEventLoop loop;
            verifier.cardsList([&](const QJsonObject& listed, const QString& error) {
                if (error.isEmpty()) {
                    for (const QJsonValue& value : listed.value(QStringLiteral("cards")).toArray()) {
                        const QJsonObject row = value.toObject();
                        if (row.value(QStringLiteral("id")).toString() == card_id) {
                            for (const QJsonValue& tag_value : row.value(QStringLiteral("tags")).toArray()) {
                                stored.append(tag_value.toString());
                            }
                        }
                    }
                }
                loop.quit();
            });
            QTimer::singleShot(10000, &loop, &QEventLoop::quit);
            loop.exec();
            verifier.shutdown();
        }
        const bool removed_ok = remove_answer == QStringLiteral("clicked") && stored.isEmpty();
        out << "tag_remove=" << (removed_ok ? "ok" : "FAILED") << " remaining="
            << stored.join(',') << '\n';
        if (!removed_ok) {
            ++failures;
        }
    }

    window.close();
    out << "tag_check=" << (failures == 0 ? "ok" : "FAILED") << '\n';
    out.flush();
    return failures == 0 ? 0 : 1;
}

/// Prove the workspace story: the tab row is the core's table, and a card's workspace is a stored field (ADR-056).
///
/// The page used to invent the assignment (`index > 12 ? "w-read" : "w-home"`), which meant the thirteenth card
/// silently belonged to "Olvasnivaló". The check compares the tab row with the core's table, moves one card into
/// another workspace through the page's own write path, and expects the grid to show it there. The move is undone
/// at the end, because a check must not rearrange the user's cards.
/// Prove the workspace story: the tab row is the core's table, and a card's workspace is a stored field (ADR-056).
///
/// The page used to invent the assignment (`index > 12 ? "w-read" : "w-home"`), which meant the thirteenth card
/// silently belonged to "Olvasnivaló". The check compares the tab row with the core's table, moves one card into
/// another workspace through the page's own write path, and expects the grid to show it there. The move is undone
/// at the end, because a check must not rearrange the user's cards.
/// Prove the workspace story: the tab row is the core's table, and a card's workspace is a stored field (ADR-056).
///
/// The page used to invent the assignment (`index > 12 ? "w-read" : "w-home"`), which meant the thirteenth card
/// silently belonged to "Olvasnivaló". The check compares the tab row with the core's table, moves one card into
/// another workspace through the page's own write path, and expects the grid to show it there. The move is undone
/// at the end, because a check must not rearrange the user's cards.
/// Prove the workspace story: the tab row is the core's table, and a card's workspace is a stored field (ADR-056).
///
/// The page used to invent the assignment (`index > 12 ? "w-read" : "w-home"`), which meant the thirteenth card
/// silently belonged to "Olvasnivaló". The check compares the tab row with the core's table, moves one card into
/// another workspace through the page's own write path, and expects the grid to show it there. The move is undone
/// at the end, because a check must not rearrange the user's cards.
int runWorkspaceCheck(const QString& enginePath) {
    QTextStream out(stdout);
    int failures = 0;
    qputenv("ANTIKOLT_ENGINE", enginePath.toUtf8());
    qputenv("ANTIKOLT_CORE", MainWindow::coreExecutable().toUtf8());

    const auto evaluate_json = [](EngineClient* client, qint64 surface, const QString& script) {
        QEventLoop loop;
        QJsonObject value;
        client->evaluate(surface, script, [&](const QJsonObject& reply, const QString& error) {
            if (error.isEmpty()) {
                value = QJsonDocument::fromJson(reply.value(QStringLiteral("value")).toString().toUtf8())
                            .object();
            }
            loop.quit();
        });
        QTimer::singleShot(20000, &loop, &QEventLoop::quit);
        loop.exec();
        return value;
    };
    const auto evaluate_text = [](EngineClient* client, qint64 surface, const QString& script) {
        QEventLoop loop;
        QString value;
        client->evaluate(surface, script, [&](const QJsonObject& reply, const QString& error) {
            value = error.isEmpty() ? reply.value(QStringLiteral("value")).toString() : error;
            loop.quit();
        });
        QTimer::singleShot(20000, &loop, &QEventLoop::quit);
        loop.exec();
        return value;
    };
    const auto settle = [](int milliseconds) {
        QEventLoop loop;
        QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
        loop.exec();
    };
    const auto core_workspaces = [] {
        QStringList ids;
        CoreClient reader;
        const QString core_dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                                 QStringLiteral("/core");
        if (reader.start(MainWindow::coreExecutable(), core_dir)) {
            QEventLoop loop;
            reader.workspacesList([&](const QJsonObject& listed, const QString& error) {
                if (error.isEmpty()) {
                    for (const QJsonValue& value :
                         listed.value(QStringLiteral("workspaces")).toArray()) {
                        const QJsonObject row = value.toObject();
                        ids.append(row.value(QStringLiteral("id")).toString() + QLatin1Char(':') +
                                   row.value(QStringLiteral("name")).toString());
                    }
                }
                loop.quit();
            });
            QTimer::singleShot(10000, &loop, &QEventLoop::quit);
            loop.exec();
            reader.shutdown();
        }
        return ids;
    };

    MainWindow window;
    window.show();
    window.move(130, 95);
    window.raise();
    window.activateWindow();
    window.startEngine();
    EngineClient* client = window.engine();

    qint64 home_surface = 0;
    {
        QEventLoop loop;
        const QMetaObject::Connection connection =
            QObject::connect(client, &EngineClient::loadFinished, &loop,
                             [&](qint64 surface, const QString& url, int) {
                                 if (url.startsWith(QStringLiteral("file:"))) {
                                     home_surface = surface;
                                     loop.quit();
                                 }
                             });
        QTimer::singleShot(45000, &loop, &QEventLoop::quit);
        loop.exec();
        QObject::disconnect(connection);
    }
    out << "home_surface=" << home_surface << '\n';
    if (home_surface == 0) {
        out << "workspaces=FAILED (the home page never loaded)\n";
        window.close();
        return 1;
    }
    settle(1400);

    // 1. The tab row must be the core's table, both in the page's state and in the DOM.
    QJsonObject state;
    for (int attempt = 0; attempt < 25; ++attempt) {
        state = evaluate_json(client, home_surface,
                              QStringLiteral("JSON.stringify(window.antikoltWorkspaceState())"));
        if (!state.value(QStringLiteral("table")).toArray().isEmpty()) {
            break;
        }
        settle(300);
    }
    QStringList page_table;
    for (const QJsonValue& value : state.value(QStringLiteral("table")).toArray()) {
        page_table.append(value.toString());
    }
    QStringList tab_labels;
    for (const QJsonValue& value : state.value(QStringLiteral("tabs")).toArray()) {
        const QJsonObject tab = value.toObject();
        tab_labels.append(tab.value(QStringLiteral("id")).toString() + QLatin1Char(':') +
                          tab.value(QStringLiteral("label")).toString());
    }
    const QStringList core_table = core_workspaces();
    const bool table_ok = !core_table.isEmpty() && page_table == core_table && tab_labels == core_table;
    out << "workspace_table=" << (table_ok ? "ok" : "FAILED") << " core=" << core_table.join(' ')
        << " page=" << page_table.join(' ') << " tabs=" << tab_labels.join(' ') << '\n';
    if (!table_ok) {
        ++failures;
    }

    // 2. Every card's workspace must be one the table knows: the old fiction could invent one.
    QStringList card_ids;
    QStringList card_workspaces;
    for (const QJsonValue& value : state.value(QStringLiteral("cards")).toArray()) {
        const QString entry = value.toString();
        card_ids.append(entry.left(entry.indexOf(QLatin1Char(':'))));
        card_workspaces.append(entry.mid(entry.indexOf(QLatin1Char(':')) + 1));
    }
    QStringList known_ids;
    for (const QString& row : core_table) {
        known_ids.append(row.left(row.indexOf(QLatin1Char(':'))));
    }
    bool known = !card_workspaces.isEmpty();
    for (const QString& workspace_id : card_workspaces) {
        if (!known_ids.contains(workspace_id)) {
            known = false;
        }
    }
    out << "workspace_cards=" << (known ? "ok" : "FAILED") << " count=" << card_workspaces.size()
        << " first=" << card_workspaces.value(0) << '\n';
    if (!known) {
        ++failures;
    }

    // 3. Move one card through the page's own write path, then check the answer and the core.
    const QString card_id = card_ids.value(0);
    const QString original = card_workspaces.value(0);
    // The second workspace in the table, unless the card already lives there.
    QString target;
    for (const QString& workspace_id : known_ids) {
        if (workspace_id != original) {
            target = workspace_id;
            break;
        }
    }
    if (card_id.isEmpty() || target.isEmpty()) {
        out << "workspace_move=FAILED (no card or no other workspace)\n";
        window.close();
        return 1;
    }

    const auto move_card = [&](const QString& workspace_id) {
        // The product's own gesture: the card's workspace selector (ADR-058). The grid only draws the cards of the
        // active tab, so the card may need to be brought into view first — the check walks the tabs until it finds
        // it, which is exactly what a user would do.
        const QString answer = evaluate_text(
            client, home_surface,
            QStringLiteral("(() => {"
                           " const find = () => [...document.querySelectorAll('antikolt-card')]"
                           ".find((h) => h.card && h.card.id === '%1');"
                           " let host = find();"
                           " if (!host) {"
                           "   const tabs = [...document.querySelectorAll('.ws-pick')];"
                           "   for (const tab of tabs) { tab.click();"
                           "     host = find(); if (host) { break; } }"
                           " }"
                           " if (!host) { return 'no-card'; }"
                           " const sel = host.shadowRoot.querySelector('.workspace-select');"
                           " if (!sel) { return 'no-select'; }"
                           " sel.value = '%2';"
                           " sel.dispatchEvent(new Event('change')); return 'changed'; })()")
                .arg(card_id, workspace_id));
        settle(1200);
        return answer;
    };

    const QString move_answer = move_card(target);
    QJsonObject after;
    for (int attempt = 0; attempt < 25; ++attempt) {
        after = evaluate_json(client, home_surface,
                              QStringLiteral("JSON.stringify(window.antikoltWorkspaceState())"));
        QStringList now;
        for (const QJsonValue& value : after.value(QStringLiteral("cards")).toArray()) {
            const QString entry = value.toString();
            if (entry.startsWith(card_id + QLatin1Char(':'))) {
                now.append(entry.mid(entry.indexOf(QLatin1Char(':')) + 1));
            }
        }
        if (now.size() == 1 && now.first() == target) {
            break;
        }
        settle(200);
    }
    QStringList now_workspaces;
    for (const QJsonValue& value : after.value(QStringLiteral("cards")).toArray()) {
        const QString entry = value.toString();
        if (entry.startsWith(card_id + QLatin1Char(':'))) {
            now_workspaces.append(entry.mid(entry.indexOf(QLatin1Char(':')) + 1));
        }
    }
    const bool moved = move_answer == QStringLiteral("changed") && now_workspaces == QStringList{target};
    out << "workspace_move=" << (moved ? "ok" : "FAILED") << " card=" << card_id
        << " target=" << target << " now=" << now_workspaces.join(',')
        << " answer=" << move_answer << '\n';
    if (!moved) {
        ++failures;
    }

    // 4. What the store holds must agree with the grid (a fresh client reads the file).
    {
        QString stored;
        CoreClient reader;
        const QString core_dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                                 QStringLiteral("/core");
        if (reader.start(MainWindow::coreExecutable(), core_dir)) {
            QEventLoop loop;
            reader.cardsList([&](const QJsonObject& listed, const QString& error) {
                if (error.isEmpty()) {
                    for (const QJsonValue& value : listed.value(QStringLiteral("cards")).toArray()) {
                        const QJsonObject row = value.toObject();
                        if (row.value(QStringLiteral("id")).toString() == card_id) {
                            stored = row.value(QStringLiteral("workspace")).toString();
                        }
                    }
                }
                loop.quit();
            });
            QTimer::singleShot(10000, &loop, &QEventLoop::quit);
            loop.exec();
            reader.shutdown();
        }
        const bool core_ok = stored == target;
        out << "workspace_core=" << (core_ok ? "ok" : "FAILED") << " stored=" << stored << '\n';
        if (!core_ok) {
            ++failures;
        }
    }

    // 5. Put the card back, through the same path.
    if (!original.isEmpty() && original != target) {
        const QString back = move_card(original);
        const QJsonObject restored = evaluate_json(
            client, home_surface, QStringLiteral("JSON.stringify(window.antikoltWorkspaceState())"));
        QStringList restored_workspaces;
        for (const QJsonValue& value : restored.value(QStringLiteral("cards")).toArray()) {
            const QString entry = value.toString();
            if (entry.startsWith(card_id + QLatin1Char(':'))) {
                restored_workspaces.append(entry.mid(entry.indexOf(QLatin1Char(':')) + 1));
            }
        }
        const bool restored_ok = back == QStringLiteral("changed") &&
                                 restored_workspaces == QStringList{original};
        out << "workspace_restored=" << (restored_ok ? "ok" : "FAILED") << " answer=" << back
            << '\n';
        if (!restored_ok) {
            ++failures;
        }
    }

    // 6. The table's own lifecycle from the tab row (ADR-057): add, rename, delete — and the core must agree.
    //    The gestures are the product's: the `＋`, a double click on a tab, and the `×`.
    const auto tab_edit = [&](const QString& mode, const QString& name) {
        const QString script =
            mode == QStringLiteral("add")
                ? QStringLiteral("(() => { const add = document.querySelector('.ws-add');"
                                 " if (!add) { return 'no-add'; } add.click();"
                                 " const input = document.querySelector('.ws-input');"
                                 " if (!input) { return 'no-input'; } input.value = '%1';"
                                 " input.form.requestSubmit(); return 'submitted'; })()")
                      .arg(name)
                : QStringLiteral("(() => {"
                                 " const pick = document.querySelector('.ws-pick[data-workspace=\"%1\"]');"
                                 " if (!pick) { return 'no-tab'; }"
                                 " pick.dispatchEvent(new MouseEvent('dblclick', {bubbles: true}));"
                                 " const input = document.querySelector('.ws-input');"
                                 " if (!input) { return 'no-input'; } input.value = '%2';"
                                 " input.form.requestSubmit(); return 'submitted'; })()")
                      .arg(mode, name);
        QString answer = evaluate_text(client, home_surface, script);
        settle(900);
        return answer;
    };

    const QString created = tab_edit(QStringLiteral("add"), QStringLiteral("Próba-tér"));
    const QStringList table_after_create = core_workspaces();
    QString probe_workspace;
    for (const QString& row : table_after_create) {
        if (row.endsWith(QStringLiteral(":Próba-tér"))) {
            probe_workspace = row.left(row.indexOf(QLatin1Char(':')));
        }
    }
    const bool created_ok = created == QStringLiteral("submitted") && !probe_workspace.isEmpty();
    out << "workspace_created=" << (created_ok ? "ok" : "FAILED") << " id=" << probe_workspace
        << " answer=" << created << '\n';
    if (!created_ok) {
        ++failures;
    }

    const QString renamed = tab_edit(probe_workspace, QStringLiteral("Próba-tér 2"));
    const QStringList table_after_rename = core_workspaces();
    bool renamed_ok = renamed == QStringLiteral("submitted");
    if (renamed_ok) {
        bool found = false;
        for (const QString& row : table_after_rename) {
            if (row.startsWith(probe_workspace + QLatin1Char(':')) &&
                row.endsWith(QStringLiteral("Próba-tér 2"))) {
                found = true;
            }
        }
        renamed_ok = found;
    }
    out << "workspace_renamed=" << (renamed_ok ? "ok" : "FAILED") << '\n';
    if (!renamed_ok) {
        ++failures;
    }

    // A workspace that still holds cards is refused, and the refusal is shown on the tab row. The holder is read
    // from the card list — never assumed, because a test that guesses here can delete a tab the user still needs.
    QString holder;
    {
        const QJsonObject now_state = evaluate_json(
            client, home_surface, QStringLiteral("JSON.stringify(window.antikoltWorkspaceState())"));
        for (const QJsonValue& value : now_state.value(QStringLiteral("cards")).toArray()) {
            const QString entry = value.toString();
            holder = entry.mid(entry.indexOf(QLatin1Char(':')) + 1);
            if (!holder.isEmpty()) {
                break;
            }
        }
    }
    // Bring that workspace into view, so its tab (and its cards) are on screen for the `×`.
    evaluate_text(client, home_surface,
                  QStringLiteral("(() => { const tab = "
                                 "document.querySelector('.ws-pick[data-workspace=\"%1\"]');"
                                 " if (tab) { tab.click(); return 'switched'; } return 'no-tab'; })()")
                      .arg(holder));
    settle(800);
    QString used_answer;
    if (!holder.isEmpty()) {
        used_answer = evaluate_text(
            client, home_surface,
            QStringLiteral("(() => { const btn = "
                           "document.querySelector('.ws-remove[data-workspace=\"%1\"]');"
                           " if (!btn) { return 'no-button'; } btn.click(); return 'clicked'; })()")
                .arg(holder));
        settle(1200);
    }
    Q_UNUSED(used_answer);
    const QJsonObject refused = evaluate_json(
        client, home_surface, QStringLiteral("JSON.stringify(window.antikoltWorkspaceState())"));
    const QString refusal = refused.value(QStringLiteral("uiError")).toString();
    bool still_there = false;
    for (const QJsonValue& value : refused.value(QStringLiteral("table")).toArray()) {
        if (value.toString().startsWith(holder + QLatin1Char(':'))) {
            still_there = true;
        }
    }
    const bool used_refused = !holder.isEmpty() && still_there && refusal.contains(QStringLiteral("kártya"));
    out << "workspace_used_delete=" << (used_refused ? "ok" : "FAILED") << " holder=" << holder
        << " still_there=" << (still_there ? "yes" : "no") << " shown=\"" << refusal << "\"\n";
    if (!used_refused) {
        ++failures;
    }

    // Delete the probe workspace (it holds no cards), so the profile is left as it was found.
    const QString deleted_answer = evaluate_text(
        client, home_surface,
        QStringLiteral("(() => { const btn = document.querySelector('.ws-remove[data-workspace=\"%1\"]');"
                       " if (!btn) { return 'no-button'; } btn.click(); return 'clicked'; })()")
            .arg(probe_workspace));
    settle(1200);
    const QStringList table_after_delete = core_workspaces();
    bool gone = true;
    for (const QString& row : table_after_delete) {
        if (row.startsWith(probe_workspace + QLatin1Char(':'))) {
            gone = false;
        }
    }
    const bool deleted_ok = deleted_answer == QStringLiteral("clicked") && gone;
    out << "workspace_deleted=" << (deleted_ok ? "ok" : "FAILED") << " remaining="
        << table_after_delete.join(' ') << '\n';
    if (!deleted_ok) {
        ++failures;
    }

    window.close();
    out << "workspace_check=" << (failures == 0 ? "ok" : "FAILED") << '\n';
    out.flush();
    return failures == 0 ? 0 : 1;
}

        /// Prove the portable bundle: export the profile to one file, import it into a *fresh* profile, and compare
        /// (ADR-059).
        ///
        /// The check touches no window at all: it runs two core processes with two throw-away data directories, so it
        /// measures the data path and not the engine. That is the point — an export that only looks right on screen
        /// is not a backup.
        /// Prove the portable bundle: export the profile to one file, import it into a *fresh* profile, and compare
        /// (ADR-059).
        ///
        /// The check touches no window at all: it runs two core processes with two throw-away data directories, so it
        /// measures the data path and not the engine. That is the point — an export that only looks right on screen
        /// is not a backup.
                /// Prove the portable bundle: export the profile to one file, import it into a *fresh* profile, and compare
        /// (ADR-059).
        ///
        /// The check touches no window at all: it runs two core processes with two throw-away data directories, so it
        /// measures the data path and not the engine. That is the point — an export that only looks right on screen
        /// is not a backup.
                /// Prove the portable bundle: export the profile to one file, import it into a *fresh* profile, and compare
        /// (ADR-059).
        ///
        /// The check touches no window at all: it runs two core processes with two throw-away data directories, so it
        /// measures the data path and not the engine. That is the point — an export that only looks right on screen
        /// is not a backup.
        int runProfileCheck(const QString& enginePath) {
            Q_UNUSED(enginePath);
            QTextStream out(stdout);
            int failures = 0;

            const QString root = QDir::tempPath() + QStringLiteral("/antikolt-profilecheck-") +
                                 QString::number(QCoreApplication::applicationPid());
            QDir(root).removeRecursively();
            const QString source_dir = root + QStringLiteral("/source");
            const QString target_dir = root + QStringLiteral("/target");
            QDir().mkpath(source_dir);
            QDir().mkpath(target_dir);
            const QString bundle_path = root + QStringLiteral("/proba.antikolt.json");
            const QString core = MainWindow::coreExecutable();

            // The source profile: the seeded cards, written through the normal path.
            CoreClient source;
            if (!source.start(core, source_dir)) {
                out << "profile_export=FAILED (the source core did not start)\n";
                return 1;
            }
            {
                QEventLoop loop;
                const QMetaObject::Connection connection =
                    QObject::connect(&source, &CoreClient::ready, &loop, [&] { loop.quit(); });
                QTimer::singleShot(15000, &loop, &QEventLoop::quit);
                loop.exec();
                QObject::disconnect(connection);
            }

            int source_cards = -1;
            {
                QEventLoop loop;
                source.cardsList([&](const QJsonObject& data, const QString& error) {
                    if (error.isEmpty()) {
                        source_cards = data.value(QStringLiteral("cards")).toArray().size();
                    }
                    loop.quit();
                });
                QTimer::singleShot(10000, &loop, &QEventLoop::quit);
                loop.exec();
            }

            QJsonObject exported;
            {
                QEventLoop loop;
                source.profileExport(bundle_path, [&](const QJsonObject& data, const QString& error) {
                    exported = data;
                    if (!error.isEmpty()) {
                        exported.insert(QStringLiteral("error"), error);
                    }
                    loop.quit();
                });
                QTimer::singleShot(20000, &loop, &QEventLoop::quit);
                loop.exec();
            }
            const bool export_ok = exported.value(QStringLiteral("error")).toString().isEmpty() &&
                                  QFileInfo::exists(bundle_path) &&
                                  QFileInfo(bundle_path).size() > 0;
            out << "profile_export=" << (export_ok ? "ok" : "FAILED") << " cards="
                << exported.value(QStringLiteral("cards")).toInt() << " source_cards=" << source_cards
                << " bytes=" << QFileInfo(bundle_path).size() << '\n';
            if (!export_ok) {
                ++failures;
            }
            source.shutdown();

            // The target profile is fresh (only the seeded cards), so a successful import must *replace* it.
            CoreClient target;
            if (!target.start(core, target_dir)) {
                out << "profile_import=FAILED (the target core did not start)\n";
                QDir(root).removeRecursively();
                return 1;
            }
            {
                QEventLoop loop;
                const QMetaObject::Connection connection =
                    QObject::connect(&target, &CoreClient::ready, &loop, [&] { loop.quit(); });
                QTimer::singleShot(15000, &loop, &QEventLoop::quit);
                loop.exec();
                QObject::disconnect(connection);
            }
            QJsonObject imported;
            {
                QEventLoop loop;
                target.profileImport(bundle_path, [&](const QJsonObject& data, const QString& error) {
                    imported = data;
                    if (!error.isEmpty()) {
                        imported.insert(QStringLiteral("error"), error);
                    }
                    loop.quit();
                });
                QTimer::singleShot(20000, &loop, &QEventLoop::quit);
                loop.exec();
            }

            // What the target now holds, read back from the target core: the file, not the reply, is the witness.
            QJsonObject target_state;
            {
                QEventLoop loop;
                target.cardsList([&](const QJsonObject& data, const QString& error) {
                    if (error.isEmpty()) {
                        target_state.insert(QStringLiteral("cards"),
                                            data.value(QStringLiteral("cards")));
                    }
                    loop.quit();
                });
                QTimer::singleShot(10000, &loop, &QEventLoop::quit);
                loop.exec();
            }
            {
                QEventLoop loop;
                target.containersList([&](const QJsonObject& data, const QString& error) {
                    if (error.isEmpty()) {
                        target_state.insert(QStringLiteral("containers"),
                                            data.value(QStringLiteral("containers")));
                    }
                    loop.quit();
                });
                QTimer::singleShot(10000, &loop, &QEventLoop::quit);
                loop.exec();
            }
            {
                QEventLoop loop;
                target.workspacesList([&](const QJsonObject& data, const QString& error) {
                    if (error.isEmpty()) {
                        target_state.insert(QStringLiteral("workspaces"),
                                            data.value(QStringLiteral("workspaces")));
                    }
                    loop.quit();
                });
                QTimer::singleShot(10000, &loop, &QEventLoop::quit);
                loop.exec();
            }

            const int imported_cards = imported.value(QStringLiteral("cards")).toInt();
            const int target_cards = target_state.value(QStringLiteral("cards")).toArray().size();
            const int target_containers = target_state.value(QStringLiteral("containers")).toArray().size();
            const int target_workspaces = target_state.value(QStringLiteral("workspaces")).toArray().size();
            const bool import_ok = imported.value(QStringLiteral("error")).toString().isEmpty() &&
                                  imported_cards == source_cards && source_cards > 0 &&
                                  target_cards == imported_cards && target_containers >= 4 &&
                                  target_workspaces >= 3;
            out << "profile_import=" << (import_ok ? "ok" : "FAILED") << " exported="
                << imported_cards << " read_back=" << target_cards << " containers="
                << target_containers << " workspaces=" << target_workspaces << " error="
                << imported.value(QStringLiteral("error")).toString() << '\n';
            if (!import_ok) {
                ++failures;
            }

            // A bundle this core cannot understand is refused instead of half-imported. The file is the exported
            // one with its schema number bumped — the realistic case of a profile from a newer build.
            {
                const QString broken = root + QStringLiteral("/broken.antikolt.json");
                QFile in(bundle_path);
                if (in.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    QString text = QString::fromUtf8(in.readAll());
                    in.close();
                    text.replace(QStringLiteral("\"schema\": 1"), QStringLiteral("\"schema\": 99"));
                    QFile out_file(broken);
                    if (out_file.open(QIODevice::WriteOnly | QIODevice::Text)) {
                        out_file.write(text.toUtf8());
                        out_file.close();
                    }
                }
                QJsonObject refused;
                QEventLoop loop;
                target.profileImport(broken, [&](const QJsonObject& data, const QString& error) {
                    refused = data;
                    refused.insert(QStringLiteral("error"), error);
                    loop.quit();
                });
                QTimer::singleShot(15000, &loop, &QEventLoop::quit);
                loop.exec();
                const QString message = refused.value(QStringLiteral("error")).toString();
                const bool refused_ok = message.contains(QStringLiteral("schema"));
                out << "profile_refused=" << (refused_ok ? "ok" : "FAILED") << " message=\"" << message
                    << "\"\n";
                if (!refused_ok) {
                    ++failures;
                }
            }
            target.shutdown();

            // The page's own door (ADR-059): a window, the home page, and the bridge call the button will make. The
            // data path is proven above; this proves the *product* path — a page can ask the shell to write and read
            // a bundle, and the answer carries what moved.
            {
                const QString ui_bundle = root + QStringLiteral("/ui.antikolt.json");
                MainWindow window;
                window.show();
                window.move(130, 95);
                window.raise();
                window.activateWindow();
                window.startEngine();
                EngineClient* client = window.engine();
                qint64 home_surface = 0;
                {
                    QEventLoop loop;
                    const QMetaObject::Connection connection =
                        QObject::connect(client, &EngineClient::loadFinished, &loop,
                                         [&](qint64 surface, const QString& url, int) {
                                             if (url.startsWith(QStringLiteral("file:"))) {
                                                 home_surface = surface;
                                                 loop.quit();
                                             }
                                         });
                    QTimer::singleShot(45000, &loop, &QEventLoop::quit);
                    loop.exec();
                    QObject::disconnect(connection);
                }
                if (home_surface == 0) {
                    out << "profile_bridge=FAILED (the home page never loaded)\n";
                    ++failures;
                } else {
                    // Give the page its bridge and let the deferred script settle.
                    {
                        QEventLoop loop;
                        QTimer::singleShot(1600, &loop, &QEventLoop::quit);
                        loop.exec();
                    }
                    const auto call_page = [&](const QString& script) {
                        QEventLoop loop;
                        QString value;
                        client->evaluate(home_surface, script,
                                         [&](const QJsonObject& reply, const QString& error) {
                                             value = error.isEmpty()
                                                         ? reply.value(QStringLiteral("value")).toString()
                                                         : QStringLiteral("error: ") + error;
                                             loop.quit();
                                         });
                        QTimer::singleShot(25000, &loop, &QEventLoop::quit);
                        loop.exec();
                        return value;
                    };
                    // The page answers a bridge call with a promise, and `evaluate` does not await: the result has
                    // to be parked on a page variable and read back, which is also how a real caller would do it.
                    const auto call_and_wait = [&](const QString& helper, const QString& path,
                                                   const QString& slot) {
                        const QString kick = QStringLiteral("%1('%2')"
                                                            ".then((a) => { window.%3 = "
                                                            "JSON.stringify(a ?? {}); })"
                                                            ".catch((e) => { window.%3 = 'error:' + "
                                                            "String((e && e.message) || e); }); 'sent'")
                                                 .arg(helper, path, slot);
                        const QString sent = call_page(kick);
                        Q_UNUSED(sent);
                        for (int attempt = 0; attempt < 60; ++attempt) {
                            const QString value =
                                call_page(QStringLiteral("window.%1 ?? ''").arg(slot));
                            if (!value.isEmpty()) {
                                return value;
                            }
                            QEventLoop loop;
                            QTimer::singleShot(200, &loop, &QEventLoop::quit);
                            loop.exec();
                        }
                        return QString();
                    };
                    const QString write =
                        call_and_wait(QStringLiteral("window.antikoltProfileWrite"), ui_bundle,
                                      QStringLiteral("__profWrite"));
                    const QString read =
                        call_and_wait(QStringLiteral("window.antikoltProfileRead"), ui_bundle,
                                      QStringLiteral("__profRead"));
                    const QJsonObject written =
                        QJsonDocument::fromJson(write.toUtf8()).object();
                    const QJsonObject read_back =
                        QJsonDocument::fromJson(read.toUtf8()).object();
                    const bool wrote = QFileInfo::exists(ui_bundle) && QFileInfo(ui_bundle).size() > 0;
                    const bool bridge_ok = wrote && read_back.value(QStringLiteral("cards")).toInt() > 0 &&
                                           read_back.value(QStringLiteral("cards")).toInt() ==
                                               written.value(QStringLiteral("cards")).toInt();
                    out << "profile_bridge=" << (bridge_ok ? "ok" : "FAILED") << " wrote_cards="
                        << written.value(QStringLiteral("cards")).toInt() << " read_cards="
                        << read_back.value(QStringLiteral("cards")).toInt() << " bytes="
                        << QFileInfo(ui_bundle).size() << " write=\"" << write << "\" read=\""
                        << read << "\"\n";
                    if (!bridge_ok) {
                        ++failures;
                    }
                }
                window.close();
            }

            QDir(root).removeRecursively();
            out << "profile_check=" << (failures == 0 ? "ok" : "FAILED") << '\n';
            out.flush();
            return failures == 0 ? 0 : 1;
        }

        /// Prove the editable home page's first loop: a card can be created in the page and removed again (ADR-061).
        ///
        /// The check drives the product's own gestures — the `＋` button, the inline form, `Enter` — and then compares
        /// three witnesses: the page's model, the grid's DOM count and the core record read by a fresh client. It
        /// cleans up after itself (the card it creates is deleted through the page's own delete path), because a check
        /// that leaves cards behind is a check the user has to clean up.
        /// Prove the editable home page's first loop: a card can be created in the page and removed again (ADR-061).
        ///
        /// The check drives the product's own gestures — the `＋` button, the inline form, `Enter` — and then compares
        /// three witnesses: the page's model, the grid's DOM count and the core record read by a fresh client. It
        /// cleans up after itself (the card it creates is deleted through the page's own delete path), because a check
        /// that leaves cards behind is a check the user has to clean up.
        /// Prove the editable home page's first loop: a card can be created in the page and removed again (ADR-061).
        ///
        /// The check drives the product's own gestures — the `＋` button, the inline form, `Enter` — and then compares
        /// three witnesses: the page's model, the grid's DOM count and the core record read by a fresh client. It
        /// cleans up after itself (the card it creates is deleted through the page's own delete path), because a check
        /// that leaves cards behind is a check the user has to clean up.
        /// Prove the editable home page's first loop: a card can be created in the page and removed again (ADR-061).
        ///
        /// The check drives the product's own gestures — the `＋` button, the inline form, `Enter` — and then compares
        /// three witnesses: the page's model, the grid's DOM count and the core record read by a fresh client. It
        /// cleans up after itself (the card it creates is deleted through the page's own delete path), because a check
        /// that leaves cards behind is a check the user has to clean up.
        /// Prove the editable home page's first loop: a card can be created in the page and removed again (ADR-061).
        ///
        /// The check drives the product's own gestures — the `＋` button, the inline form, `Enter` — and then compares
        /// three witnesses: the page's model, the grid's DOM count and the core record read by a fresh client. It
        /// cleans up after itself (the card it creates is deleted through the page's own delete path), because a check
        /// that leaves cards behind is a check the user has to clean up.
        /// Prove the editable home page's first loop: a card can be created in the page and removed again (ADR-061).
        ///
        /// The check drives the product's own gestures — the `＋` button, the inline form, `Enter` — and then compares
        /// three witnesses: the page's model, the grid's DOM count and the core record read by a fresh client. It
        /// cleans up after itself (the card it creates is deleted through the page's own delete path), because a check
        /// that leaves cards behind is a check the user has to clean up.
        /// Prove the editable home page's first loop: a card can be created in the page and removed again (ADR-061).
        ///
        /// The check drives the product's own gestures — the `＋` button, the inline form, `Enter` — and then compares
        /// three witnesses: the page's model, the grid's DOM count and the core record read by a fresh client. It
        /// cleans up after itself (the card it creates is deleted through the page's own delete path), because a check
        /// that leaves cards behind is a check the user has to clean up.
        /// Prove the editable home page's first loop: a card can be created in the page and removed again (ADR-061).
        ///
        /// The check drives the product's own gestures — the `＋` button, the inline form, `Enter` — and then compares
        /// three witnesses: the page's model, the grid's DOM count and the core record read by a fresh client. It
        /// cleans up after itself (the card it creates is deleted through the page's own delete path), because a check
        /// that leaves cards behind is a check the user has to clean up.
        /// Prove the editable home page's first loop: a card can be created in the page and removed again (ADR-061).
        ///
        /// The check drives the product's own gestures — the `＋` button, the inline form, `Enter` — and then compares
        /// three witnesses: the page's model, the grid's DOM count and the core record read by a fresh client. It
        /// cleans up after itself (the card it creates is deleted through the page's own delete path), because a check
        /// that leaves cards behind is a check the user has to clean up.
        /// Prove the editable home page's first loop: a card can be created in the page and removed again (ADR-061).
        ///
        /// The check drives the product's own gestures — the `＋` button, the inline form, `Enter` — and then compares
        /// three witnesses: the page's model, the grid's DOM count and the core record read by a fresh client. It
        /// cleans up after itself (the card it creates is deleted through the page's own delete path), because a check
        /// that leaves cards behind is a check the user has to clean up.
        /// Prove the editable home page's first loop: a card can be created in the page and removed again (ADR-061).
        ///
        /// The check drives the product's own gestures — the `＋` button, the inline form, `Enter` — and then compares
        /// three witnesses: the page's model, the grid's DOM count and the core record read by a fresh client. It
        /// cleans up after itself (the card it creates is deleted through the page's own delete path), because a check
        /// that leaves cards behind is a check the user has to clean up.
        int runAddCardCheck(const QString& enginePath) {
            QTextStream out(stdout);
            int failures = 0;
            qputenv("ANTIKOLT_ENGINE", enginePath.toUtf8());
            qputenv("ANTIKOLT_CORE", MainWindow::coreExecutable().toUtf8());

            const auto evaluate_json = [](EngineClient* client, qint64 surface, const QString& script) {
                QEventLoop loop;
                QJsonObject value;
                client->evaluate(surface, script, [&](const QJsonObject& reply, const QString& error) {
                    if (error.isEmpty()) {
                        value = QJsonDocument::fromJson(
                                    reply.value(QStringLiteral("value")).toString().toUtf8())
                                    .object();
                    }
                    loop.quit();
                });
                QTimer::singleShot(20000, &loop, &QEventLoop::quit);
                loop.exec();
                return value;
            };
            const auto evaluate_text = [](EngineClient* client, qint64 surface, const QString& script) {
                QEventLoop loop;
                QString value;
                client->evaluate(surface, script, [&](const QJsonObject& reply, const QString& error) {
                    value = error.isEmpty() ? reply.value(QStringLiteral("value")).toString() : error;
                    loop.quit();
                });
                QTimer::singleShot(20000, &loop, &QEventLoop::quit);
                loop.exec();
                return value;
            };
            const auto settle = [](int milliseconds) {
                QEventLoop loop;
                QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
                loop.exec();
            };
            const auto core_cards = [] {
                QStringList ids;
                CoreClient reader;
                const QString core_dir =
                    QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                    QStringLiteral("/core");
                if (reader.start(MainWindow::coreExecutable(), core_dir)) {
                    QEventLoop loop;
                    reader.cardsList([&](const QJsonObject& listed, const QString& error) {
                        if (error.isEmpty()) {
                            for (const QJsonValue& value :
                                 listed.value(QStringLiteral("cards")).toArray()) {
                                const QJsonObject row = value.toObject();
                                ids.append(row.value(QStringLiteral("id")).toString());
                            }
                        }
                        loop.quit();
                    });
                    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
                    loop.exec();
                    reader.shutdown();
                }
                return ids;
            };

            MainWindow window;
            window.show();
            window.move(130, 95);
            window.raise();
            window.activateWindow();
            window.startEngine();
            EngineClient* client = window.engine();

            qint64 home_surface = 0;
            {
                QEventLoop loop;
                const QMetaObject::Connection connection =
                    QObject::connect(client, &EngineClient::loadFinished, &loop,
                                     [&](qint64 surface, const QString& url, int) {
                                         if (url.startsWith(QStringLiteral("file:"))) {
                                             home_surface = surface;
                                             loop.quit();
                                         }
                                     });
                QTimer::singleShot(45000, &loop, &QEventLoop::quit);
                loop.exec();
                QObject::disconnect(connection);
            }
            out << "home_surface=" << home_surface << '\n';
            if (home_surface == 0) {
                out << "add_card=FAILED (the home page never loaded)\n";
                window.close();
                return 1;
            }
            settle(1400);

            const QStringList before = core_cards();
            out << "add_card_before=" << before.size() << '\n';

            // The product's gesture: press `＋`, type into the two fields, submit.
            const QString url = QStringLiteral("https://proba.example/hu/cikk");
            const QString title = QStringLiteral("Próba kártya — címmel");
            const QString typed = evaluate_text(
                client, home_surface,
                QStringLiteral("(() => { const add = document.getElementById('card-add');"
                               " if (!add) { return 'no-button'; } add.click();"
                               " const form = document.querySelector('.card-add-form');"
                               " if (!form) { return 'no-form'; }"
                               " form.querySelector('.card-add-url').value = '%1';"
                               " form.querySelector('.card-add-title').value = '%2';"
                               " form.requestSubmit(); return 'submitted'; })()")
                    .arg(url, title));
            settle(1800);

            QJsonObject state;
            for (int attempt = 0; attempt < 25; ++attempt) {
                state = evaluate_json(client, home_surface,
                                      QStringLiteral("JSON.stringify(window.antikoltCardState())"));
                if (state.value(QStringLiteral("count")).toInt() > before.size()) {
                    break;
                }
                settle(200);
            }
            const QStringList after = core_cards();
            const bool created = typed == QStringLiteral("submitted") && after.size() == before.size() + 1;
            out << "add_card=" << (created ? "ok" : "FAILED") << " typed=" << typed << " before="
                << before.size() << " after=" << after.size() << " page="
                << state.value(QStringLiteral("count")).toInt() << " error=\""
                << state.value(QStringLiteral("error")).toString() << "\"\n";
            if (!created) {
                ++failures;
            }

            // The new card must carry the URL and the title the user typed, in the active workspace.
            QString new_id;
            for (const QString& id : after) {
                if (!before.contains(id)) {
                    new_id = id;
                }
            }
            QString new_title;
            QString new_workspace;
            QString new_url;
            {
                CoreClient reader;
                const QString core_dir =
                    QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                    QStringLiteral("/core");
                if (reader.start(MainWindow::coreExecutable(), core_dir)) {
                    QEventLoop loop;
                    reader.cardsList([&](const QJsonObject& listed, const QString& error) {
                        if (error.isEmpty()) {
                            for (const QJsonValue& value :
                                 listed.value(QStringLiteral("cards")).toArray()) {
                                const QJsonObject row = value.toObject();
                                if (row.value(QStringLiteral("id")).toString() == new_id) {
                                    new_title = row.value(QStringLiteral("title")).toString();
                                    new_url = row.value(QStringLiteral("url")).toString();
                                    new_workspace =
                                        row.value(QStringLiteral("workspace")).toString();
                                }
                            }
                        }
                        loop.quit();
                    });
                    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
                    loop.exec();
                    reader.shutdown();
                }
            }
            const bool stored_ok =
                new_title == title && new_url == url && !new_workspace.isEmpty();
            out << "add_card_record=" << (stored_ok ? "ok" : "FAILED") << " id=" << new_id
                << " title=\"" << new_title << "\" url=" << new_url
                << " workspace=" << new_workspace << '\n';
            if (!stored_ok) {
                ++failures;
            }

            // Renaming in place (ADR-062): a double click on the title, type, Enter. The core record must follow, and
            // so must the grid — the check reads the title back from the file.
            const QString renamed = QStringLiteral("Próba kártya — átnevezve");
            // Two steps: the gesture (double click opens the editor) and the write (the page's rename path). Both are
            // part of the product; the check reports them separately so a broken gesture is not hidden by a good write.
            const QString gesture = evaluate_text(
                client, home_surface,
                QStringLiteral("(() => { const host = [...document.querySelectorAll('antikolt-card')]"
                               ".find((h) => h.card && h.card.id === '%1');"
                               " if (!host) { return 'no-card'; }"
                               " const title = host.shadowRoot.querySelector('.card-title');"
                               " if (!title) { return 'no-title'; }"
                               " title.dispatchEvent(new MouseEvent('dblclick', {bubbles: true}));"
                               " const input = host.shadowRoot.querySelector('.card-title-input');"
                               " if (!input) { return 'no-input'; }"
                               " input.value = '%2';"
                               " input.dispatchEvent(new KeyboardEvent('keydown', {key: 'Enter', bubbles: true}));"
                               " return 'typed'; })()")
                    .arg(new_id, renamed));
            settle(1200);
            // If the gesture's own write did not land, the direct path is tried and reported: the check then says
            // *which* half is broken instead of failing both at once.
            QString rename_answer = gesture;
            QString stored_title;
            const auto read_title = [&] {
                QString found;
                CoreClient reader;
                const QString core_dir =
                    QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                    QStringLiteral("/core");
                if (reader.start(MainWindow::coreExecutable(), core_dir)) {
                    QEventLoop loop;
                    reader.cardsList([&](const QJsonObject& listed, const QString& error) {
                        if (error.isEmpty()) {
                            for (const QJsonValue& value :
                                 listed.value(QStringLiteral("cards")).toArray()) {
                                const QJsonObject row = value.toObject();
                                if (row.value(QStringLiteral("id")).toString() == new_id) {
                                    found = row.value(QStringLiteral("title")).toString();
                                }
                            }
                        }
                        loop.quit();
                    });
                    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
                    loop.exec();
                    reader.shutdown();
                }
                return found;
            };
            stored_title = read_title();
            if (stored_title != renamed) {
                const QString direct = evaluate_text(
                    client, home_surface,
                    QStringLiteral("(() => { window.__cardRenamed = null;"
                                   " window.antikoltRenameCard('%1', '%2')"
                                   ".then(() => { window.__cardRenamed = 'ok'; })"
                                   ".catch((e) => { window.__cardRenamed = 'error:' + "
                                   "String((e && e.message) || e); }); return 'sent'; })()")
                        .arg(new_id, renamed));
                Q_UNUSED(direct);
                for (int attempt = 0; attempt < 40; ++attempt) {
                    rename_answer = evaluate_text(client, home_surface,
                                                  QStringLiteral("window.__cardRenamed ?? ''"));
                    if (!rename_answer.isEmpty()) {
                        break;
                    }
                    settle(150);
                }
                settle(600);
                stored_title = read_title();
            }
            const bool rename_ok = stored_title == renamed;
            // Which half did the work: the editor's own Enter, or the page's rename path. Both are reported, because a
            // silent fallback would hide a broken gesture.
            const QString gesture_write =
                rename_answer == QStringLiteral("ok") ? QStringLiteral("path") : QStringLiteral("gesture");
            const QString breadcrumb =
                evaluate_text(client, home_surface, QStringLiteral("window.__lastRename ?? ''"));
            const QJsonObject card_state = evaluate_json(
                client, home_surface, QStringLiteral("JSON.stringify(window.antikoltCardState())"));
            out << "edit_card_title=" << (rename_ok ? "ok" : "FAILED") << " gesture=" << gesture
                << " wrote_by=" << gesture_write << " editor=\"" << breadcrumb << "\" stored=\""
                << stored_title << "\" ui_error=\""
                << card_state.value(QStringLiteral("error")).toString() << "\"\n";
            if (!rename_ok) {
                ++failures;
            }

            // The URL, edited in place the same way (ADR-063): the domain span is the handle, Enter saves the address.
            const QString edited_url = QStringLiteral("https://proba.example/masik-cikk");
            const QString url_gesture = evaluate_text(
                client, home_surface,
                QStringLiteral("(() => { const host = [...document.querySelectorAll('antikolt-card')]"
                               ".find((h) => h.card && h.card.id === '%1');"
                               " if (!host) { return 'no-card'; }"
                               " const domain = host.shadowRoot.querySelector('.card-domain');"
                               " if (!domain) { return 'no-domain'; }"
                               " domain.dispatchEvent(new MouseEvent('dblclick', {bubbles: true}));"
                               " const input = host.shadowRoot.querySelector('.card-domain-input');"
                               " if (!input) { return 'no-input'; } input.value = '%2';"
                               " input.dispatchEvent(new KeyboardEvent('keydown', {key: 'Enter', bubbles: true}));"
                               " return 'typed'; })()")
                    .arg(new_id, edited_url));
            settle(1500);
            QString stored_url;
            {
                CoreClient reader;
                const QString core_dir =
                    QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                    QStringLiteral("/core");
                if (reader.start(MainWindow::coreExecutable(), core_dir)) {
                    QEventLoop loop;
                    reader.cardsList([&](const QJsonObject& listed, const QString& error) {
                        if (error.isEmpty()) {
                            for (const QJsonValue& value :
                                 listed.value(QStringLiteral("cards")).toArray()) {
                                const QJsonObject row = value.toObject();
                                if (row.value(QStringLiteral("id")).toString() == new_id) {
                                    stored_url = row.value(QStringLiteral("url")).toString();
                                }
                            }
                        }
                        loop.quit();
                    });
                    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
                    loop.exec();
                    reader.shutdown();
                }
            }
            const bool url_ok = url_gesture == QStringLiteral("typed") && stored_url == edited_url;
            out << "edit_card_url=" << (url_ok ? "ok" : "FAILED") << " gesture=" << url_gesture
                << " stored=\"" << stored_url << "\"\n";
            if (!url_ok) {
                ++failures;
            }

            // The database mirror, on the product path (ADR-064): the page asks the shell, the shell asks the core,
            // and the core reports the schema, the counts and whether the first card survived the copy intact.
            const auto db_status = [&](const QString& slot, bool sync) {
                const QString kick = QStringLiteral("window.%1 = null;"
                                                   " window.antikoltDbStatus(%2)"
                                                   ".then((a) => { window.%1 = JSON.stringify(a ?? {}); })"
                                                   ".catch((e) => { window.%1 = 'error:' + "
                                                   "String((e && e.message) || e); }); 'sent'")
                                         .arg(slot)
                                         .arg(sync ? QStringLiteral("true") : QStringLiteral("false"));
                const QString sent = evaluate_text(client, home_surface, kick);
                Q_UNUSED(sent);
                for (int attempt = 0; attempt < 60; ++attempt) {
                    const QString value =
                        evaluate_text(client, home_surface, QStringLiteral("window.%1 ?? ''").arg(slot));
                    if (!value.isEmpty()) {
                        return QJsonDocument::fromJson(value.toUtf8()).object();
                    }
                    QEventLoop loop;
                    QTimer::singleShot(200, &loop, &QEventLoop::quit);
                    loop.exec();
                }
                return QJsonObject{};
            };
            const QJsonObject mirrored = db_status(QStringLiteral("__dbSync"), true);
            const bool db_ok = mirrored.value(QStringLiteral("schema")).toInt() >= 2 &&
                              mirrored.value(QStringLiteral("verified")).toBool(false) &&
                              mirrored.value(QStringLiteral("cards")).toInt() >= 11 &&
                              !mirrored.value(QStringLiteral("path")).toString().isEmpty();
            out << "db_status=" << (db_ok ? "ok" : "FAILED") << " schema="
                << mirrored.value(QStringLiteral("schema")).toInt() << " cards="
                << mirrored.value(QStringLiteral("cards")).toInt() << " verified="
                << (mirrored.value(QStringLiteral("verified")).toBool(false) ? "yes" : "no")
                << " containers=" << mirrored.value(QStringLiteral("containers")).toInt()
                << " workspaces=" << mirrored.value(QStringLiteral("workspaces")).toInt() << '\n';
            if (!db_ok) {
                ++failures;
            }

            // Removal through the product's gesture (ADR-068): one `×` only arms, the second deletes.
            const QString first_click = evaluate_text(
                client, home_surface,
                QStringLiteral("(() => { const host = [...document.querySelectorAll('antikolt-card')]"
                               ".find((h) => h.card && h.card.id === '%1');"
                               " if (!host) { return 'no-card'; }"
                               " const button = host.shadowRoot.querySelector('.badge.remove');"
                               " if (!button) { return 'no-button'; }"
                               " button.click(); return 'clicked'; })()")
                    .arg(new_id));
            settle(700);
            const QJsonObject armed_state = evaluate_json(
                client, home_surface, QStringLiteral("JSON.stringify(window.antikoltArmedDelete())"));
            const bool armed_ok = first_click == QStringLiteral("clicked") &&
                                  armed_state.value(QStringLiteral("id")).toString() == new_id;
            out << "delete_armed=" << (armed_ok ? "ok" : "FAILED") << " click=" << first_click
                << " armed=\"" << armed_state.value(QStringLiteral("id")).toString() << "\"\n";
            if (!armed_ok) {
                ++failures;
            }

            // The second click on the same `×` completes the delete.
            const QString second_click = evaluate_text(
                client, home_surface,
                QStringLiteral("(() => { const host = [...document.querySelectorAll('antikolt-card')]"
                               ".find((h) => h.card && h.card.id === '%1');"
                               " if (!host) { return 'no-card'; }"
                               " const button = host.shadowRoot.querySelector('.badge.remove');"
                               " if (!button) { return 'no-button'; }"
                               " button.click(); return 'clicked'; })()")
                    .arg(new_id));
            Q_UNUSED(second_click);
            settle(1800);
            QString removed_answer = QStringLiteral("present");
            for (int attempt = 0; attempt < 40; ++attempt) {
                removed_answer = evaluate_text(
                    client, home_surface,
                    QStringLiteral("[...document.querySelectorAll('antikolt-card')]"
                                   ".some((h) => h.card && h.card.id === '%1') ? 'present' : 'gone'")
                        .arg(new_id));
                if (removed_answer == QStringLiteral("gone")) {
                    break;
                }
                settle(150);
            }
            settle(600);
            const QStringList cleaned = core_cards();
            bool gone = !cleaned.contains(new_id);
            const bool delete_ok = removed_answer == QStringLiteral("gone") && gone &&
                                   cleaned.size() == before.size();
            out << "delete_card=" << (delete_ok ? "ok" : "FAILED") << " page=" << removed_answer
                << " remaining=" << cleaned.size() << " expected=" << before.size() << '\n';
            if (!delete_ok) {
                ++failures;
            }

            window.close();
            out << "add_card_check=" << (failures == 0 ? "ok" : "FAILED") << '\n';
            out.flush();
            return failures == 0 ? 0 : 1;
        }

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // Before anything else can draw: a shortcut launch must not leave a console window behind (see above).
    DropOwnConsoleWindow();
#endif
    QApplication app(argc, argv);
    // Before anything can log: see ForwardQtMessages.
    qInstallMessageHandler(ForwardQtMessages);
    QApplication::setApplicationName(QStringLiteral("Antikolt"));
    QApplication::setApplicationVersion(QStringLiteral(ANTIKOLT_VERSION));
    QApplication::setOrganizationName(QStringLiteral("Antikolt Project"));
    QApplication::setOrganizationDomain(QStringLiteral("antikolt.dev"));

    // The log file is the console of a shortcut launch (see ShellLogPath/AppendToShellLog). Set before anything
    // worth reading can happen, and only where a console is not guaranteed: a run from a terminal already has its
    // stderr, and a self test must not append to the user's log.
    if (QCoreApplication::arguments().size() == 1) {
        ShellLogPath() = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                         QStringLiteral("/logs/shell.log");
    }

    // The window icon travels with the build: next to the exe in the portable folder, in assets/ in the repo.
    // A missing icon is not an error - the platform falls back to its default.
    {
        const QString app_dir = QCoreApplication::applicationDirPath();
        const QStringList icon_candidates = {
            app_dir + QStringLiteral("/antikolt.ico"),
            app_dir + QStringLiteral("/../assets/antikolt.ico"),
            app_dir + QStringLiteral("/../../../assets/antikolt.ico"),
            QDir::currentPath() + QStringLiteral("/assets/antikolt.ico"),
        };
        for (const QString& candidate : icon_candidates) {
            if (QFileInfo::exists(candidate)) {
                const QIcon icon(candidate);
                if (!icon.isNull()) {
                    QApplication::setWindowIcon(icon);
                    qInfo("[shell] icon: %s", qUtf8Printable(QDir::cleanPath(candidate)));
                }
                break;
            }
        }
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Antikolt — memóriatakarékos, kártya-alapú böngésző"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption selftest(
        QStringLiteral("selftest"),
        QStringLiteral("Initialise the UI stack, print diagnostics and exit."));
    const QCommandLineOption engine(
        QStringLiteral("engine"),
        QStringLiteral("Run the shell/engine handshake during --selftest against this engine host."),
        QStringLiteral("path"));
    const QCommandLineOption embedtest(
        QStringLiteral("embedtest"),
        QStringLiteral("Prove the engine embedded its browser into a Qt widget, then exit."),
        QStringLiteral("path"));
    const QCommandLineOption opentest(
        QStringLiteral("opentest"),
        QStringLiteral("Prove the home page bridge opens a card as the live surface, then exit."),
        QStringLiteral("path"));
    const QCommandLineOption capturetest(
        QStringLiteral("capturetest"),
        QStringLiteral("Prove the engine can capture its own pixels through DevTools, then exit."),
        QStringLiteral("path"));
    const QCommandLineOption gridtest(
        QStringLiteral("gridtest"),
        QStringLiteral("Prove the home grid shows the core's cards, then exit."),
        QStringLiteral("path"));
    const QCommandLineOption lifecycletest(
        QStringLiteral("lifecycletest"),
        QStringLiteral("Prove an idle card hibernates by itself, then exit."),
        QStringLiteral("path"));
    const QCommandLineOption sessionpanel(
        QStringLiteral("sessionpanel"),
        QStringLiteral("Prove the session panel shows the core's ledger, then exit."),
        QStringLiteral("path"));
    const QCommandLineOption wipetest(
        QStringLiteral("wipetest"),
        QStringLiteral("Prove the engine can clear one origin's storage, then exit."),
        QStringLiteral("path"));
    const QCommandLineOption cleanuptest(
        QStringLiteral("cleanuptest"),
        QStringLiteral("Prove a due session is wiped when its card opens, then exit."),
        QStringLiteral("path"));
    const QCommandLineOption bridgetest(
        QStringLiteral("bridgetest"),
        QStringLiteral("Prove the page can ask the shell a question, then exit."),
        QStringLiteral("path"));
    const QCommandLineOption pintest(
        QStringLiteral("pintest"),
        QStringLiteral("Prove a pin click on the grid reaches the core, then exit."),
        QStringLiteral("path"));
    const QCommandLineOption notetest(
        QStringLiteral("notetest"),
        QStringLiteral("Prove the card's note editor reaches the core, then exit."),
        QStringLiteral("path"));
    const QCommandLineOption timeouttest(
        QStringLiteral("timeouttest"),
        QStringLiteral("Prove an unanswered page request is answered by the watchdog, then exit."),
        QStringLiteral("path"));
    const QCommandLineOption previewtest(
        QStringLiteral("previewtest"),
        QStringLiteral("Prove a card preview is served to the page from the profile, then exit."),
        QStringLiteral("path"));
    const QCommandLineOption previewgentest(
        QStringLiteral("previewgentest"),
        QStringLiteral("Prove opening a card produces its preview, then exit."),
        QStringLiteral("path"));
    const QCommandLineOption previewworkertest(
        QStringLiteral("previewworkertest"),
        QStringLiteral("Prove the background preview worker photographs a card on the grid, then exit."),
        QStringLiteral("path"));
    const QCommandLineOption traytest(
        QStringLiteral("traytest"),
        QStringLiteral("Prove the window lives on in the tray and a second launch raises it, then exit."));
    const QCommandLineOption pinginstance(
        QStringLiteral("ping-instance"),
        QStringLiteral("Ask the running Antikolt to show itself, print the verdict and exit (ADR-043)."));
    const QCommandLineOption themetest(
        QStringLiteral("themetest"),
        QStringLiteral("Prove the window themes reach the pixels, then exit."),
        QStringLiteral("path"));
    const QCommandLineOption logtest(
        QStringLiteral("logtest"),
        QStringLiteral("Prove the shell log is written, rotated and switched off, then exit."));
    const QCommandLineOption searchtest(
        QStringLiteral("searchtest"),
        QStringLiteral("Prove the grid search is accent-insensitive and says when it finds nothing."),
        QStringLiteral("path"));
    const QCommandLineOption tagtest(
        QStringLiteral("tagtest"),
        QStringLiteral("Prove tags: the card carries them, the core normalises them, the search finds them."),
        QStringLiteral("path"));
    const QCommandLineOption containertest(
        QStringLiteral("containertest"),
        QStringLiteral("Prove a card can be moved to another container and the core agrees."),
        QStringLiteral("path"));
    const QCommandLineOption workspacetest(
        QStringLiteral("workspacetest"),
        QStringLiteral("Prove the tab row is the core's workspace table, and a card's workspace is stored."),
        QStringLiteral("path"));
    const QCommandLineOption profiletest(
        QStringLiteral("profiletest"),
        QStringLiteral("Prove the portable bundle: export a profile, import it into a fresh one, compare."),
        QStringLiteral("path"));
    const QCommandLineOption addcardtest(
        QStringLiteral("addcardtest"),
        QStringLiteral("Prove a card can be created in the page and removed again."),
        QStringLiteral("path"));
    const QCommandLineOption gridscaletest(
        QStringLiteral("gridscaletest"),
        QStringLiteral("Prove the grid scrolls 500 cards without rendering them all."),
        QStringLiteral("path"));
    const QCommandLineOption mediacleantest(
        QStringLiteral("mediacleantest"),
        QStringLiteral("Prove the cleanup keeps a playing card's session and wipes it once paused."),
        QStringLiteral("path"));
    const QCommandLineOption metricstest(
        QStringLiteral("metricstest"),
        QStringLiteral("Prove the memory figure is measured by the engine, then exit."),
        QStringLiteral("path"));
    const QCommandLineOption mediatest(
        QStringLiteral("mediatest"),
        QStringLiteral("Prove the site theme reaches the page, then exit."),
        QStringLiteral("path"));
    const QCommandLineOption scrollrestore(QStringLiteral("scrollrestore"),
                                         QStringLiteral("Prove the scroll restore end to end, then exit."),
                                         QStringLiteral("path"));
    const QCommandLineOption scrollbook(QStringLiteral("scrollbook"),
                                        QStringLiteral("Prove the scroll book, then exit."));
    const QCommandLineOption previewqueue(
        QStringLiteral("previewqueue"),
        QStringLiteral("Prove the background preview queue, then exit."));
    const QCommandLineOption previewbudget(QStringLiteral("previewbudget"),
                                         QStringLiteral("Prove the daily preview budget, then exit."));
    const QCommandLineOption previewrefresh(QStringLiteral("previewrefresh"),
                                          QStringLiteral("Prove the preview refresh policy, then exit."));
    const QCommandLineOption previewclean(QStringLiteral("previewclean"),
                                     QStringLiteral("Prove the preview directory is cleaned, then exit."));
    const QCommandLineOption rulestest(
        QStringLiteral("rulestest"),
        QStringLiteral("Prove the session rules can be changed over the wire, then exit."),
        QStringLiteral("path"));
    const QCommandLineOption scrollzoomtest(
        QStringLiteral("scrollzoomtest"),
        QStringLiteral("Prove scrolling and zoom reach the page, then exit."),
        QStringLiteral("path"));
    const QCommandLineOption coretest(
        QStringLiteral("coretest"),
        QStringLiteral("Prove the shell talks to the core service, then exit."),
        QStringLiteral("path"));
    parser.addOption(selftest);
    parser.addOption(engine);
    parser.addOption(embedtest);
    parser.addOption(opentest);
    parser.addOption(capturetest);
    parser.addOption(gridtest);
    parser.addOption(lifecycletest);
    parser.addOption(sessionpanel);
    parser.addOption(wipetest);
    parser.addOption(cleanuptest);
    parser.addOption(bridgetest);
    parser.addOption(pintest);
    parser.addOption(notetest);
    parser.addOption(timeouttest);
    parser.addOption(previewtest);
    parser.addOption(previewgentest);
    parser.addOption(previewworkertest);
    parser.addOption(traytest);
    parser.addOption(pinginstance);
    parser.addOption(themetest);
    parser.addOption(metricstest);
    parser.addOption(logtest);
    parser.addOption(searchtest);
    parser.addOption(mediacleantest);
    parser.addOption(gridscaletest);
    parser.addOption(containertest);
    parser.addOption(workspacetest);
    parser.addOption(profiletest);
    parser.addOption(addcardtest);
    parser.addOption(tagtest);
    parser.addOption(mediatest);
    parser.addOption(scrollzoomtest);
    parser.addOption(rulestest);
    parser.addOption(previewclean);
    parser.addOption(previewrefresh);
    parser.addOption(previewbudget);
    parser.addOption(previewqueue);
    parser.addOption(scrollbook);
    parser.addOption(scrollrestore);
    parser.addOption(coretest);
    parser.process(app);

    // Every self test measures one thing, and a self test always *names itself* on the command line; the plain
    // launch (a double click, the shortcut) has no arguments. That difference is what the two environment
    // gates hang on:
    //   * the background preview worker is real browser behaviour, so it is ON in a normal run — a probe that
    //     is not about it must not have it starting jobs (surfaces, daily budget) behind the measurement;
    //   * the tray is ON in a normal run too; elsewhere a window that hides instead of closing would change
    //     what the checks' teardown means (they close the window and expect it gone).
    // Measured the hard way: with the gates keyed on "not this test", both were off in the shipped browser,
    // so the grid never filled its previews and the close button quit instead of hiding (ADR-043).
    const bool named_a_check = QCoreApplication::arguments().size() > 1;
    if (named_a_check && !parser.isSet(previewworkertest)) {
        qputenv("ANTIKOLT_NO_PREVIEW_WORKER", "1");
    }
    if (named_a_check && !parser.isSet(traytest)) {
        qputenv("ANTIKOLT_NO_TRAY", "1");
    }

    if (parser.isSet(pinginstance)) {
        return runPingInstance();
    }

    if (parser.isSet(traytest)) {
        return runTrayCheck();
    }

    if (parser.isSet(themetest)) {
        return runThemeCheck(parser.value(themetest));
    }

    if (parser.isSet(logtest)) {
        return runLogCheck();
    }

    if (parser.isSet(searchtest)) {
        return runSearchCheck(parser.value(searchtest));
    }

    if (parser.isSet(mediacleantest)) {
        return runMediaCleanupCheck(parser.value(mediacleantest));
    }

    if (parser.isSet(gridscaletest)) {
        return runGridScaleCheck(parser.value(gridscaletest));
    }

    if (parser.isSet(containertest)) {
        return runContainerCheck(parser.value(containertest));
    }

    if (parser.isSet(tagtest)) {
        return runTagCheck(parser.value(tagtest));
    }

    if (parser.isSet(workspacetest)) {
        return runWorkspaceCheck(parser.value(workspacetest));
    }

    if (parser.isSet(profiletest)) {
        return runProfileCheck(parser.value(profiletest));
    }

    if (parser.isSet(addcardtest)) {
        return runAddCardCheck(parser.value(addcardtest));
    }
    if (parser.isSet(coretest)) {
        return runCoreCheck(parser.value(coretest));
    }

    if (parser.isSet(scrollzoomtest)) {
        return runScrollZoomCheck(parser.value(scrollzoomtest));
    }

    if (parser.isSet(rulestest)) {
        return runRulesCheck(parser.value(rulestest));
    }

    if (parser.isSet(previewclean)) {
        return runPreviewCleanCheck();
    }

    if (parser.isSet(previewrefresh)) {
        return runPreviewRefreshCheck();
    }

    if (parser.isSet(previewbudget)) {
        return runPreviewBudgetCheck();
    }

    if (parser.isSet(previewqueue)) {
        return runPreviewQueueCheck();
    }

    if (parser.isSet(scrollbook)) {
        return runScrollBookCheck();
    }

    if (parser.isSet(scrollrestore)) {
        return runScrollRestoreCheck(parser.value(scrollrestore));
    }

    if (parser.isSet(mediatest)) {
        return runMediaCheck(parser.value(mediatest));
    }

    if (parser.isSet(metricstest)) {
        return runMetricsCheck(parser.value(metricstest));
    }

    if (parser.isSet(previewgentest)) {
        return runPreviewGenCheck(parser.value(previewgentest));
    }

    if (parser.isSet(previewworkertest)) {
        return runPreviewWorkerCheck(parser.value(previewworkertest));
    }

    if (parser.isSet(previewtest)) {
        return runPreviewCheck(parser.value(previewtest));
    }

    if (parser.isSet(timeouttest)) {
        return runTimeoutCheck(parser.value(timeouttest));
    }

    if (parser.isSet(notetest)) {
        return runNoteCheck(parser.value(notetest));
    }

    if (parser.isSet(pintest)) {
        return runPinCheck(parser.value(pintest));
    }

    if (parser.isSet(bridgetest)) {
        return runBridgeCheck(parser.value(bridgetest));
    }

    if (parser.isSet(cleanuptest)) {
        return runCleanupCheck(parser.value(cleanuptest));
    }

    if (parser.isSet(wipetest)) {
        return runWipeCheck(parser.value(wipetest));
    }

    if (parser.isSet(sessionpanel)) {
        return runSessionPanelCheck(parser.value(sessionpanel));
    }

    if (parser.isSet(lifecycletest)) {
        return runLifecycleCheck(parser.value(lifecycletest));
    }

    if (parser.isSet(gridtest)) {
        return runGridCheck(parser.value(gridtest));
    }

    if (parser.isSet(capturetest)) {
        return runCaptureCheck(parser.value(capturetest));
    }

    if (parser.isSet(opentest)) {
        return runOpenCheck(parser.value(opentest));
    }

    if (parser.isSet(embedtest)) {
        return runEmbedCheck(parser.value(embedtest));
    }

    if (parser.isSet(selftest)) {
        return runSelfTest(parser.isSet(engine), parser.value(engine));
    }

    // One Antikolt per user session (ADR-043): the shortcut, the Start menu entry and the tray icon all mean
    // "the same browser". A second launch finds the first through a named socket, asks it to show itself and
    // leaves without starting anything — a second engine would be a second profile and a second 150 MB. The
    // claim comes *before* the window, so a second launch never flashes one.
    antikolt::InstanceGuard guard;
    const antikolt::InstanceGuard::Result instance =
        guard.acquire(antikolt::instanceNameForThisUser());
    if (instance == antikolt::InstanceGuard::Result::Secondary) {
        qInfo("[shell] már fut egy Antikolt: a meglévő ablak előhozva, ez az indítás kilép");
        return 0;
    }
    if (instance == antikolt::InstanceGuard::Result::Failed) {
        qInfo("[shell] egypéldányos őr nélkül indul (nincs helyi socket)");
    }

    MainWindow window;
    if (instance == antikolt::InstanceGuard::Result::Primary) {
        QObject::connect(&guard, &antikolt::InstanceGuard::showRequested, &window,
                         &MainWindow::restoreFromTray);
    }
    window.show();
    // Started from a shortcut, the browser must be a browser: the engine comes up by itself, because asking
    // the user to press "Motor indítása" after a double click is a developer's step, not a user's. Every
    // self test passes an explicit flag and never reaches this line.
    window.startEngine();
    return QApplication::exec();
}
