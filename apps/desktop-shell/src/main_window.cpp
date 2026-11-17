// SPDX-License-Identifier: MPL-2.0

#include "main_window.h"

#include <QActionGroup>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QLabel>
#include <QStandardPaths>
#include <QStatusBar>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

#include "engine_client.h"
#include "live_card.h"

namespace {

// Keep these in sync with docs/HU/08-DIZAJN.md §1. A mismatch is a review finding, not a taste
// question: the documentation is the specification.
const ThemePalette kLight{
    "#F7F7F5", "#FFFFFF", "#FFFFFF", "#1A1A18", "#5F5F5A", "#E1E1DC", "#2F6FEB", "#1E7D4F", "#C23B32"};
const ThemePalette kGrey{
    "#2E3134", "#35383C", "#3C4045", "#EDEDEA", "#A8AAAD", "#44484D", "#7FB0FF", "#7BD8A5", "#FF8A80"};
const ThemePalette kDark{
    "#1C1D1F", "#232527", "#2A2C2F", "#E9E9E7", "#9A9C9F", "#33363A", "#6EA8FF", "#6FD3A0", "#F27A72"};
const ThemePalette kAmoled{
    "#000000", "#0A0A0A", "#121212", "#EDEDED", "#9A9A9A", "#1E1E1E", "#8AB8FF", "#7BD8A5", "#FF8A80"};

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Antikolt"));
    resize(1280, 800);
    setMinimumSize(760, 480);

    buildToolBar();
    buildCardArea();

    status_ = new QLabel(this);
    statusBar()->addWidget(status_);

    engine_ = new EngineClient(this);
    connect(engine_, &EngineClient::failed, this, [this](const QString& message) {
        setStatus(QStringLiteral("motor hiba: %1").arg(message));
    });
    connect(engine_, &EngineClient::engineReady, this, [this](const QJsonObject& info) {
        setStatus(QStringLiteral("motor: %1 %2 · képességek: %3")
                      .arg(info.value(QStringLiteral("name")).toString(),
                           info.value(QStringLiteral("version")).toString())
                      .arg(info.value(QStringLiteral("capabilities")).toArray().size()));
        ensureSurface();
    });
    connect(engine_, &EngineClient::loadFinished, this,
            [this](qint64 surface, const QString& url, int httpStatus) {
                setStatus(QStringLiteral("betöltve (#%1, %2): %3")
                              .arg(surface)
                              .arg(httpStatus)
                              .arg(url));
            });
    connect(engine_, &EngineClient::loadFailed, this,
            [this](qint64 surface, const QString& url, const QString& reason) {
                setStatus(QStringLiteral("betöltési hiba (#%1): %2 — %3").arg(surface).arg(url, reason));
            });

    // The card owns the surface lifecycle locally; the window mirrors it for the status bar.
    connect(card_, &LiveCard::surfaceAttached, this, [this](qint64 surface) {
        surface_ = surface;
        setStatus(QStringLiteral("kártya felület: %1 (%2×%3 fizikai pixel)")
                      .arg(surface)
                      .arg(card_->physicalSize().width())
                      .arg(card_->physicalSize().height()));
        engine_->navigate(surface, homePageUrl());
    });
    connect(card_, &LiveCard::surfaceDetached, this, [this](qint64 surface) {
        surface_ = 0;
        setStatus(QStringLiteral("kártya #%1 lemezre mentve · memória felszabadítva").arg(surface));
    });

    setStatus(QStringLiteral("motor: nincs elindítva · kártyák: 0"));
    applyTheme(QStringLiteral("grey"));
}

MainWindow::~MainWindow() {
    if (engine_ != nullptr) {
        engine_->shutdown();
    }
}

QStringList MainWindow::themeNames() {
    return {QStringLiteral("light"), QStringLiteral("grey"), QStringLiteral("dark"),
            QStringLiteral("amoled")};
}

ThemePalette MainWindow::paletteFor(const QString& theme) {
    if (theme == QStringLiteral("light")) {
        return kLight;
    }
    if (theme == QStringLiteral("dark")) {
        return kDark;
    }
    if (theme == QStringLiteral("amoled")) {
        return kAmoled;
    }
    // Grey is the default (ADR-012): the least tiring theme day and night.
    return kGrey;
}

QString MainWindow::styleSheetFor(const ThemePalette& palette) {
    return QStringLiteral(R"(
        QMainWindow, QWidget#antikoltRoot { background: %1; color: %4; }
        QLabel#antikoltHint { color: %5; }
        QToolBar {
            background: %1;
            border: 0;
            border-bottom: 1px solid %6;
            padding: 4px 8px;
            spacing: 4px;
        }
        QToolButton {
            color: %4;
            background: %2;
            border: 1px solid %6;
            border-radius: 8px;
            padding: 4px 10px;
        }
        QToolButton:hover { background: %3; }
        QToolButton:checked { background: %3; border-color: %7; }
        QStatusBar { background: %1; color: %5; border-top: 1px solid %6; }
    )")
        .arg(palette.background, palette.surface, palette.surfaceRaised, palette.text,
             palette.textMuted, palette.border, palette.accent);
}

bool MainWindow::applyTheme(const QString& theme) {
    if (!themeNames().contains(theme)) {
        return false;
    }
    // Named `colors`, not `palette`: a local called `palette` would shadow QWidget::palette().
    const ThemePalette colors = paletteFor(theme);
    setStyleSheet(styleSheetFor(colors));

    QPalette windowPalette = palette();
    windowPalette.setColor(QPalette::Window, QColor(colors.background));
    windowPalette.setColor(QPalette::WindowText, QColor(colors.text));
    windowPalette.setColor(QPalette::Base, QColor(colors.surface));
    windowPalette.setColor(QPalette::Text, QColor(colors.text));
    windowPalette.setColor(QPalette::Mid, QColor(colors.textMuted));
    windowPalette.setColor(QPalette::Highlight, QColor(colors.accent));
    setPalette(windowPalette);

    // The card paints itself (it is a native window), so it needs the palette too.
    if (card_ != nullptr) {
        card_->setPalette(windowPalette);
        card_->update();
    }

    currentTheme_ = theme;
    return true;
}

QString MainWindow::engineExecutable() {
    const QString fromEnvironment = qEnvironmentVariable("ANTIKOLT_ENGINE");
    if (!fromEnvironment.isEmpty() && QFileInfo::exists(fromEnvironment)) {
        return fromEnvironment;
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/antikolt-engine-host.exe"),
        appDir + QStringLiteral("/../engine-host/antikolt-engine-host.exe"),
        appDir + QStringLiteral("/apps/engine-host/antikolt-engine-host.exe"),
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QDir::cleanPath(candidate);
        }
    }
    return {};
}

QString MainWindow::homePageUrl() {
    const QString fromEnvironment = qEnvironmentVariable("ANTIKOLT_HOME");
    if (!fromEnvironment.isEmpty()) {
        return QUrl::fromLocalFile(fromEnvironment).toString();
    }
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/../home/index.html"),
        appDir + QStringLiteral("/../../../home/index.html"),
        QDir::currentPath() + QStringLiteral("/home/index.html"),
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QUrl::fromLocalFile(QDir::cleanPath(candidate)).toString();
        }
    }
    return QStringLiteral("about:blank");
}

void MainWindow::startEngine() {
    const QString enginePath = engineExecutable();
    if (enginePath.isEmpty()) {
        setStatus(QStringLiteral("nem találom a motorházat (állítsd be az ANTIKOLT_ENGINE változót)"));
        return;
    }

    const QString profile =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/engine");
    QDir().mkpath(profile);

    if (!engine_->start(enginePath, profile)) {
        return;
    }
    setStatus(QStringLiteral("motor indítása: %1").arg(enginePath));
    engine_->handshake();
}

void MainWindow::ensureSurface() {
    if (!engine_->isRunning()) {
        return;
    }
    if (card_->hasSurface()) {
        engine_->navigate(card_->surface(), homePageUrl());
        return;
    }
    if (!card_->attach(engine_, QStringLiteral("persist:home"))) {
        setStatus(QStringLiteral("a kártya nem tudott felületet kérni a motortól"));
    }
}

void MainWindow::loadHome() {
    if (!engine_->isRunning()) {
        setStatus(QStringLiteral("a motor még nem fut — nyomd meg a „Motor indítása” gombot"));
        return;
    }
    ensureSurface();
}

void MainWindow::freezeCard() {
    if (!engine_->isRunning() || !card_->hasSurface()) {
        setStatus(QStringLiteral("nincs mit fagyasztani"));
        return;
    }
    const qint64 surface = card_->surface();
    engine_->snapshot(surface, [this, surface](const QJsonObject& result, const QString& error) {
        if (!error.isEmpty()) {
            setStatus(QStringLiteral("fagyasztás hiba: %1").arg(error));
            return;
        }
        setStatus(QStringLiteral("pillanatkép: %1 bájt · állapot %2 bájt · motor leállítása")
                      .arg(result.value(QStringLiteral("bytes")).toInt())
                      .arg(result.value(QStringLiteral("state_bytes")).toInt()));
        // Order matters: the state is on disk before the surface goes away.
        card_->detach();
        engine_->destroySurface(surface);
    });
}

void MainWindow::buildToolBar() {
    auto* toolbar = addToolBar(QStringLiteral("Fő"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);

    toolbar->addAction(QStringLiteral("Motor indítása"), this, &MainWindow::startEngine);
    toolbar->addAction(QStringLiteral("⌂ Kezdőlap"), this, &MainWindow::loadHome);
    toolbar->addAction(QStringLiteral("❄ Fagyasztás"), this, &MainWindow::freezeCard);

    auto* spacer = new QWidget(this);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

    // Theme switching is part of the identity (light / grey / dark), not a settings-page afterthought.
    auto* themeGroup = new QActionGroup(this);
    themeGroup->setExclusive(true);
    for (const QString& name : themeNames()) {
        auto* action = toolbar->addAction(name);
        action->setCheckable(true);
        action->setChecked(name == QStringLiteral("grey"));
        themeGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, name] { applyTheme(name); });
    }
}

void MainWindow::buildCardArea() {
    auto* root = new QWidget(this);
    root->setObjectName(QStringLiteral("antikoltRoot"));

    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(16, 16, 16, 8);
    layout->setSpacing(8);

    // The live card: a native hole in the Qt layout that the engine paints into.
    card_ = new LiveCard(root);
    card_->setObjectName(QStringLiteral("antikoltCard"));

    auto* hint = new QLabel(root);
    hint->setObjectName(QStringLiteral("antikoltHint"));
    hint->setText(QStringLiteral(
        "A motor külön folyamatban fut; a kártya natív ablaka ebbe az ablakba van ágyazva, így a "
        "kép és a bemenet az operációs rendszeren megy át, nem a protokollon."));

    layout->addWidget(card_, 1);
    layout->addWidget(hint, 0);

    setCentralWidget(root);
}

void MainWindow::setStatus(const QString& text) {
    if (status_ != nullptr) {
        status_->setText(text);
    }
    statusBar()->showMessage(text);
}
