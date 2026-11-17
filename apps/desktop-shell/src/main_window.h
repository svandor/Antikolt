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
#include <QSet>
#include <QString>
#include <QStringList>

class QLabel;
class QTimer;
class QDockWidget;
class QTableWidget;
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

/// A queued session cleanup: which container the row belongs to and which storages its plan touched.
///
/// The plan is the core's answer, carried here so the wipe uses the storage list it was decided with
/// (ADR-033) — the shell never re-derives it.
struct PendingWipe {
    QString container;
    QStringList storages;
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

    /// Show the session ledger panel (`docs/HU/10-MUNKAMENET.md` §5) and fill it from the core.
    ///
    /// The rows are the core's answer, not the shell's bookkeeping: `session_list` for the ledger and
    /// `session_due` for what the 24-hour policy would do right now (ADR-033).
    void showSessions();
    /// What the panel is showing, one line per row — the witness a self test reads.
    QString sessionPanelSummary() const;
    /// How many rows the panel holds (0 before the first refresh).
    int sessionPanelRowCount() const;
    /// Run the 24-hour policy now: what the core reports as due is cleared through the engine.
    ///
    /// A due origin whose card is on screen is wiped immediately; the others are remembered and wiped
    /// when their card is next opened, because this shell has exactly one live surface (I1) and a
    /// cleanup must not steal the page the user is reading.
    void cleanupSessions();

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
    void handleBridgeCall(qint64 surface, const QString& method, const QJsonObject& args,
                          qint64 request);
    /// Answer a page request (`request > 0`). Every question gets exactly one answer, always.
    void replyToPage(qint64 request, const QJsonObject& json, const QString& error = {});
    /// Push the core's card list into the home page (`window.antikoltAcceptCards`).
    ///
    /// The push is shell-initiated on purpose: the page cannot query the shell yet — the bridge is
    /// one-way until the persistent `cefQuery` lands — and a grid showing the cards the core owns is
    /// worth more than a grid showing the page's own sample data. A missing core is not an error
    /// here: the page then keeps its samples, and the status bar says why.
    void pushCardsToGrid(qint64 surface);
    /// Ask the core for the ledger and fill the panel with it.
    void refreshSessions();
    /// Produce one card's preview image, once, and remember its name on the record (ADR-035).
    ///
    /// The card is already on screen when this runs, so the picture is the real page — the engine's own
    /// compositor writes it (`capture`), the shell moves it into the profile's `previews` directory, and the
    /// core stores the name. A card that already has a preview keeps it: this is not a screenshot loop.
    void producePreview(qint64 surface);
    /// (Re)start the hibernation clock for the live card.
    void armIdleTimer();
    /// Any sign of the user: restarts the hibernation clock.
    void noteActivity();
    /// The clock ran out: hibernate the card, unless it must stay alive (playing media).
    void onIdleTimeout();
    void setStatus(const QString& text);

    bool eventFilter(QObject* watched, QEvent* event) override;

    LiveCard* card_ = nullptr;
    QLabel* status_ = nullptr;
    QString currentTheme_;
    EngineClient* engine_ = nullptr;
    /// The core owns the data; the shell only asks it.
    CoreClient* core_ = nullptr;
    qint64 surface_ = 0;
    /// The surface that shows the home page, or 0 when a real card is in front. Only this surface
    /// receives the card list.
    qint64 home_surface_ = 0;
    /// The hibernation clock (docs/HU/02 §4.2). Ten minutes of no user activity puts the live card
    /// back on disk, which is the whole memory promise in one timer.
    QTimer* idle_ = nullptr;
    /// True while the card in front is being hibernated, so a late reply cannot re-arm the clock.
    bool hibernating_ = false;
    /// Set when the idle timeout hibernated the card: the grid comes back once the surface is gone,
    /// so the window is never left showing an empty card area.
    bool return_to_grid_ = false;
    /// Whether the card in front is pinned. A pinned card is the documented opt-out from automatic
    /// hibernation (docs/HU/02 §4.2); the flag arrives with the page's open request, because the shell
    /// does not keep the card list.
    bool live_card_pinned_ = false;
    /// The flag from the page's open request, held until the card's surface is attached: the grid's
    /// own detach happens in between and would otherwise clear it.
    bool pending_pinned_ = false;
    /// The card waiting to be shown once a surface exists.
    QString pending_url_;
    QString pending_title_;
    /// The session ledger panel and its table (created on first use).
    QDockWidget* sessions_dock_ = nullptr;
    QTableWidget* sessions_table_ = nullptr;
    /// Origins the policy marked due, with the storage list its plan touched: the wipe happens when
    /// that origin's card is opened next (or immediately, if it is already on screen).
    QHash<QString, PendingWipe> wipe_on_open_;
    /// The page requests still waiting for an answer (protocol 2). Every request is answered exactly once —
    /// `replyToPage` removes, the watchdog answers what a path forgot.
    QSet<qint64> awaiting_page_reply_;
    /// The engine's profile (`<user data>`), where `previews/` lives: the shell writes the preview images
    /// there and the engine serves them (ADR-035).
    QString engine_profile_;
    /// The card the page asked to open (the request carries its id), used to name its preview.
    QString pending_card_id_;
    /// How many previews this run has produced; the cap keeps a long browsing session from filling the
    /// disk with screenshots.
    int previews_made_ = 0;
    /// The origin of the card in front, as the engine loaded it ("" when the grid is showing).
    QString live_origin_;
};
