// SPDX-License-Identifier: MPL-2.0
//
// Entry point of the Antikolt shell.
//
// `--selftest` exists so that CI (and the first F0 spike) can prove the UI stack initialises without
// showing a window: it prints the Qt version, the platform plugin, the screen geometry and checks
// that every documented theme produces a stylesheet. A build that cannot do this is not a build.

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QScreen>
#include <QSysInfo>
#include <QTextStream>

#include "main_window.h"

namespace {

int runSelfTest() {
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
    parser.addOption(selftest);
    parser.process(app);

    if (parser.isSet(selftest)) {
        return runSelfTest();
    }

    MainWindow window;
    window.show();
    return QApplication::exec();
}
