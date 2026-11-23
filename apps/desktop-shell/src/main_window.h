// SPDX-License-Identifier: MPL-2.0
//
// The shell's main window.
//
// It owns the theme palette (docs/HU/08-DIZAJN.md), the one live card and the engine link. The card
// is a native widget whose window handle the engine host embeds Chromium into (ADR-004): the shell
// keeps one UI process and one engine process, and the pixels never travel through the protocol.

#pragma once

#include <QJsonObject>
#include <QHash>
#include <QMainWindow>
#include <QPixmap>
#include <QSet>
#include <QString>

#include <functional>
#include <QStringList>

#include "previews.h"
#include "i18n.h"

class QLabel;
class QLineEdit;
class QTimer;
class QStackedLayout;
class QDockWidget;
class QTableWidget;
class QSpinBox;
class QCheckBox;
class QComboBox;
class QSystemTrayIcon;
class QMenu;
class QCloseEvent;
class QToolButton;
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

/// The memory governor's pressure class (ADR-020) from the measured working set and the profile's warn/hard bands.
/// Boundary-inclusive, mirroring the core's `ProfileParams::pressure`: `hard` at the hard band, `warn` at the warn
/// band, `normal` below. An unset (zero) band never fires, so the two bands are independently optional.
QString memoryPressureName(qint64 used_bytes, qint64 warn_bytes, qint64 hard_bytes);

/// Whether the machine is on mains power (F6, ADR-020 `auto`). `ANTIKOLT_AC_POWER=1/0` overrides for a self test.
bool onAcPower();

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

    /// The home page to load in the engine (the editable card grid). A user-authored `home.html` in the core's
    /// data directory overrides the built-in grid (item 3: editable home, storage + serving).
    static QString homePageUrl();

    /// Where the core keeps its data. Honours `ANTIKOLT_CORE_DATA`, then the platform app-data location.
    static QString coreDataDir();

    /// The live card, exposed so the embedding self test can drive it.
    LiveCard* liveCard() const { return card_; }

    /// The engine link, exposed for the same reason.
    EngineClient* engine() const { return engine_; }

    /// The core link, exposed so the preferences self test can drive `user_prefs_set`.
    CoreClient* core() const { return core_; }

    /// Show the session ledger panel (`docs/HU/10-MUNKAMENET.md` §5) and fill it from the core.
    ///
    /// The rows are the core's answer, not the shell's bookkeeping: `session_list` for the ledger and
    /// `session_due` for what the 24-hour policy would do right now (ADR-033).
    void showSessions();
    /// Refresh the cached user preferences from the core (live-card cap, hibernation timings and the memory
    /// budget, F6). The budget is a profile setting that changes rarely, so it is cached, not re-read per query.
    void refreshUserPrefs();
    /// Apply one derived profile's params to the cached governor state (bands, video cap, preview refresh, engine
    /// lifetime). Shared by `refreshUserPrefs` (the stored kind) and the `auto` re-resolution (F6, ADR-020).
    void applyProfileParams(const QJsonObject& profile);
    /// Import bookmarks from a Netscape-format HTML file into the card store (M7): each anchor becomes a card in
    /// the default workspace; URLs already present are skipped, never duplicated.
    void importBookmarksFromPath(const QString& path);
    /// Show the user-preferences dock (ADR-072/074) and fill it from the cached core values.
    void showSettings();
    /// Send the dock's values to the core (`user_prefs_set`); the core validates, the shell re-reads.
    void saveUserPrefs();
    /// Remove the user-authored `home.html` from the core data directory and reload the built-in grid (item 3,
    /// editable home: a user who set a broken page needs a way back).
    void resetCustomHome();
    /// Create a starter `home.html` (and copy the card package next to it) when the user has none yet — the first
    /// step of the editable home's editor, so the user edits a working page instead of starting from a blank file.
    void createCustomHomeTemplate();
    /// Rasterise the grid: capture the home surface to `<profile>/home-raster.png` (the image the raster home
    /// shows while the engine is stopped, F6).
    void captureGrid();
    /// Enter the raster home (F6): rasterise the grid, stop the engine, and remember the mode. `wakeFromRaster`
    /// brings the engine back.
    void enterRasterMode();
    /// Wake from the raster home: restart the engine; its welcome reloads the grid.
    void wakeFromRaster();
    /// Whether the raster home is active (the engine stopped, the grid shown as a raster). Exposed for the test.
    bool rasterMode() const { return raster_mode_; }
    /// Arm the engine-idle clock (T2): when a card goes back to disk and the profile has a shutdown threshold,
    /// the raster home activates after that threshold. Exposed so the self test can drive it directly.
    void armEngineIdleTimer();
    /// Capture each visible card's rectangle from the grid (for the raster click mapping), then call `done`.
    void captureCardPositions(std::function<void()> done = {});
    /// Map a point in raster CSS pixels to a card id ("" when nothing is under it).
    QString cardAtRasterPoint(qreal x, qreal y) const;
    /// How many card rectangles the last position capture recorded. Exposed for the test.
    int rasterCardCount() const { return raster_cards_.size(); }
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
    /// Set the live page's zoom, in percent (25..500). Remembered and re-applied on every load (ADR-038).
    void setZoomPercent(int percent);
    /// Send the panel's rules to the core (ADR-039). The core validates them; a refusal is shown, not hidden.
    void saveSessionRules();
    /// Delete preview files whose card no longer exists (ADR-035). Runs with the cleanup round.
    void cleanupPreviews();
    /// Where this browser keeps the per-card scroll positions (`scroll.json` in the profile).
    QString scrollBookPath() const;
    /// Remember the live card's offset before it is left (the grid is a place too).
    void rememberScroll(std::function<void()> then = {});
    /// The panel's rule editors as the user sees them (self test witnesses).
    int sessionPanelTtlHours() const;
    bool sessionPanelLockWipe() const;
    /// Emulate the live page's site theme: `auto`, `dark` or `light` (ADR-037).
    ///
    /// The preference is remembered and re-applied when a surface attaches or loads, because the emulation
    /// belongs to the page that is showing: a page that navigates away would otherwise forget it. Public
    /// for the same reason the panel helpers are — the self test drives the real path.
    void setPageMedia(const QString& media);

    /// Ask the engine to darken the page itself, or to stop doing it (ADR-044/ADR-045).
    ///
    /// The switch is the user's (toolbar), remembered in the settings and re-applied on every load. It is *not*
    /// tied to the window theme: the site-theme query already tells a page what the system prefers, and a page
    /// without a dark theme of its own stays light until this is asked for — which is what the user saw when
    /// the window was amoled and the article was white.
    void setPageDarkMode(const QString& mode);
    /// The page-dark mode in force: `off`, `invert`, `dim` or `warm` (self test witness).
    QString pageDarkMode() const { return page_dark_mode_; }
    /// The page-dark modes the toolbar offers, in menu order.
    static QStringList pageDarkModes();
    /// The shell's user-facing language: `hu` (authoritative, default) or `en` (i18n, ADR-078).
    QString language() const { return language_; }
    /// Switch the shell's language, persist it in the settings, and re-label the chrome in place — no restart.
    void setLanguage(const QString& language);
    /// The chrome's translation of a Hungarian source string in the current language (i18n, ADR-078).
    QString t(const QString& hu) const { return antikolt::uiText(hu, language_); }
    /// The theme button of one theme, so the self test can prove it carries an icon and a tooltip
    /// (docs/HU/08-DIZAJN.md §6: the toolbar's theme switch is icons, the words live in the tooltip).
    QToolButton* themeButton(const QString& theme) const;
    /// The toolbar's dark-pages button (icon + menu) and its icon for one mode.
    QToolButton* pageDarkButton() const { return page_dark_button_; }

    /// Start the background preview worker for one card (ADR-041). False when the guards refuse it.
    ///
    /// The worker gets its own hidden surface, loads the card's page, photographs it through the engine's
    /// compositor, writes the file into the profile and the name onto the card record. It runs only while
    /// the user is on the grid and yields the moment a card opens: the self test drives this directly, and
    /// `previewWorkerAutoEnabled()` is what keeps the grid filling itself in a real run.
    bool startPreviewWorker(const QString& cardId, const QString& url, const QString& container = {});
    /// Where the worker is — the witness a self test reads (`Idle` once a job is over).
    antikolt::PreviewWorker previewWorkerState() const { return worker_state_; }
    /// The surface the last job used. Kept after it is destroyed only so a test can prove the engine no
    /// longer knows it; nothing in the window acts on it.
    qint64 previewWorkerLastSurface() const { return worker_last_surface_; }
    /// Whether the grid fills its empty preview frames by itself.
    ///
    /// A self test drives the worker explicitly, so the automatic queue is turned off there
    /// (`ANTIKOLT_NO_PREVIEW_WORKER`) — one probe should measure one thing.
    static bool previewWorkerAutoEnabled();

    /// Whether this window puts its icon in the system tray (ADR-043).
    ///
    /// False when the platform has no tray, and when `ANTIKOLT_NO_TRAY` is set: every self test except
    /// `--traytest` turns the tray off, so closing a test window keeps meaning what it always meant.
    static bool trayEnabled();

    /// True when the icon is in the tray — which is also the reason the window survives its own close.
    bool trayAvailable() const { return tray_ != nullptr; }
    /// The tray menu's labels, in order (separators left out) — the witness a self test reads.
    QStringList trayMenuLabels() const;

signals:
    /// One worker job ended. `produced` is true only when the image reached the profile *and* the record.
    void previewWorkerFinished(const QString& cardId, bool produced);

public slots:
    void startEngine();
    void loadHome();
    void freezeCard();
    /// Show one card: hibernate whatever is live, then open this URL as the live card.
    void openCard(const QString& url, const QString& title);
    /// Bring the window back from the tray: the tray click, its "Megnyitás" item, and a second launch of
    /// the shortcut all end up here (ADR-043).
    void restoreFromTray();
    /// End the process from the tray menu. After this the tray is no longer a reason to keep running, so
    /// the window really closes.
    void quitFromTray();
    /// Export the portable profile bundle to a file the user picks in a native dialog (ADR-059).
    ///
    /// The dialog itself is native UI the harness cannot click; the export/import it triggers is the same core
    /// path `--profiletest` measures through the bridge.
    void exportProfile();
    /// Import a portable profile bundle from a file the user picks (ADR-059), then refresh the grid.
    void importProfile();
    /// Save the live card as a PDF (F7 "nyomtatás"): a native file dialog picks the path, the engine writes it.
    /// The dialog cannot be clicked by the harness; the `print_pdf` path is measured by `--printtest`.
    void printToPdf();
    /// Find-in-page (F7 "Ctrl+F"): show the find bar, hide it, or drive the engine's `find` op (next/previous).
    void openFind();
    void closeFind();
    void findNext();
    void findPrevious();
    /// A `find_result` event arrived from one of the engines (home or a card's container engine).
    void onFindResult(qint64 surface, int count, int active);

protected:
    /// The close button hides the window to the tray instead of ending the browser (ADR-043); without a
    /// tray it closes as it always did.
    void closeEvent(QCloseEvent* event) override;

private:
    void buildToolBar();
    void buildCardArea();
    /// The engine for one container ("" = the home/grid engine). Container engines are started lazily,
    /// each with its own `--user-data-dir` under the profile. Returns nullptr only when the engine
    /// executable is missing.
    EngineClient* engineFor(const QString& container);
    /// The profile directory name a container owns: its id with every character outside the safe set folded
    /// to `_` (the same rule the engine applies to partition keys, so the two never disagree).
    static QString containerDirName(const QString& container);
    /// Remove the on-disk profile a deleted container left behind (`<profile>/containers/<name>`), stopping
    /// its engine process first so Windows can release the directory.
    void cleanupContainerProfile(const QString& container);
    /// Remove the on-disk partition a deleted card left behind (`persist_card_<hash>`), from the card's own
    /// engine profile — the home engine for a container-less card, the container engine otherwise.
    void cleanupCardPartition(const QString& url, const QString& container);
    /// Wire one engine's signals into the window. The home engine and every container engine get the
    /// same handlers, so a container's surface behaves exactly like the home one.
    void wireEngine(EngineClient* engine);
    /// Fill the preferences dock's four spinboxes from a core `prefs` object (showSettings / refreshUserPrefs).
    void fillSettingsDock(const QJsonObject& prefs);
    /// Put the icon in the tray (ADR-043). Does nothing when the platform has no tray or the tray is off.
    void setupTray();
    /// Hide the window, leaving the tray icon as the way back — the close button's real work.
    void hideToTray();
    /// Tell the grid page which theme the toolbar picked (`data-theme` in home/app.js).
    ///
    /// The chrome and the product's own page must never disagree about the theme: the toolbar's buttons are
    /// the user's choice, so the grid follows them (measured: the two used to be separate switches).
    void pushThemeToGrid(qint64 surface);
    /// Push the shell's language to the grid (i18n, ADR-079): the page owns a `data-language` attribute and
    /// re-renders its own strings from it, the same way the theme attribute works.
    void pushLanguageToGrid(qint64 surface);
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
    /// Keep the grid's previews filled: ask the core which card needs a picture (previews.h) and start one
    /// job. Re-entered when a job finishes, so the queue drains one card at a time.
    void schedulePreviewWork();
    /// The worker's surface finished loading: photograph it (ADR-041).
    void onPreviewWorkerLoaded();
    /// One worker job's end: state back to Idle, deadline stopped, surface destroyed, witness emitted.
    void finishPreviewWorker(bool produced, const QString& reason);
    /// Abandon a running job because the user opened a card: the surface goes now, the record stays.
    void abortPreviewWorker(const QString& reason);
    /// True when the page in front is the grid — the only place a worker may run (ADR-041, point 1).
    bool gridIsShowing() const;
    /// (Re)start the hibernation clock for the live card.
    void armIdleTimer();
    /// Any sign of the user: restarts the hibernation clock.
    void noteActivity();
    /// The clock ran out: hibernate the card, unless it must stay alive (playing media).
    void onIdleTimeout();
    /// The memory governor's aggressive action (F6, ADR-020): the live card's working set at or above the hard
    /// band is treated like the idle timeout — the same guards and freeze path, only the trigger differs.
    void checkMemoryPressure();
    void setStatus(const QString& text);

    bool eventFilter(QObject* watched, QEvent* event) override;

    LiveCard* card_ = nullptr;
    QLabel* status_ = nullptr;
    QString currentTheme_;
    /// The home/grid engine: the surface that shows the editable grid, and the process that answers the
    /// page's bridge calls. A card opens in its *container's* engine instead (ADR-071).
    EngineClient* engine_ = nullptr;
    /// One engine process per container, keyed by container id. Started lazily when a card of that
    /// container opens; each gets its own `--user-data-dir`, so a container's profile is its own CEF
    /// profile and its cookie jar never shares a root with another container's.
    QHash<QString, EngineClient*> container_engines_;
    /// The engine that owns the live surface (`surface_`): the home engine while the grid is showing,
    /// the card's container engine while a card is open.
    EngineClient* live_engine_ = nullptr;
    /// The engine that owns the background preview worker's surface (`worker_surface_`).
    EngineClient* worker_engine_ = nullptr;
    /// The core owns the data; the shell only asks it.
    CoreClient* core_ = nullptr;
    qint64 surface_ = 0;
    /// The surface that shows the home page, or 0 when a real card is in front. Only this surface
    /// receives the card list.
    qint64 home_surface_ = 0;
    /// The hibernation clock (docs/HU/02 §4.2). Ten minutes of no user activity puts the live card
    /// back on disk, which is the whole memory promise in one timer.
    QTimer* idle_ = nullptr;
    /// The memory-pressure check clock (F6, ADR-020): a repeating timer that, while a card is live, samples its
    /// working set against the hard band. `ANTIKOLT_MEMORY_CHECK_MS` shortens it for the self test.
    QTimer* memory_check_ = nullptr;
    /// True while the card in front is being hibernated, so a late reply cannot re-arm the clock.
    bool hibernating_ = false;
    /// Set when the idle timeout hibernated the card: the grid comes back once the surface is gone,
    /// so the window is never left showing an empty card area.
    bool return_to_grid_ = false;
    /// Whether the card in front is pinned. A pinned card is the documented opt-out from automatic
    /// hibernation (docs/HU/02 §4.2); the flag arrives with the page's open request, because the shell
    /// does not keep the card list.
    bool live_card_pinned_ = false;
    /// True while a login popup (OAuth/SSO) is open on the live card: hibernation must wait, or the flow
    /// would lose its window and its cookies (docs/HU/02 §8.5).
    bool popup_open_ = false;
    /// The flag from the page's open request, held until the card's surface is attached: the grid's
    /// own detach happens in between and would otherwise clear it.
    bool pending_pinned_ = false;
    /// The card waiting to be shown once a surface exists.
    QString pending_url_;
    QString pending_title_;
    /// The container the card waiting to be shown belongs to (ADR-071). The page sends it with `open_card`, and the
    /// policy is read from the core's container table — the shell never invents a proxy or grants a permission.
    QString pending_container_;
    /// The container table the policy comes from, refreshed when a card opens (id → the core's record).
    QHash<QString, QJsonObject> container_policies_;
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
    /// The card currently on screen, for the scroll book (it outlives `pending_card_id_`).
    QString current_card_id_;
    /// The live card's URL, tracked so a crashed renderer can be reloaded (F7 crash recovery). Empty on the grid.
    QString live_card_url_;
    /// How many previews this run has produced; the cap keeps a long browsing session from filling the
    /// disk with screenshots.
    int previews_made_ = 0;
    /// The site theme the user asked for (`auto`, `dark`, `light`), re-applied on every surface (ADR-037).
    QString page_media_ = QStringLiteral("auto");
    /// Force dark pages (ADR-044/ADR-045): the user's switch, remembered across runs, re-applied on every load.
    QString page_dark_mode_ = QStringLiteral("off");
    /// The shell's user-facing language (i18n, ADR-078): `hu` is authoritative and default; `en` re-labels the
    /// chrome. Status messages and the grid are a follow-up slice and stay Hungarian for now.
    QString language_ = QStringLiteral("hu");
    /// The chrome widgets whose labels follow the language, kept so `setLanguage` can re-label in place.
    QAction* action_start_engine_ = nullptr;
    QAction* action_home_ = nullptr;
    QAction* action_freeze_ = nullptr;
    QAction* action_sessions_ = nullptr;
    QAction* action_settings_ = nullptr;
    QAction* action_site_theme_ = nullptr;
    QAction* action_import_bookmarks_ = nullptr;
    QToolButton* profile_button_ = nullptr;
    QToolButton* print_button_ = nullptr;
    QLabel* settings_live_label_ = nullptr;
    QLabel* settings_idle_label_ = nullptr;
    QLabel* settings_cold_label_ = nullptr;
    QLabel* settings_budget_label_ = nullptr;
    QLabel* settings_profile_label_ = nullptr;
    QLabel* settings_language_label_ = nullptr;
    QComboBox* prefs_language_ = nullptr;
    QAction* tray_open_ = nullptr;
    QAction* tray_quit_ = nullptr;
    /// The toolbar's dark-pages button, so its icon and tooltip always show the mode in force.
    QToolButton* page_dark_button_ = nullptr;
    /// The four theme buttons, by theme name (icon-only, the words are in the tooltip).
    QHash<QString, QToolButton*> theme_buttons_;
    /// The page zoom in percent, re-applied on every load (ADR-038); the label shows it.
    int zoom_percent_ = 100;
    QAction* zoom_label_ = nullptr;
    /// The panel's rule editors (ADR-039): the TTL in hours and the app-lock wipe.
    QSpinBox* sessions_ttl_ = nullptr;
    QCheckBox* sessions_lock_wipe_ = nullptr;
    /// The preferences dock (ADR-072/074) and its four editors. A dock, like the session panel — not a dialog.
    QDockWidget* settings_dock_ = nullptr;
    QSpinBox* prefs_live_cards_ = nullptr;
    QSpinBox* prefs_idle_minutes_ = nullptr;
    QSpinBox* prefs_cold_hours_ = nullptr;
    QSpinBox* prefs_budget_mb_ = nullptr;
    /// The resource profile picker (F6, `09-CELHARDVER.md` §3): a preset that fills the hibernation and budget.
    QComboBox* prefs_profile_ = nullptr;
    /// The stored resource profile kind string (`fast`/`balanced`/`saver`/`ultra`/`auto`/`manual`), cached so the
    /// governor knows whether it must re-resolve `auto` on every pressure sample (F6, ADR-020).
    QString resource_profile_ = QStringLiteral("balanced");
    /// Whether `resource_profile_` is `auto`: the effective profile is re-derived from power + pressure.
    bool auto_profile_ = false;
    /// Whether the machine is on mains power, cached from the system power status (and `ANTIKOLT_AC_POWER`).
    bool ac_power_ = true;
    /// The container default the core reported (`keep`/`sweep`/`wipe`), sent back unchanged on save.
    QString session_policy_ = QStringLiteral("sweep");
    /// The profile's memory budget in bytes (`memory_budget_mb` × MiB), cached from the core; the factory 512 MiB
    /// until the first `refreshUserPrefs` answers. `system.memory` reports it as `budgetBytes` (F6).
    qint64 memory_budget_bytes_ = 512LL * 1024 * 1024;
    /// The profile's governor bands in bytes (ADR-020 `memory_warn_bytes`/`memory_hard_bytes`), cached from the
    /// derived profile. `system.memory` derives the `pressure` signal from them against the measured working set
    /// (F6). The factory values match the `Balanced` profile (850 MiB / 1000 MiB).
    qint64 memory_warn_bytes_ = 850LL * 1024 * 1024;
    qint64 memory_hard_bytes_ = 1000LL * 1024 * 1024;
    /// The profile's top-level video display cap in CSS pixels (`video_max_height`, ADR-020/ADR-076), cached
    /// from the derived profile. 0 = no cap until the first `refreshUserPrefs` answers, and for a `Manual`
    /// profile. Applied to every surface on load, like the theme and zoom preferences.
    int video_max_height_ = 0;
    /// Until what epoch (ms) the background preview worker is paused after warn/hard pressure (F6, ADR-020 "warn =
    /// free background resources"). Zero when not paused; `startPreviewWorker` refuses while `now` is below it.
    qint64 preview_paused_until_ms_ = 0;
    /// The find bar (F7 "Ctrl+F"): a line edit + match counter shown while searching a page.
    QWidget* find_bar_ = nullptr;
    QLineEdit* find_edit_ = nullptr;
    QLabel* find_status_ = nullptr;
    QString find_last_text_;
    int find_last_count_ = 0;
    /// The idle-hibernation window in ms, cached from `user_prefs.idle_hibernate_ms` (ADR-072). The factory
    /// 10 minutes until the first `refreshUserPrefs` answers; `ANTIKOLT_HIBERNATE_MS` still overrides for the
    /// self test.
    qint64 idle_hibernate_ms_ = 600000;
    /// Whether the current profile refreshes previews in the background (ADR-020 `preview_refresh`). The factory
    /// `Balanced` profile does; `Saver`/`Ultra` do not. Cached from the derived profile in `refreshUserPrefs`.
    bool preview_refresh_ = true;
    /// When the engine host may shut down after being idle (ADR-020 `engine_warm_idle_shutdown_ms`). `-1` means
    /// "keep it warm" (T1, the factory); `0` means "as soon as no card is live" (T2, `Ultra`). Cached from the
    /// derived profile — the raster home (engine stopped) builds on this.
    qint64 engine_shutdown_ms_ = -1;
    /// Whether the raster home is active: the engine is stopped and the grid is shown from `home-raster.png`.
    bool raster_mode_ = false;
    /// The single-shot engine-idle timer that auto-activates the raster home (T2); null until first armed.
    QTimer* engine_idle_ = nullptr;
    /// A card's rectangle in the raster (CSS pixels), captured before the grid is rasterised (click mapping).
    struct RasterCard {
        QString id;
        QString url;
        QString title;
        qreal x = 0;
        qreal y = 0;
        qreal w = 0;
        qreal h = 0;
    };
    QList<RasterCard> raster_cards_;
    /// The grid's viewport size in CSS pixels when the positions were captured (maps a raster click back to the
    /// cards' coordinate space).
    qreal raster_css_w_ = 0;
    qreal raster_css_h_ = 0;
    /// The stacked card area: the live card (page 0) or the raster home (page 1, the engine stopped).
    QStackedLayout* card_stack_ = nullptr;
    QLabel* raster_label_ = nullptr;
    /// The full-resolution raster, kept so it can be re-fit (aspect-preserving) whenever the label resizes.
    QPixmap raster_full_;
    /// The size the raster is actually drawn at (after aspect-preserving scaling), for the click mapping.
    QSize raster_drawn_size_;
    /// Re-scale `raster_full_` to the label (aspect-preserving, centred) and install it as the pixmap.
    void updateRasterPixmap();
    /// The origin of the card in front, as the engine loaded it ("" when the grid is showing).
    QString live_origin_;
    /// ---- the background preview worker (ADR-041) ------------------------------------------------
    /// Where the worker is. The whole protocol is this value plus `worker_surface_`: a worker that is not
    /// `Idle` must be destroyed before another may start, and it yields the moment a card opens.
    antikolt::PreviewWorker worker_state_ = antikolt::PreviewWorker::Idle;
    /// The worker's own surface, 0 when none exists. Never the card's surface: those are different pages.
    qint64 worker_surface_ = 0;
    /// The last surface the worker held, for the self test's "the engine no longer knows it" witness.
    qint64 worker_last_surface_ = 0;
    QString worker_card_id_;
    QString worker_page_url_;
    QString worker_preview_name_;
    /// The job's ceiling (8 s, one shot): a page that never paints must not hold the slot.
    QTimer* worker_deadline_ = nullptr;
    /// ---- the tray icon (ADR-043) ----------------------------------------------------------------
    /// The icon in the notification area: the same icon the shortcut and the window carry, so "launcher"
    /// and "running" are the same picture. Null when the platform has no tray.
    QSystemTrayIcon* tray_ = nullptr;
    QMenu* tray_menu_ = nullptr;
    /// Set by `quitFromTray`: from then on the close button closes, because the tray menu already asked.
    bool quit_requested_ = false;
    /// The "it keeps running in the tray" hint is said once per run, not on every close.
    bool tray_hint_shown_ = false;
};
