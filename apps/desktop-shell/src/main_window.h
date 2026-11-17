// SPDX-License-Identifier: MPL-2.0
//
// The shell's main window.
//
// It owns the theme palette (docs/HU/08-DIZAJN.md), the one live card and the engine link. The card
// is a native widget whose window handle the engine host embeds Chromium into (ADR-004): the shell
// keeps one UI process and one engine process, and the pixels never travel through the protocol.

#pragma once

#include <QJsonObject>
#include <QMainWindow>
#include <QString>
#include <QStringList>

class QLabel;
class EngineClient;
class CoreClient;
class LiveCard;

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

    /// Where the core service lives. Honours ANTIKOLT_CORE, then the Cargo build layout.
    static QString coreExecutable();

    /// The home page to load in the engine (the editable card grid).
    static QString homePageUrl();

    /// The live card, exposed so the embedding self test can drive it.
    LiveCard* liveCard() const { return card_; }

    /// The engine link, exposed for the same reason.
    EngineClient* engine() const { return engine_; }

public slots:
    void startEngine();
    void loadHome();
    void freezeCard();
    /// Show one card: hibernate whatever is live, then open this URL as the live card.
    void openCard(const QString& url, const QString& title);

private:
    void buildToolBar();
    void buildCardArea();
    void ensureSurface();
    void closeLiveCard();
    void handleBridgeCall(qint64 surface, const QString& method, const QJsonObject& args);
    void setStatus(const QString& text);

    LiveCard* card_ = nullptr;
    QLabel* status_ = nullptr;
    QString currentTheme_;
    EngineClient* engine_ = nullptr;
    /// The core owns the data; the shell only asks it.
    CoreClient* core_ = nullptr;
    qint64 surface_ = 0;
    /// The card waiting to be shown once a surface exists.
    QString pending_url_;
    QString pending_title_;
};
