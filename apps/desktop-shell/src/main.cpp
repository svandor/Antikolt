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

#include "engine_client.h"
#include "live_card.h"
#include "main_window.h"

namespace {

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

                                     // Geometry is not painting: grab the card's screen area and
                                     // check that it is not a uniform rectangle. A window can sit at
                                     // exactly the right place and size and still show nothing.
                                     const QPixmap shot = card->screen()->grabWindow(card->winId());
                                     if (shot.isNull()) {
                                         out << "screenshot=FAILED\n";
                                     } else {
                                         const QImage image = shot.toImage();
                                         QString path = QDir::temp().filePath(
                                             QStringLiteral("antikolt-embed.png"));
                                         const bool saved = image.save(path, "PNG");
                                         out << "screenshot=" << (saved ? path : QStringLiteral("FAILED"))
                                             << '\n';
                                         out << "screenshot_size=" << image.width() << 'x'
                                             << image.height() << '\n';
                                         // Distinct colours prove that something was drawn, not
                                         // just a filled background.
                                         QSet<QRgb> colours;
                                         for (int y = 0; y < image.height(); y += 8) {
                                             for (int x = 0; x < image.width(); x += 8) {
                                                 colours.insert(image.pixel(x, y));
                                             }
                                         }
                                         out << "screenshot_colours=" << colours.size() << '\n';
                                         painted_ok = colours.size() > 2;

                                         // Focus is the last link: a child window in another process
                                         // does not inherit activation, so the shell sends `focus` and
                                         // the page must agree that it has the keyboard.
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
                                                         const QString value = focused
                                                                                   .value(QStringLiteral(
                                                                                       "value"))
                                                                                   .toString();
                                                         out << "page_has_focus=" << value << '\n';
                                                         focus_ok = value == QStringLiteral("true");
                                                     }
                                                     loop.quit();
                                                 });
                                         });
                                     }
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
    parser.addOption(selftest);
    parser.addOption(engine);
    parser.addOption(embedtest);
    parser.addOption(opentest);
    parser.process(app);

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
