// SPDX-License-Identifier: MPL-2.0

#include "main_window.h"

#include <QActionGroup>
#include <QFont>
#include <QLabel>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>

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
    buildPlaceholder();

    statusBar()->showMessage(QStringLiteral("motor: nincs betöltve · kártyák: 0 · memória: —"));

    applyTheme(QStringLiteral("grey"));
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
        QLabel#antikoltPlaceholder {
            background: %2;
            color: %4;
            border: 1px solid %6;
            border-radius: 14px;
            padding: 32px;
        }
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
    windowPalette.setColor(QPalette::Highlight, QColor(colors.accent));
    setPalette(windowPalette);

    currentTheme_ = theme;
    statusBar()->showMessage(
        QStringLiteral("téma: %1 · motor: nincs betöltve · kártyák: 0 · memória: —").arg(theme));
    return true;
}

void MainWindow::buildToolBar() {
    auto* toolbar = addToolBar(QStringLiteral("Fő"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);

    toolbar->addAction(QStringLiteral("⌂ Kezdőlap"));
    toolbar->addAction(QStringLiteral("←"));
    toolbar->addAction(QStringLiteral("→"));
    toolbar->addAction(QStringLiteral("⟳"));

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

void MainWindow::buildPlaceholder() {
    auto* root = new QWidget(this);
    root->setObjectName(QStringLiteral("antikoltRoot"));

    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);

    placeholder_ = new QLabel(root);
    placeholder_->setObjectName(QStringLiteral("antikoltPlaceholder"));
    placeholder_->setAlignment(Qt::AlignCenter);
    placeholder_->setWordWrap(true);
    placeholder_->setText(QStringLiteral(
        "A kezdőlap itt jelenik meg.\n\n"
        "Ez a felület a héj (Qt) váza: a kártyarács és az oldalak a motorházban (CEF) futnak, "
        "külön folyamatban, hogy a motor bármikor leállítható legyen (ADR-004).\n\n"
        "A statikus előnézet megnyitható a repóból: home/index.html"));

    auto* hint = new QLabel(root);
    hint->setObjectName(QStringLiteral("antikoltHint"));
    hint->setText(QStringLiteral(
        "Következő lépés: a motorház prototípusa (CEF) és az F0 memória-mérés a három "
        "referenciaoldalon."));

    layout->addWidget(placeholder_, 1);
    layout->addWidget(hint, 0);

    setCentralWidget(root);
}
