// SPDX-License-Identifier: MPL-2.0
//
// The shell's main window.
//
// It owns the theme palette (docs/HU/08-DIZAJN.md) and the engine link: the status bar reports what
// the engine host says, and the toolbar drives it. The card surface still lives in the engine's own
// native window until cross-process embedding lands, but the protocol between the two halves is real.

#pragma once

#include <QJsonObject>
#include <QMainWindow>
#include <QString>
#include <QStringList>

class QLabel;
class EngineClient;

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
    ~MainWindow() override;

    /// The four shipped themes, in the order the toolbar shows them.
    static QStringList themeNames();

    /// The palette for a theme, or the grey (default) palette when the name is unknown.
    static ThemePalette paletteFor(const QString& theme);

    /// Apply a theme to the whole window. Returns false for an unknown name.
    bool applyTheme(const QString& theme);

    /// The stylesheet a theme produces; exposed for the self test.
    static QString styleSheetFor(const ThemePalette& palette);

    /// Where the engine host lives. Honours ANTIKOLT_ENGINE, then the build layout, then a sibling
    /// directory, so a developer build works without configuration.
    static QString engineExecutable();

    /// The home page to load in the engine (the editable card grid).
    static QString homePageUrl();

private slots:
    void startEngine();
    void loadHome();
    void freezeCard();

private:
    void buildToolBar();
    void buildPlaceholder();
    void setStatus(const QString& text);

    QLabel* placeholder_ = nullptr;
    QLabel* status_ = nullptr;
    QString currentTheme_;
    EngineClient* engine_ = nullptr;
    qint64 surface_ = 0;
};
