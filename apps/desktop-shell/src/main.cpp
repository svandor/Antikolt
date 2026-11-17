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
#include <QEventLoop>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>
#include <QPixmap>
#include <QRegularExpression>
#include <QScreen>
#include <QSet>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "core_client.h"
#include "engine_client.h"
#include "live_card.h"
#include "main_window.h"

namespace {

/// Qt's own messages — and the engine's forwarded stderr, which arrives through `qInfo` — go to
/// stderr instead of the platform debug output.
///
/// A console build must be readable from the terminal, and a swallowed engine log is exactly how a
/// page-side failure stayed invisible for two slices: the engine said `console level=4 … TypeError`
/// and the shell threw it away. Everything the engine reports is now visible where the shell runs.
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
    if (type == QtFatalMsg) {
        std::abort();
    }
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
                         QTimer::singleShot(500, &loop, [&] {
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
                                 [&](const QJsonObject& result, const QString& error) {
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
        client.destroySurface(surface, [](const QJsonObject&, const QString&) {});
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
        client.destroySurface(surface, [](const QJsonObject&, const QString&) {});
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
                             QTimer::singleShot(500, &loop, [&, id] {
                                 client->evaluate(
                                     id,
                                     QStringLiteral(
                                         "document.querySelector('#viewport antikolt-card').click();"
                                         " 'clicked'"),
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
    window.close();
    out.flush();
    return closed && back ? 0 : 1;
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
        client->evaluate(
            home_surface,
            QStringLiteral("document.querySelector('#viewport antikolt-card').click(); 'clicked'"),
            [&](const QJsonObject& reply, const QString& error) {
                out << "card_click="
                    << (error.isEmpty() ? reply.value(QStringLiteral("value")).toString()
                                        : QStringLiteral("FAILED ") + error)
                    << '\n';
            });
        QTimer::singleShot(30000, &click_loop, &QEventLoop::quit);
        click_loop.exec();
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
                loop.quit();
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
    if (!ready || card_count <= 0 || !stamped) {
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

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    // Before anything can log: see ForwardQtMessages.
    qInstallMessageHandler(ForwardQtMessages);
    QApplication::setApplicationName(QStringLiteral("Antikolt"));
    QApplication::setApplicationVersion(QStringLiteral(ANTIKOLT_VERSION));
    QApplication::setOrganizationName(QStringLiteral("Antikolt Project"));
    QApplication::setOrganizationDomain(QStringLiteral("antikolt.dev"));

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
    parser.addOption(coretest);
    parser.process(app);

    if (parser.isSet(coretest)) {
        return runCoreCheck(parser.value(coretest));
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

    MainWindow window;
    window.show();
    return QApplication::exec();
}
