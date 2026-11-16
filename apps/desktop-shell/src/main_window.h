// SPDX-License-Identifier: MPL-2.0
//
// The shell's main window.
//
// Today it shows a placeholder where the home page will be hosted, and it owns the theme palette so
// the four documented themes (docs/HU/08-DIZAJN.md) are a single source of truth from the first
// commit. The card surface arrives with the engine host.

#pragma once

#include <QMainWindow>
#include <QString>
#include <QStringList>

class QLabel;

/// One theme's colour tokens, mirroring docs/HU/08-DIZAJN.md §1.
struct ThemePalette {
    QString background;
    QString surface;
    QString surfaceRaised;
    QString text;
    QString textMuted;
    QString border;
    QString accent;
    QString ok;
    QString danger;
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    /// The four shipped themes, in the order the toolbar shows them.
    static QStringList themeNames();

    /// The palette for a theme, or the grey (default) palette when the name is unknown.
    static ThemePalette paletteFor(const QString& theme);

    /// Apply a theme to the whole window. Returns false for an unknown name.
    bool applyTheme(const QString& theme);

    /// The stylesheet a theme produces; exposed for the self test.
    static QString styleSheetFor(const ThemePalette& palette);

private:
    void buildToolBar();
    void buildPlaceholder();

    QLabel* placeholder_ = nullptr;
    QString currentTheme_;
};
