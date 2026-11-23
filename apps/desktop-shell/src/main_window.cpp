// SPDX-License-Identifier: MPL-2.0

#include "main_window.h"

#include <QActionGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QStackedLayout>
#include <QStandardPaths>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "core_client.h"
#include "engine_client.h"
#include "previews.h"
#include "scroll_book.h"
#include "live_card.h"

namespace {

/// A JSON document rendered as a JavaScript string literal.
///
/// The shell pushes data into the page with `evaluate`, and the only safe way to do that is to hand
/// the page *data*: a string it parses itself, never text spliced into code. JSON's own escaping is
/// valid JavaScript string escaping, so wrapping the document in double quotes — with the two
/// characters that would end the literal escaped — is enough. U+2028/U+2029 are line terminators in
/// JavaScript but not in JSON strings, so they are escaped explicitly.
/// How long a page request may wait for the shell's answer before the watchdog answers it.///
/// Longer than any shell path (a core round trip is milliseconds), short enough that a page never sits on
/// a promise for a noticeable time. The engine drops held callbacks when the page goes away; this is the
/// other half of the same promise: the shell always answers (ADR-034).
constexpr int kPageAnswerMs = 5000;

/// How many previews one run may produce. A browsing session that opens hundreds of cards should not fill
/// the disk with screenshots; the cap is per run because the counter is in memory (a daily budget needs
/// persistence the core does not have yet, and that is recorded as open work rather than faked).
constexpr int kPreviewCap = 40;

/// How many times one preview is attempted before it is given up. Two attempts tolerate one stalled
/// capture; more would turn a broken engine into a screenshot loop.
constexpr int kPreviewAttempts = 2;

/// How old a preview may be before the next visit refreshes it (ADR-035 follow-up).
constexpr qint64 kPreviewMaxAgeMs = 7LL * 24 * 60 * 60 * 1000;

/// The hard ceiling on previews per day, persisted in the profile (a budget that resets on launch is not one).
constexpr int kPreviewDailyMax = 100;

/// The background worker's own viewport: the same default a card window has, so a preview looks like the
/// page does when the card is opened — and a browser has to lay something out to photograph it.
constexpr int kWorkerViewportWidth = 1280;
constexpr int kWorkerViewportHeight = 800;

/// One worker job's ceiling: load plus capture (ADR-041, point 3).
///
/// Eight seconds is enough for a page to paint on this machine and short enough that a stalled host cannot
/// hold the one worker slot; a page that has not painted by then is skipped, not waited for.
constexpr int kWorkerDeadlineMs = 8000;

/// The ceiling a job actually gets, in milliseconds.
///
/// `ANTIKOLT_PREVIEW_DEADLINE_MS` overrides it, the way `ANTIKOLT_HIBERNATE_MS` shortens the idle clock: a
/// self test must measure the worker, not how fast this machine's compositor falls back from a dead GPU,
/// and the same knob lets a check drive the ceiling deterministically with a tiny value.
int workerDeadlineMs() {
    bool configured = false;
    const int requested = qEnvironmentVariableIntValue("ANTIKOLT_PREVIEW_DEADLINE_MS", &configured);
    return configured && requested > 0 ? requested : kWorkerDeadlineMs;
}

/// The origin of a URL (`scheme://host[:port]`), or "" when the scheme has no storable origin.
///
/// The session ledger is keyed by origin, and a wipe must name the same string the browser uses: a
/// `file:` page has no origin to clear and a `data:` page has an opaque one, so both answer "".
QString originOfUrl(const QString& url) {
    const QUrl parsed(url);
    const QString scheme = parsed.scheme();
    if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")) {
        return QString();
    }
    QString origin = scheme + QStringLiteral("://") + parsed.host();
    if (parsed.port() > 0) {
        origin += QStringLiteral(":%1").arg(parsed.port());
    }
    return origin;
}

QString asJavaScriptStringLiteral(const QString& text) {
    QString escaped = text;
    escaped.replace(QLatin1String("\\"), QLatin1String("\\\\"));
    escaped.replace(QLatin1String("\""), QLatin1String("\\\""));
    escaped.replace(QLatin1String("\n"), QLatin1String("\\n"));
    escaped.replace(QLatin1String("\r"), QLatin1String("\\r"));
    escaped.replace(QLatin1String("\t"), QLatin1String("\\t"));
    escaped.replace(QChar(0x2028), QStringLiteral("\\u2028"));
    escaped.replace(QChar(0x2029), QStringLiteral("\\u2029"));
    return escaped;
}

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

/// The painter's own size for the generated icons: Qt scales them down for the toolbar, so they are drawn once
/// at a size that survives High DPI instead of being a bitmap of the wrong size.
constexpr int kIconPixels = 32;

/// The icon of one theme: a rounded "window" filled with the theme's background, carrying three lines of text in
/// the theme's text colour. Distinct at 20 px, and it needs no asset files — the palette is the specification.
QIcon ThemeIcon(const ThemePalette& palette) {
    QPixmap pixmap(kIconPixels, kIconPixels);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF frame(2.0, 2.0, kIconPixels - 4.0, kIconPixels - 4.0);
    painter.setPen(QPen(QColor(palette.border), 2.0));
    painter.setBrush(QColor(palette.background));
    painter.drawRoundedRect(frame, 6.0, 6.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(palette.text));
    painter.drawRoundedRect(QRectF(7.0, 8.0, 18.0, 3.5), 1.5, 1.5);
    painter.drawRoundedRect(QRectF(7.0, 15.0, 13.0, 3.0), 1.5, 1.5);
    painter.drawRoundedRect(QRectF(7.0, 21.0, 15.0, 3.0), 1.5, 1.5);
    painter.end();
    return QIcon(pixmap);
}

/// A crescent moon whose fill shows what the mode does to a page: hollow (off), near-black (invert), dark grey
/// (dim), warm grey (warm). The crescent is the *page*, not the browser — the browser theme has its own icons.
QIcon PageDarkIcon(const QString& mode) {
    QPixmap pixmap(kIconPixels, kIconPixels);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QPainterPath moon;
    moon.addEllipse(QRectF(3.0, 3.0, 26.0, 26.0));
    QPainterPath bite;
    bite.addEllipse(QRectF(13.0, 0.0, 26.0, 26.0));
    moon = moon.subtracted(bite);

    if (mode == QStringLiteral("off")) {
        painter.setPen(QPen(QColor(QStringLiteral("#8E9196")), 2.2));
        painter.setBrush(Qt::NoBrush);
    } else if (mode == QStringLiteral("invert")) {
        painter.setPen(QPen(QColor(QStringLiteral("#E9E9E7")), 1.2));
        painter.setBrush(QColor(QStringLiteral("#0B0B0C")));
    } else if (mode == QStringLiteral("dim")) {
        painter.setPen(QPen(QColor(QStringLiteral("#C9CDD2")), 1.2));
        painter.setBrush(QColor(QStringLiteral("#3A3F45")));
    } else {
        painter.setPen(QPen(QColor(QStringLiteral("#E0C9A6")), 1.2));
        painter.setBrush(QColor(QStringLiteral("#3C332A")));
    }
    painter.drawPath(moon);
    painter.end();
    return QIcon(pixmap);
}

/// The page's own answer to "are you playing sound or video right now?" — the one documented exception a user
/// feels (docs/HU/02 §4.2), asked the same way by hibernation and by the cleanup of session storage.
///
/// A `<video muted autoplay>` or an `<audio>` mid-playback reports `paused === false`; a page with no media at all
/// answers `idle`. Nothing is inferred from the page's title or URL: the element state is the fact.
QString LiveMediaPlayingScript() {
    return QStringLiteral(
        "(function(){var m=document.querySelectorAll('video,audio');"
        "for(var i=0;i<m.length;i++){if(!m[i].paused&&!m[i].ended)return 'playing';}"
        "return 'idle';})()");
}

/// The accent folding the page uses, on the shell side (`cards.search`, ADR-047).
///
/// `Kávé` must be found by typing `kave`, and the two sides must fold the same way — otherwise the same query
/// answers differently depending on whether the page or the shell filtered it.
QString FoldForSearch(const QString& text) {
    QString folded = text.normalized(QString::NormalizationForm_D);
    folded.removeIf([](QChar character) { return character.category() == QChar::Mark_NonSpacing; });
    folded = folded.toLower();
    // The handful of letters Unicode does not decompose — the same short list as the page's `foldSearch`.
    folded.replace(QChar(0x0142), QLatin1Char('l'));  // ł
    folded.replace(QChar(0x00F8), QLatin1Char('o'));  // ø
    folded.replace(QChar(0x0111), QLatin1Char('d'));  // đ
    folded.replace(QStringLiteral("ß"), QStringLiteral("ss"));
    return folded;
}

/// The human name of a theme, in the product's language. The toolbar shows the icon; tooltips and screen
/// readers show this.
QString themeLabel(const QString& theme) {
    if (theme == QStringLiteral("light")) {
        return QStringLiteral("Világos téma");
    }
    if (theme == QStringLiteral("grey")) {
        return QStringLiteral("Szürke téma (alap)");
    }
    if (theme == QStringLiteral("dark")) {
        return QStringLiteral("Sötét téma");
    }
    return QStringLiteral("AMOLED fekete téma");
}

/// The human name of a page-dark mode (ADR-045).
QString pageDarkModeLabel(const QString& mode) {
    if (mode == QStringLiteral("invert")) {
        return QStringLiteral("Sötét (invertálás)");
    }
    if (mode == QStringLiteral("dim")) {
        return QStringLiteral("Sötét szürke");
    }
    if (mode == QStringLiteral("warm")) {
        return QStringLiteral("Sötét meleg");
    }
    return QStringLiteral("Kikapcsolva");
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Antikolt"));
    resize(1280, 800);
    setMinimumSize(760, 480);

    // The forced-dark switch is a user preference, not a per-session whim: read it before the toolbar builds, so
    // the action starts in the state the engine will be asked for (ADR-044).
    // The page-dark mode is a user preference, not a per-session whim: read it before the toolbar builds, so the
    // button starts in the state the engine will be asked for. The old boolean key is the previous build's
    // spelling and is migrated here once (ADR-045).
    {
        QSettings settings;
        const QString stored = settings.value(QStringLiteral("ui/page_dark_mode")).toString();
        if (!stored.isEmpty() && pageDarkModes().contains(stored)) {
            page_dark_mode_ = stored;
        } else if (settings.value(QStringLiteral("ui/forced_dark"), false).toBool()) {
            page_dark_mode_ = QStringLiteral("invert");
        }
    }

    // The shell language is a user preference too (i18n, ADR-078): read it before the toolbar builds, so the
    // chrome starts in the chosen language. Hungarian is authoritative; anything unrecognised stays Hungarian.
    {
        QSettings settings;
        const QString lang = settings.value(QStringLiteral("ui/language")).toString();
        if (lang == QStringLiteral("en") || lang == QStringLiteral("hu")) {
            language_ = lang;
        }
    }

    buildToolBar();
    buildCardArea();

    status_ = new QLabel(this);
    statusBar()->addWidget(status_);

    engine_ = new EngineClient(this);
    wireEngine(engine_);

    // The card owns the surface lifecycle locally; the window mirrors it for the status bar.
    connect(card_, &LiveCard::surfaceAttached, this, [this](qint64 surface) {
        // A card is coming to the front: the worker yields first, so its surface is destroyed before the
        // card asks for one — the user must never wait for a preview (ADR-041, point 4).
        if (worker_state_ != antikolt::PreviewWorker::Idle) {
            abortPreviewWorker(QStringLiteral("kártya nyílik"));
        }
        surface_ = surface;
        // A pending card wins over the home page: this is the click-through path from the grid.
        const QString target = pending_url_.isEmpty() ? homePageUrl() : pending_url_;
        const bool opening_home = pending_url_.isEmpty();
        const QString title = pending_title_;
        pending_url_.clear();
        pending_title_.clear();

        setStatus(QStringLiteral("kártya felület: %1 · %2")
                      .arg(surface)
                      .arg(title.isEmpty() ? target : title));
        live_card_url_ = opening_home ? QString() : target;
        live_engine_->navigate(surface, target);
        if (opening_home) {
            // The grid is the only page allowed to talk to the shell. Installing it here (before the
            // load finishes) is enough: the engine re-injects it whenever the page reloads.
            live_engine_->installBridge(surface);
            home_surface_ = surface;
        } else {
            home_surface_ = 0;
            // A card just came to the front: the hibernation clock starts now, and the pinned flag
            // from the open request belongs to *this* card (the grid's detach came before it).
            live_card_pinned_ = pending_pinned_;
            pending_pinned_ = false;
            armIdleTimer();
        }
    });
    connect(card_, &LiveCard::surfaceDetached, this, [this](qint64 surface) {
        surface_ = 0;
        home_surface_ = 0;
        live_card_pinned_ = false;
        live_card_url_.clear();
        setStatus(QStringLiteral("kártya #%1 lemezre mentve · memória felszabadítva").arg(surface));
        // A card went back to disk: the engine is now idle, so the raster home's clock starts (T2). Skipped while
        // already in the raster home (enterRasterMode detaches too, and must not re-arm itself).
        if (!raster_mode_) {
            armEngineIdleTimer();
        }
    });
    connect(engine_, &EngineClient::bridgeCall, this, &MainWindow::handleBridgeCall);

    // The hibernation clock: `user_prefs.idle_hibernate_ms` of no user activity put the live card back on
    // disk, which is the memory promise in one timer (ADR-072). `ANTIKOLT_HIBERNATE_MS` shortens it for the
    // self test; without that a check would have to wait ten minutes to prove anything.
    const int configured_ms = qEnvironmentVariableIntValue("ANTIKOLT_HIBERNATE_MS");
    if (configured_ms > 0) {
        idle_hibernate_ms_ = configured_ms;
    }
    idle_ = new QTimer(this);
    idle_->setSingleShot(true);
    idle_->setInterval(static_cast<int>(idle_hibernate_ms_));
    connect(idle_, &QTimer::timeout, this, &MainWindow::onIdleTimeout);
    // The memory-pressure governor (F6, ADR-020): a repeating sample while a card is live. The factory 15 s keeps
    // the cost trivial; `ANTIKOLT_MEMORY_CHECK_MS` shortens it so the self test does not wait a quarter minute.
    const int memory_check_ms = qEnvironmentVariableIntValue("ANTIKOLT_MEMORY_CHECK_MS");
    memory_check_ = new QTimer(this);
    memory_check_->setInterval(memory_check_ms > 0 ? memory_check_ms : 15000);
    connect(memory_check_, &QTimer::timeout, this, &MainWindow::checkMemoryPressure);
    // It samples whichever surface is live (grid or card), so it runs from the start and stops only with the window.
    memory_check_->start();
    // Activity is what the user does, not what the engine reports: mouse and keyboard, wherever they
    // land inside the shell. The page's own input arrives in the engine's process, so the card also
    // wakes the clock through the bridge (a page that is not touched never sends anything).
    qApp->installEventFilter(this);

    // The worker's ceiling and its queue (ADR-041). One timer for the whole job — load *and* capture —
    // because the promise is "eight seconds, then the slot is free", not eight seconds per step.
    worker_deadline_ = new QTimer(this);
    worker_deadline_->setSingleShot(true);
    // The interval is set per job (`workerDeadlineMs()`), so a check can drive the ceiling too.
    connect(worker_deadline_, &QTimer::timeout, this, [this] {
        if (worker_state_ != antikolt::PreviewWorker::Idle) {
            abortPreviewWorker(QStringLiteral("a nyolc másodperces határidő lejárt"));
        }
    });
    // A finished job makes room for the next one. Deferred, because the signal is emitted from paths that
    // are in the middle of opening a card or tearing the surface down; scheduling then would race them.
    connect(this, &MainWindow::previewWorkerFinished, this, [this](const QString&, bool) {
        QTimer::singleShot(1200, this, &MainWindow::schedulePreviewWork);
    });

    setStatus(QStringLiteral("motor: nincs elindítva · kártyák: 0"));
    applyTheme(QStringLiteral("grey"));

    // The icon in the notification area: the same picture the shortcut carries, alive exactly as long as
    // this process is (ADR-043).
    setupTray();
}

void MainWindow::setLanguage(const QString& language) {
    const QString next = (language == QStringLiteral("en") || language == QStringLiteral("hu"))
                             ? language
                             : QStringLiteral("hu");
    if (next == language_) {
        return;
    }
    language_ = next;
    QSettings settings;
    settings.setValue(QStringLiteral("ui/language"), language_);
    // Re-label the chrome in place (i18n, ADR-078): the strings are the Hungarian sources, `t()` maps them.
    if (action_start_engine_ != nullptr) {
        action_start_engine_->setText(t(QStringLiteral("Motor indítása")));
    }
    if (action_home_ != nullptr) {
        action_home_->setText(t(QStringLiteral("⌂ Kezdőlap")));
    }
    if (action_freeze_ != nullptr) {
        action_freeze_->setText(t(QStringLiteral("❄ Fagyasztás")));
    }
    if (action_sessions_ != nullptr) {
        action_sessions_->setText(t(QStringLiteral("🗂 Munkamenetek")));
    }
    if (action_settings_ != nullptr) {
        action_settings_->setText(t(QStringLiteral("⚙ Beállítások")));
    }
    if (action_site_theme_ != nullptr) {
        action_site_theme_->setText(t(QStringLiteral("🌓 Oldal-téma: %1")).arg(page_media_));
    }
    if (profile_button_ != nullptr) {
        profile_button_->setText(t(QStringLiteral("💾 Profil")));
    }
    if (print_button_ != nullptr) {
        print_button_->setText(t(QStringLiteral("🖨️ PDF")));
    }
    if (tray_ != nullptr) {
        tray_->setToolTip(t(QStringLiteral("Antikolt — fut")));
    }
    if (tray_open_ != nullptr) {
        tray_open_->setText(t(QStringLiteral("Megnyitás")));
    }
    if (tray_quit_ != nullptr) {
        tray_quit_->setText(t(QStringLiteral("Kilépés")));
    }
    if (settings_dock_ != nullptr) {
        settings_dock_->setWindowTitle(t(QStringLiteral("Beállítások")));
    }
    if (settings_live_label_ != nullptr) {
        settings_live_label_->setText(t(QStringLiteral("Élő kártyák (max):")));
    }
    if (settings_idle_label_ != nullptr) {
        settings_idle_label_->setText(t(QStringLiteral("Hibernálás tétlenség után (perc):")));
    }
    if (settings_cold_label_ != nullptr) {
        settings_cold_label_->setText(t(QStringLiteral("Hideg kártya (óra):")));
    }
    if (settings_budget_label_ != nullptr) {
        settings_budget_label_->setText(t(QStringLiteral("Memória-büdzsé (MiB):")));
    }
    if (settings_profile_label_ != nullptr) {
        settings_profile_label_->setText(t(QStringLiteral("Erőforrás-profil:")));
    }
    if (settings_language_label_ != nullptr) {
        settings_language_label_->setText(t(QStringLiteral("Nyelv:")));
    }
    if (find_edit_ != nullptr) {
        find_edit_->setAccessibleName(t(QStringLiteral("Keresés az oldalon")));
        find_edit_->setPlaceholderText(t(QStringLiteral("Keresés az oldalon…")));
    }
    // The grid is the product's own page; its strings follow the same choice (i18n, ADR-079).
    if (home_surface_ != 0) {
        pushLanguageToGrid(home_surface_);
    }
}

bool MainWindow::trayEnabled() {
    return qEnvironmentVariableIsEmpty("ANTIKOLT_NO_TRAY");
}

void MainWindow::setupTray() {
    if (!trayEnabled()) {
        return;
    }
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        // No notification area (a session without a shell, a bare CI agent): the close button keeps meaning
        // what it always meant, and nothing pretends otherwise.
        qInfo("[shell] tray: nincs rendszertálca, a bezárás kilép");
        return;
    }
    tray_ = new QSystemTrayIcon(windowIcon(), this);
    tray_->setToolTip(t(QStringLiteral("Antikolt — fut")));
    tray_menu_ = new QMenu(this);
    tray_open_ = tray_menu_->addAction(t(QStringLiteral("Megnyitás")));
    tray_open_->setObjectName(QStringLiteral("tray.open"));
    connect(tray_open_, &QAction::triggered, this, &MainWindow::restoreFromTray);
    tray_menu_->addSeparator();
    tray_quit_ = tray_menu_->addAction(t(QStringLiteral("Kilépés")));
    tray_quit_->setObjectName(QStringLiteral("tray.quit"));
    connect(tray_quit_, &QAction::triggered, this, &MainWindow::quitFromTray);
    tray_->setContextMenu(tray_menu_);
    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason != QSystemTrayIcon::Trigger && reason != QSystemTrayIcon::DoubleClick) {
                    return;
                }
                // One click toggles: the icon is the way in and the way out of the way.
                if (isVisible()) {
                    hideToTray();
                } else {
                    restoreFromTray();
                }
            });
    tray_->show();
    qInfo("[shell] tray: ikon a tálcán (%s)",
          qUtf8Printable(windowIcon().isNull() ? QStringLiteral("ikon nélkül")
                                              : QStringLiteral("a program ikonjával")));
}

QStringList MainWindow::trayMenuLabels() const {
    QStringList labels;
    if (tray_menu_ == nullptr) {
        return labels;
    }
    for (const QAction* action : tray_menu_->actions()) {
        if (action != nullptr && !action->isSeparator()) {
            labels.append(action->text());
        }
    }
    return labels;
}

void MainWindow::hideToTray() {
    hide();
    if (tray_ != nullptr && !tray_hint_shown_) {
        // Said once: the first time the window vanishes the user must know where it went, and how to stop
        // it for good. Repeating it on every close would be a nag, not an explanation.
        tray_hint_shown_ = true;
        tray_->showMessage(QStringLiteral("Antikolt"),
                           QStringLiteral("A böngésző a tálcán fut tovább. Megnyitás és kilépés: a "
                                          "tálcaikon menüje."),
                           QSystemTrayIcon::Information, 5000);
    }
    setStatus(QStringLiteral("a tálcán fut · megnyitás: a tálcaikon"));
}

void MainWindow::restoreFromTray() {
    showNormal();
    raise();
    activateWindow();
    setStatus(QStringLiteral("előhozva a tálcáról"));
}

void MainWindow::quitFromTray() {
    quit_requested_ = true;
    if (tray_ != nullptr) {
        tray_->hide();
    }
    // Closing for real now: the close event sees the flag and lets the window go, and the engine is shut
    // down by the destructor like every other exit.
    close();
    QCoreApplication::quit();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (tray_ != nullptr && !quit_requested_) {
        // The close button puts the window away, it does not end the browser: the tray icon is the way
        // back, and "Kilépés" in its menu is the way out (ADR-043).
        hideToTray();
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::armIdleTimer() {
    if (idle_ != nullptr) {
        idle_->start();
    }
}

void MainWindow::noteActivity() {
    // Only a real card has something to hibernate: on the grid the clock would just tick away.
    if (surface_ != 0 && surface_ != home_surface_) {
        armIdleTimer();
    }
}

void MainWindow::onIdleTimeout() {
    if (surface_ == 0 || surface_ == home_surface_ || !card_->hasSurface() || hibernating_) {
        return;
    }
    // The documented opt-out, and the user asked for it explicitly: a pinned card stays alive. The
    // clock keeps running, so unpinning it (or pinning a different card) needs no extra bookkeeping.
    if (live_card_pinned_) {
        setStatus(QStringLiteral("kitűzött kártya — a hibernálás kimarad"));
        armIdleTimer();
        return;
    }
    if (popup_open_) {
        setStatus(QStringLiteral("bejelentkezés folyamatban — a hibernálás vár"));
        armIdleTimer();
        return;
    }
    hibernating_ = true;
    // The one documented exception a user would feel: a page that is playing sound or video keeps its
    // card alive (docs/HU/02 §4.2). Everything else goes to disk, unpinned or not.
    live_engine_->evaluate(
        surface_, LiveMediaPlayingScript(),
        [this](const QJsonObject& reply, const QString& error) {
            hibernating_ = false;
            const QString value = reply.value(QStringLiteral("value")).toString();
            if (error.isEmpty() && value == QStringLiteral("playing")) {
                setStatus(QStringLiteral("a kártya médiát játszik — a hibernálás vár"));
                armIdleTimer();
                return;
            }
            setStatus(QStringLiteral("tétlenség: a kártya lemezre megy"));
            return_to_grid_ = true;
            freezeCard();
        });
}

void MainWindow::checkMemoryPressure() {
    // The memory governor (F6, ADR-020). It samples the *current* live surface — the grid or a card — and acts on
    // the profile's bands. Warn frees background resources (the preview worker pauses for a cooldown); hard takes
    // the aggressive action too (the live card is hibernated, the same guards and freeze path as the idle timeout).
    if (surface_ == 0 || !card_->hasSurface() || hibernating_ || live_engine_ == nullptr ||
        !live_engine_->isRunning()) {
        return;
    }
    live_engine_->metrics(surface_, [this](const QJsonObject& measured, const QString& error) {
        if (!error.isEmpty()) {
            return;
        }
        const qint64 used = static_cast<qint64>(
            measured.value(QStringLiteral("engine_working_set_bytes")).toString().toLongLong());
        if (memory_hard_bytes_ > 0 && used >= memory_hard_bytes_) {
            // Hard: pause background previews, then hibernate the card itself when one is live.
            preview_paused_until_ms_ = QDateTime::currentMSecsSinceEpoch() + 300000;
            setStatus(QStringLiteral("memória-nyomás: a kemény sáv fölött"));
            if (surface_ != home_surface_) {
                onIdleTimeout();
            }
            return;
        }
        if (memory_warn_bytes_ > 0 && used >= memory_warn_bytes_) {
            // Warn: free background resources — pause the preview worker for a cooldown, but keep the card alive.
            preview_paused_until_ms_ = QDateTime::currentMSecsSinceEpoch() + 300000;
            setStatus(QStringLiteral(
                "memória-nyomás: a figyelmeztető sáv fölött — a preview szünetel"));
        }
    });
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    // Keep the raster fitted to its label, aspect-preserving, across resizes.
    if (watched == raster_label_ && event->type() == QEvent::Resize) {
        updateRasterPixmap();
        return QMainWindow::eventFilter(watched, event);
    }
    // A click on the raster home wakes the engine, and opens the card under the cursor when one is there.
    if (watched == raster_label_ && event->type() == QEvent::MouseButtonPress && raster_mode_) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton && raster_drawn_size_.width() > 0 &&
            raster_drawn_size_.height() > 0 && raster_css_w_ > 0 && raster_css_h_ > 0) {
            // The label letterboxes the aspect-preserving pixmap; map the click through the drawn rect back to the
            // grid's CSS coordinate space.
            const qreal offset_x = (raster_label_->width() - raster_drawn_size_.width()) / 2.0;
            const qreal offset_y = (raster_label_->height() - raster_drawn_size_.height()) / 2.0;
            const qreal x_in = mouse->position().x() - offset_x;
            const qreal y_in = mouse->position().y() - offset_y;
            if (x_in >= 0 && y_in >= 0 && x_in < raster_drawn_size_.width() && y_in < raster_drawn_size_.height()) {
                const qreal x_css = x_in / raster_drawn_size_.width() * raster_css_w_;
                const qreal y_css = y_in / raster_drawn_size_.height() * raster_css_h_;
                for (const RasterCard& card : raster_cards_) {
                    if (x_css >= card.x && x_css < card.x + card.w && y_css >= card.y && y_css < card.y + card.h) {
                        pending_url_ = card.url;
                        pending_title_ = card.title;
                        pending_card_id_ = card.id;
                        current_card_id_ = card.id;
                        pending_pinned_ = false;
                        pending_container_.clear();
                        break;
                    }
                }
            }
        }
        wakeFromRaster();
        return QMainWindow::eventFilter(watched, event);
    }
    switch (event->type()) {
        case QEvent::KeyPress:
        case QEvent::MouseButtonPress:
        case QEvent::MouseMove:
        case QEvent::Wheel:
            noteActivity();
            break;
        default:
            break;
    }
    return QMainWindow::eventFilter(watched, event);
}

MainWindow::~MainWindow() {
    // Teardown order is not cosmetic: the browser lives in a child window of this window, so it has to
    // be closed *before* Qt destroys the widget that owns it, and the engine has to be gone before the
    // process exits. Getting this wrong crashed with an access violation after every successful run.
    // The worker's surface goes first: the engine is still alive to destroy it, and `shutdown()` below
    // would otherwise leave the request unanswered.
    abortPreviewWorker(QStringLiteral("a shell leáll"));
    closeLiveCard();
    if (core_ != nullptr) {
        core_->shutdown();
    }
    // Every engine process, the home engine and each container engine, must be gone before the window is.
    if (engine_ != nullptr) {
        engine_->shutdown();
    }
    const auto containers = container_engines_;
    for (EngineClient* client : containers) {
        if (client != nullptr && client != engine_) {
            client->shutdown();
        }
    }
}

void MainWindow::closeLiveCard() {
    if (live_engine_ == nullptr || card_ == nullptr) {
        return;
    }
    const qint64 live = card_->surface();
    if (live <= 0) {
        return;
    }
    // Ask for the close and let `shutdown()` be the barrier: it waits for the engine process to exit,
    // and the engine cannot exit before it has closed the browser. Pumping Qt events here instead
    // re-enters widget destruction and crashed the process on the way out.
    live_engine_->destroySurface(live, nullptr);
    card_->detach();
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

    // The remembered theme is the one being applied — set before anything reads it, or the page would always
    // receive the *previous* choice (measured: the grid lagged exactly one click behind the toolbar).
    currentTheme_ = theme;

    // The grid is the product's own page and has a theme attribute of its own: the toolbar's choice is the
    // one the user made, so the page follows it instead of keeping a second, disagreeing switch.
    if (home_surface_ != 0) {
        pushThemeToGrid(home_surface_);
    }

    return true;
}

void MainWindow::pushThemeToGrid(qint64 surface) {
    if (engine_ == nullptr || surface == 0 || currentTheme_.isEmpty()) {
        return;
    }
    // The page owns the attribute and re-renders the cards from it (home/app.js `setTheme`); the storage write
    // is the same one its own buttons do, so a reload keeps the choice. Storage can be denied in a locked
    // profile — the attribute still applies for this session, hence the try/catch on the page.
    engine_->evaluate(
        surface,
        QStringLiteral("document.documentElement.setAttribute('data-theme','%1');"
                       "try{localStorage.setItem('antikolt.theme','%1')}catch(e){}")
            .arg(currentTheme_),
        [](const QJsonObject&, const QString&) {});
}

void MainWindow::pushLanguageToGrid(qint64 surface) {
    if (engine_ == nullptr || surface == 0) {
        return;
    }
    // The page owns the attribute and re-renders its own strings from it (home/app.js `setLanguage`); the
    // storage write is the same one its own code does, so a reload keeps the choice. Storage can be denied in a
    // locked profile — the attribute still applies for this session, hence the try/catch on the page.
    engine_->evaluate(
        surface,
        QStringLiteral("document.documentElement.setAttribute('data-language','%1');"
                       "try{localStorage.setItem('antikolt.language','%1')}catch(e){}"
                       "if(window.antikoltSetLanguage){window.antikoltSetLanguage('%1')}")
            .arg(language_),
        [](const QJsonObject&, const QString&) {});
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

QString MainWindow::coreDataDir() {
    const QString fromEnvironment = qEnvironmentVariable("ANTIKOLT_CORE_DATA");
    if (!fromEnvironment.isEmpty()) {
        return fromEnvironment;
    }
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/core");
}

QString MainWindow::homePageUrl() {
    const QString fromEnvironment = qEnvironmentVariable("ANTIKOLT_HOME");
    if (!fromEnvironment.isEmpty()) {
        return QUrl::fromLocalFile(fromEnvironment).toString();
    }
    // A user-authored home page (item 3, editable home): one `home.html` in the core's data directory, served
    // instead of the built-in grid when it exists. The same directory the core uses, so the profile carries it.
    const QString customHome = coreDataDir() + QStringLiteral("/home.html");
    if (QFileInfo::exists(customHome)) {
        return QUrl::fromLocalFile(QDir::cleanPath(customHome)).toString();
    }
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/home/index.html"),
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

QString MainWindow::coreExecutable() {
    const QString fromEnvironment = qEnvironmentVariable("ANTIKOLT_CORE");
    if (!fromEnvironment.isEmpty() && QFileInfo::exists(fromEnvironment)) {
        return fromEnvironment;
    }
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/antikolt-core-service.exe"),
        appDir + QStringLiteral("/../../../target/debug/antikolt-core-service.exe"),
        appDir + QStringLiteral("/../../../target/release/antikolt-core-service.exe"),
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QDir::cleanPath(candidate);
        }
    }
    return {};
}

void MainWindow::wireEngine(EngineClient* engine) {
    connect(engine, &EngineClient::failed, this, [this](const QString& message) {
        setStatus(QStringLiteral("motor hiba: %1").arg(message));
    });
    // Find-in-page results (F7 "Ctrl+F") come from whichever engine owns the live surface.
    connect(engine, &EngineClient::findResult, this, &MainWindow::onFindResult);
    // A crashed renderer (F7 "crash recovery"): reload the page. The browser object survives the renderer, so a
    // fresh navigation spawns a new one — the grid goes home, a card reloads its own URL.
    connect(engine, &EngineClient::rendererTerminated, this,
            [this, engine](qint64 surface, int status, int code) {
                const QString url = (surface == home_surface_) ? homePageUrl() : live_card_url_;
                setStatus(QStringLiteral("az oldal összeomlott (#%1) — újratöltés (állapot %2, kód %3)")
                              .arg(surface)
                              .arg(status)
                              .arg(code));
                if (!url.isEmpty()) {
                    engine->navigate(surface, url);
                }
            });
    connect(engine, &EngineClient::engineReady, this, [this, engine](const QJsonObject& info) {
        setStatus(QStringLiteral("motor: %1 %2 · képességek: %3")
                      .arg(info.value(QStringLiteral("name")).toString(),
                           info.value(QStringLiteral("version")).toString())
                      .arg(info.value(QStringLiteral("capabilities")).toArray().size()));
        // Only the home engine's welcome raises the grid. A container engine's welcome means its process
        // is up; its surface is requested by `openCard`, never by the welcome (a second `create_surface`
        // while the first is still pending is answered with `engine_busy`).
        if (engine == engine_) {
            ensureSurface();
        }
    });
    connect(engine, &EngineClient::loadFinished, this,
            [this](qint64 surface, const QString& url, int httpStatus) {
                // The background worker's own surface: its load is not the card's, and must not move the
                // window's idea of what the user is looking at (ADR-041). The engine's events carry the
                // surface id, so the worker's page can never masquerade as the grid.
                if (worker_state_ != antikolt::PreviewWorker::Idle && surface == worker_surface_) {
                    onPreviewWorkerLoaded();
                    return;
                }
                setStatus(QStringLiteral("betöltve (#%1, %2): %3")
                              .arg(surface)
                              .arg(httpStatus)
                              .arg(url));
                // The emulation belongs to the page that is showing, so a navigation forgets it: the
                // remembered preference is applied again on every load (ADR-037).
                if (page_media_ != QStringLiteral("auto") && live_engine_ != nullptr) {
                    live_engine_->setMedia(surface, page_media_, nullptr);
                }
                // Same for the zoom: it is a preference of this browser window, not of one page (ADR-038).
                if (zoom_percent_ != 100 && live_engine_ != nullptr) {
                    live_engine_->setZoom(surface, zoom_percent_, nullptr);
                }
                // And the page-dark mode: the page forgot what the previous load asked for (ADR-044/ADR-045).
                if (page_dark_mode_ != QStringLiteral("off") && live_engine_ != nullptr) {
                    live_engine_->setPageDarkMode(surface, page_dark_mode_, nullptr);
                }
                // The profile's video-height cap (ADR-020/ADR-076): a display cap on top-level videos,
                // re-applied on every load like the theme and zoom, because a navigation forgets it.
                if (video_max_height_ > 0 && live_engine_ != nullptr) {
                    live_engine_->setVideoCap(surface, video_max_height_, nullptr);
                }
                // The grid must show the cards the core owns, so the list is pushed once the page is
                // there to receive it. A real card is not a grid, and the *tracked* home id is not a reliable
                // test on its own: it is cleared when a card detaches, and that detach can arrive after the
                // next grid has already loaded (measured: the second grid showed the page's static demo
                // cards because this push never ran). The home URL is what makes a page the grid.
                if (surface == home_surface_ || url == homePageUrl()) {
                    live_origin_.clear();
                    home_surface_ = surface;
                    // The page starts in the toolbar's theme, not in its own remembered default: one product,
                    // one theme switch (measured: the grid used to come up grey while the chrome was amoled).
                    pushThemeToGrid(surface);
                    pushLanguageToGrid(surface);
                    pushCardsToGrid(surface);
                    // The grid is the only place the preview worker may run (ADR-041), and its empty frames
                    // are exactly what the worker exists to fill. Scheduled a beat after the push, so the
                    // core has answered and the grid is not still laying out.
                    QTimer::singleShot(1500, this, &MainWindow::schedulePreviewWork);
                    return;
                }
                // A card finished loading: if the policy queued this origin (see cleanupSessions), this
                // is the moment to wipe it — on its own surface, with the plan's storage list.
                live_origin_ = originOfUrl(url);
                producePreview(surface);
                // The reader's place: a card is a place, and the top of a long article is not where they left
                // it. The offset was remembered when this card was left (see rememberScroll).
                if (!current_card_id_.isEmpty()) {
                    const int remembered =
                        antikolt::scrollPositionFor(scrollBookPath(), current_card_id_);
                    if (remembered > 0) {
                        // Absolute, a beat after the load: `loadFinished` is not "layout done" (a scroll then
                        // is clamped to 0), and the event can fire twice for one page — a relative scroll
                        // would apply twice and land at 1400 instead of 700 (measured).
                        QTimer::singleShot(300, this, [this, surface, remembered] {
                            live_engine_->evaluate(
                                surface, QStringLiteral("window.scrollTo(0,%1)").arg(remembered),
                                [](const QJsonObject&, const QString&) {});
                        });
                    }
                }
                // A card finished loading: if the policy queued this origin (see cleanupSessions), this is
                // the moment to wipe it — on its own surface, with the plan's storage list.
                const auto queued = wipe_on_open_.find(live_origin_);
                if (live_origin_.isEmpty() || queued == wipe_on_open_.end()) {
                    return;
                }
                const QString origin = live_origin_;
                const PendingWipe pending = queued.value();
                wipe_on_open_.erase(queued);
                live_engine_->clearOriginData(
                    surface, origin, pending.storages,
                    [this, origin, container = pending.container](const QJsonObject&,
                                                                  const QString& error) {
                        if (!error.isEmpty()) {
                            setStatus(QStringLiteral("takarítás hiba (%1): %2")
                                          .arg(origin, error));
                            return;
                        }
                        core_->sessionClose(
                            container, origin,
                            [this, origin](const QJsonObject&, const QString& close_error) {
                                setStatus(close_error.isEmpty()
                                              ? QStringLiteral("kitakarva: %1").arg(origin)
                                              : QStringLiteral("kitakarva, de a napló nem (%1)")
                                                    .arg(close_error));
                                if (sessions_dock_ != nullptr && sessions_dock_->isVisible()) {
                                    refreshSessions();
                                }
                            });
                    });
            });
    connect(engine, &EngineClient::loadFailed, this,
            [this](qint64 surface, const QString& url, const QString& reason) {
                setStatus(QStringLiteral("betöltési hiba (#%1): %2 — %3").arg(surface).arg(url, reason));
            });
    // The login popup (docs/HU/02 §8.5): while it is open the card must not hibernate, or the SSO flow
    // would lose its window and cookies. The engine enforces the 60 s cap; the shell only reacts.
    connect(engine, &EngineClient::popupRequested, this,
            [this](qint64, const QString& url, const QString& disposition) {
                if (disposition == QStringLiteral("authenticated_window")) {
                    popup_open_ = true;
                    setStatus(QStringLiteral("bejelentkezési ablak nyitva: %1").arg(url));
                    armIdleTimer();
                } else {
                    setStatus(QStringLiteral("elutasított felugró ablak: %1").arg(url));
                }
            });
    connect(engine, &EngineClient::popupClosed, this, [this](qint64) {
        popup_open_ = false;
        setStatus(QStringLiteral("a bejelentkezési ablak bezárult"));
        armIdleTimer();
    });
}

EngineClient* MainWindow::engineFor(const QString& container) {
    if (container.isEmpty()) {
        return engine_;
    }
    const auto found = container_engines_.constFind(container);
    if (found != container_engines_.constEnd()) {
        return found.value();
    }
    const QString enginePath = engineExecutable();
    if (enginePath.isEmpty() || engine_profile_.isEmpty()) {
        return nullptr;
    }
    // A container id is minted by the core and is safe, but the profile path must never let one container
    // read another's directory through a separator, so anything but the safe set is folded to `_`.
    const QString directory = containerDirName(container);
    auto* client = new EngineClient(this);
    wireEngine(client);
    const QString profile = engine_profile_ + QStringLiteral("/containers/") + directory;
    QDir().mkpath(profile);
    // Every engine serves the *same* shared preview directory, so a preview the shell wrote is
    // reachable from a card's container engine too (antikolt://blob/ is otherwise engine-local).
    if (!client->start(enginePath, profile, /*hidden=*/false,
                       engine_profile_ + QStringLiteral("/previews"))) {
        delete client;
        return nullptr;
    }
    client->handshake();
    container_engines_.insert(container, client);
    setStatus(QStringLiteral("konténer-motor indítva: %1").arg(container));
    return client;
}

QString MainWindow::containerDirName(const QString& container) {
    QString directory = container;
    for (QChar& character : directory) {
        const bool safe = (character >= QLatin1Char('a') && character <= QLatin1Char('z')) ||
                          (character >= QLatin1Char('A') && character <= QLatin1Char('Z')) ||
                          (character >= QLatin1Char('0') && character <= QLatin1Char('9')) ||
                          character == QLatin1Char('-') || character == QLatin1Char('_') ||
                          character == QLatin1Char('.');
        if (!safe) {
            character = QLatin1Char('_');
        }
    }
    return directory;
}

void MainWindow::cleanupContainerProfile(const QString& container) {
    if (engine_profile_.isEmpty() || container.isEmpty()) {
        return;
    }
    // Stop the container's engine first: Windows cannot remove a directory a live process holds open. The engine
    // object is deleted afterwards; the destructor iterates `container_engines_`, so it must be out of the map by
    // then.
    EngineClient* client = container_engines_.take(container);
    if (client != nullptr) {
        if (live_engine_ == client) {
            closeLiveCard();
        }
        client->shutdown();
        client->deleteLater();
    }
    const QString directory = engine_profile_ + QStringLiteral("/containers/") + containerDirName(container);
    QDir(directory).removeRecursively();
}

void MainWindow::cleanupCardPartition(const QString& url, const QString& container) {
    if (engine_profile_.isEmpty() || url.isEmpty()) {
        return;
    }
    // The partition key is `persist:card_<12 hex chars>` (the URL's SHA-1, ADR-009); the engine stores it as the
    // directory `persist_card_<hash>` under the engine's own profile. A container-less card lives in the home
    // engine, a card in a container in that container's engine.
    const QString hash =
        QString::fromLatin1(QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
    const QString root = container.isEmpty()
                             ? engine_profile_
                             : engine_profile_ + QStringLiteral("/containers/") + containerDirName(container);
    QDir(root + QStringLiteral("/persist_card_") + hash).removeRecursively();
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
    engine_profile_ = profile;
    QDir().mkpath(profile + QStringLiteral("/previews"));

    if (!engine_->start(enginePath, profile, /*hidden=*/false, profile + QStringLiteral("/previews"))) {
        return;
    }
    setStatus(QStringLiteral("motor indítása: %1").arg(enginePath));
    engine_->handshake();

    // The core owns the data. A missing core must not stop the browser from working: it only means the
    // grid cannot show real cards yet, and the user deserves to be told that instead of seeing nothing.
    if (core_ == nullptr) {
        core_ = new CoreClient(this);
        connect(core_, &CoreClient::failed, this, [this](const QString& message) {
            setStatus(QStringLiteral("mag hiba: %1").arg(message));
        });
        connect(core_, &CoreClient::ready, this, [this](const QJsonObject& info) {
            setStatus(QStringLiteral("mag: %1 %2")
                          .arg(info.value(QStringLiteral("name")).toString(),
                               info.value(QStringLiteral("version")).toString()));
            // The memory budget (F6) is a profile setting: cache it once the core is there to answer.
            refreshUserPrefs();
        });
    }
    if (!core_->isRunning()) {
        const QString corePath = coreExecutable();
        // `ANTIKOLT_CORE_DATA` exists for the self tests: the 500-card scale check must not seed a developer's
        // own grid with five hundred probe cards (and must not read their cards either). Unset means the profile
        // directory, which is what the product uses.
        const QString coreData = coreDataDir();
        QDir().mkpath(coreData);
        if (corePath.isEmpty()) {
            setStatus(QStringLiteral("a mag nem található (ANTIKOLT_CORE) — a rács egyelőre mintaadatot mutat"));
        } else if (core_->start(corePath, coreData)) {
            core_->handshake();
        }
    }
}

void MainWindow::ensureSurface() {
    const bool opening_home = pending_url_.isEmpty();
    EngineClient* target = opening_home ? engine_ : engineFor(pending_container_);
    if (target == nullptr || !target->isRunning()) {
        return;
    }
    if (card_->hasSurface()) {
        // The surface already exists and lives in `live_engine_`. Going home *navigates* the same surface when it
        // is already in the home engine — it is not re-attached, so nothing else would install the bridge.
        // Crossing an engine boundary (a card open in a container engine) means the grid must move back to the
        // home engine, so the old surface is hibernated and recreated there.
        if (live_engine_ == target) {
            const QString dest = opening_home ? homePageUrl() : pending_url_;
            live_card_url_ = opening_home ? QString() : dest;
            live_engine_->navigate(card_->surface(), dest);
            pending_url_.clear();
            pending_title_.clear();
            if (opening_home) {
                live_engine_->installBridge(card_->surface());
                home_surface_ = card_->surface();
            }
            return;
        }
        const qint64 previous = card_->surface();
        EngineClient* previous_engine = live_engine_;
        previous_engine->snapshot(
            previous, [this, previous, previous_engine](const QJsonObject&, const QString& error) {
                if (!error.isEmpty()) {
                    setStatus(QStringLiteral("az előző kártya mentése nem sikerült: %1").arg(error));
                }
                card_->detach();
                previous_engine->destroySurface(previous,
                                                [this](const QJsonObject&, const QString&) {
                                                    ensureSurface();
                                                });
            });
        return;
    }
    // One partition per card: two cards never share cookies or storage by accident (ADR-009). The partition is the
    // storage; the container is the policy (ADR-071) — its proxy and the permissions its pages may ask for. Each
    // container runs in its own engine process, so the surface is attached to the engine that owns its profile.
    live_engine_ = target;
    const QString partition =
        opening_home
            ? QStringLiteral("persist:home")
            : QStringLiteral("persist:card_%1")
                  .arg(QString::fromLatin1(
                      QCryptographicHash::hash(pending_url_.toUtf8(), QCryptographicHash::Sha1)
                          .toHex()
                          .left(12)));
    SurfacePolicy policy;
    if (!opening_home) {
        policy = surfacePolicyFromEntry(container_policies_.value(pending_container_));
        policy.container = pending_container_;
    }
    if (!card_->attach(live_engine_, partition, policy)) {
        setStatus(QStringLiteral("a kártya nem tudott felületet kérni a motortól"));
    }
}

void MainWindow::openCard(const QString& url, const QString& title) {
    // The user wins over the background: a preview is never worth a moment of their wait, so the running
    // worker is stopped before anything else happens (ADR-041, point 4).
    if (worker_state_ != antikolt::PreviewWorker::Idle) {
        abortPreviewWorker(QStringLiteral("kártya nyílik"));
    }
    rememberScroll([this, url, title] {
    if (url.isEmpty()) {
        setStatus(QStringLiteral("a kártyának nincs címe"));
        return;
    }
    if (!engine_->isRunning()) {
        setStatus(QStringLiteral("a motor még nem fut — nyomd meg a „Motor indítása” gombot"));
        return;
    }
    pending_url_ = url;
    pending_title_ = title;

    if (!card_->hasSurface()) {
        ensureSurface();
        return;
    }

    // Hibernation before switching is not optional: the live surface must be on disk before it is
    // destroyed (invariant I2), otherwise the card the user just left is gone for good.
    const qint64 previous = card_->surface();
    live_engine_->snapshot(previous, [this, previous, title](const QJsonObject& result,
                                                             const QString& error) {
        if (error.isEmpty()) {
            setStatus(QStringLiteral("előző kártya mentve (%1 bájt) · váltás: %2")
                          .arg(result.value(QStringLiteral("bytes")).toInt())
                          .arg(title.isEmpty() ? pending_url_ : title));
        } else {
            setStatus(QStringLiteral("az előző kártya mentése nem sikerült: %1").arg(error));
        }
        card_->detach();
        live_engine_->destroySurface(previous, [this](const QJsonObject&, const QString&) {
            ensureSurface();
        });
    });
    });
}

void MainWindow::handleBridgeCall(qint64 surface, const QString& method, const QJsonObject& args,
                                  qint64 request) {
    if (request > 0) {
        // Every question is answered exactly once. The set remembers who is waiting; the watchdog answers
        // what a path forgot, because a page promise that never settles is a hung page (ADR-034).
        awaiting_page_reply_.insert(request);
        QTimer::singleShot(kPageAnswerMs, this, [this, request] {
            if (awaiting_page_reply_.contains(request)) {
                setStatus(QStringLiteral("a lap kérése nem kapott választ időben (#%1)").arg(request));
                replyToPage(request, {}, QStringLiteral("a shell nem válaszolt időben"));
            }
        });
    }
    if (method == QStringLiteral("open_card")) {
        const QString url = args.value(QStringLiteral("url")).toString();
        const QString title = args.value(QStringLiteral("title")).toString();
        if (url.isEmpty()) {
            setStatus(QStringLiteral("a kezdőlap cím nélküli kártyát kért megnyitni (#%1)")
                          .arg(surface));
            replyToPage(request, {}, QStringLiteral("a kártyának nincs címe"));
            return;
        }
        pending_pinned_ = args.value(QStringLiteral("pinned")).toBool();
        pending_card_id_ = args.value(QStringLiteral("id")).toString();
        current_card_id_ = pending_card_id_;
        pending_container_ = args.value(QStringLiteral("container")).toString();
        // The container's proxy and permission policy (ADR-071) are the core's data, so the table is fetched before the
        // surface exists: the engine gets the policy with the first navigation instead of a second, unprotected one.
        const auto open = [this, url, title] { openCard(url, title); };
        if (pending_container_.isEmpty() || core_ == nullptr || !core_->isRunning()) {
            open();
        } else {
            core_->containersList([this, open](const QJsonObject& reply, const QString& error) {
                if (error.isEmpty()) {
                    for (const QJsonValue& value :
                         reply.value(QStringLiteral("containers")).toArray()) {
                        const QJsonObject entry = value.toObject();
                        const QString id = entry.value(QStringLiteral("id")).toString();
                        if (!id.isEmpty()) {
                            container_policies_.insert(id, entry);
                        }
                    }
                }
                open();
            });
        }
        // An acknowledgement, not a result: the card loads asynchronously and the page follows it
        // through the grid. Answered anyway, so a page that awaits never hangs.
        replyToPage(request, QJsonObject{{QStringLiteral("opened"), true}});
        return;
    }
    if (method == QStringLiteral("freeze_card")) {
        freezeCard();
        replyToPage(request, QJsonObject{{QStringLiteral("frozen"), true}});
        return;
    }
    // ---- questions the page waits for (protocol 2, ADR-034) --------------------------------
    if (request > 0) {
        if (method == QStringLiteral("cards.search")) {
            const QString query = FoldForSearch(args.value(QStringLiteral("q")).toString()).trimmed();
            if (core_ == nullptr || !core_->isRunning()) {
                replyToPage(request, {}, QStringLiteral("a mag nem fut"));
                return;
            }
            core_->cardsList([this, request, query](const QJsonObject& reply,
                                                    const QString& error) {
                if (!error.isEmpty()) {
                    replyToPage(request, {}, error);
                    return;
                }
                QJsonArray matches;
                for (const QJsonValue& value : reply.value(QStringLiteral("cards")).toArray()) {
                    const QJsonObject card = value.toObject();
                    if (query.isEmpty()) {
                        matches.append(card);
                        continue;
                    }
                    // The page's captured text is part of what a search may look at (ADR-049): a user remembers a
                    // sentence, not the title. It is folded the same way the query is, so `kave` finds `Kávé`.
                    const QString haystack = FoldForSearch(
                        card.value(QStringLiteral("title")).toString() + QLatin1Char(' ') +
                        card.value(QStringLiteral("url")).toString() + QLatin1Char(' ') +
                        card.value(QStringLiteral("notes")).toString() + QLatin1Char(' ') +
                        card.value(QStringLiteral("page_text")).toString());
                    if (haystack.contains(query)) {
                        matches.append(card);
                    }
                }
                replyToPage(request, QJsonObject{{QStringLiteral("cards"), matches}});
            });
            return;
        }
        if (method == QStringLiteral("cards.upsert")) {
            const QJsonObject card = args.value(QStringLiteral("card")).toObject();
            if (card.isEmpty() || core_ == nullptr || !core_->isRunning()) {
                replyToPage(request, {}, QStringLiteral("nincs kártya vagy a mag nem fut"));
                return;
            }
            // The whole record travels, so a pin, a note and a new card are the same call; the core is
            // the one that validates and stores it.
            core_->cardUpsert(card, [this, request](const QJsonObject& saved, const QString& error) {
                if (!error.isEmpty()) {
                    replyToPage(request, {}, error);
                    return;
                }
                replyToPage(request,
                            QJsonObject{{QStringLiteral("card"),
                                         saved.value(QStringLiteral("card"))}});
            });
            return;
        }
        if (method == QStringLiteral("cards.delete")) {
            const QString card_id = args.value(QStringLiteral("id")).toString();
            if (card_id.isEmpty() || core_ == nullptr || !core_->isRunning()) {
                replyToPage(request, {}, QStringLiteral("nincs azonosító vagy a mag nem fut"));
                return;
            }
            // The on-disk partition is named from the card's URL (ADR-009), so the record is read *before* the delete
            // and the directory removed *after* it (ADR-071 cleanup). A missing record is not an error: the delete may
            // still go through, and the cleanup is best-effort.
            core_->cardsList([this, request, card_id](const QJsonObject& listed, const QString& list_error) {
                QString url;
                QString container;
                if (list_error.isEmpty()) {
                    const QJsonArray cards = listed.value(QStringLiteral("cards")).toArray();
                    for (const QJsonValue& value : cards) {
                        const QJsonObject card = value.toObject();
                        if (card.value(QStringLiteral("id")).toString() == card_id) {
                            url = card.value(QStringLiteral("url")).toString();
                            container = card.value(QStringLiteral("container")).toString();
                            break;
                        }
                    }
                }
                if (core_ == nullptr || !core_->isRunning()) {
                    replyToPage(request, {}, QStringLiteral("a mag nem fut"));
                    return;
                }
                core_->cardDelete(card_id,
                                  [this, request, url, container](const QJsonObject& reply,
                                                                  const QString& error) {
                                      if (!error.isEmpty()) {
                                          replyToPage(request, {}, error);
                                          return;
                                      }
                                      cleanupCardPartition(url, container);
                                      replyToPage(request, reply);
                                  });
            });
            return;
        }
        // ---- the container table (ADR-052) -----------------------------------------------------
        // The page draws the legend and the card's selector from here, so a rename in the core is what a reload
        // shows: the table is not a constant in the page any more.
        if (method == QStringLiteral("containers.list")) {
            if (core_ == nullptr || !core_->isRunning()) {
                replyToPage(request, {}, QStringLiteral("a mag nem fut"));
                return;
            }
            core_->containersList([this, request](const QJsonObject& reply, const QString& error) {
                if (!error.isEmpty()) {
                    replyToPage(request, {}, error);
                    return;
                }
                replyToPage(request, QJsonObject{{QStringLiteral("containers"),
                                                 reply.value(QStringLiteral("containers"))}});
            });
            return;
        }
        if (method == QStringLiteral("containers.upsert")) {
            const QJsonObject container = args.value(QStringLiteral("container")).toObject();
            if (container.isEmpty() || core_ == nullptr || !core_->isRunning()) {
                replyToPage(request, {}, QStringLiteral("nincs konténer vagy a mag nem fut"));
                return;
            }
            core_->containerUpsert(container,
                                   [this, request](const QJsonObject& saved, const QString& error) {
                                       if (!error.isEmpty()) {
                                           replyToPage(request, {}, error);
                                           return;
                                       }
                                       replyToPage(request,
                                                   QJsonObject{{QStringLiteral("container"),
                                                                saved.value(QStringLiteral("container"))}});
                                   });
            return;
        }
        if (method == QStringLiteral("containers.delete")) {
            const QString container_id = args.value(QStringLiteral("id")).toString();
            // The target of the move is optional: without it the core refuses a container that still holds cards;
            // with it the cards travel first and the container goes afterwards (ADR-054).
            const QString move_to = args.value(QStringLiteral("moveTo")).toString();
            if (container_id.isEmpty() || core_ == nullptr || !core_->isRunning()) {
                replyToPage(request, {}, QStringLiteral("nincs azonosító vagy a mag nem fut"));
                return;
            }
            core_->containerDelete(container_id, move_to,
                                   [this, request, container_id](const QJsonObject& reply,
                                                                const QString& error) {
                                       if (!error.isEmpty()) {
                                           replyToPage(request, {}, error);
                                           return;
                                       }
                                       // The container's engine profile (`<profile>/containers/<id>`) goes with it,
                                       // otherwise a deleted container leaves its cookies and cache behind (ADR-071).
                                       cleanupContainerProfile(container_id);
                                       replyToPage(request, reply);
                                   });
            return;
        }
        // ---- the workspace table (ADR-056) ------------------------------------------------------
        if (method == QStringLiteral("workspaces.list")) {
            if (core_ == nullptr || !core_->isRunning()) {
                replyToPage(request, {}, QStringLiteral("a mag nem fut"));
                return;
            }
            core_->workspacesList([this, request](const QJsonObject& reply, const QString& error) {
                if (!error.isEmpty()) {
                    replyToPage(request, {}, error);
                    return;
                }
                replyToPage(request, QJsonObject{{QStringLiteral("workspaces"),
                                                  reply.value(QStringLiteral("workspaces"))}});
            });
            return;
        }
        if (method == QStringLiteral("workspaces.upsert")) {
            const QJsonObject workspace = args.value(QStringLiteral("workspace")).toObject();
            if (workspace.isEmpty() || core_ == nullptr || !core_->isRunning()) {
                replyToPage(request, {}, QStringLiteral("nincs munkaterület vagy a mag nem fut"));
                return;
            }
            core_->workspaceUpsert(workspace,
                                   [this, request](const QJsonObject& saved, const QString& error) {
                                       if (!error.isEmpty()) {
                                           replyToPage(request, {}, error);
                                           return;
                                       }
                                       replyToPage(request,
                                                   QJsonObject{{QStringLiteral("workspace"),
                                                                saved.value(QStringLiteral("workspace"))}});
                                   });
            return;
        }
        if (method == QStringLiteral("workspaces.delete")) {
            const QString workspace_id = args.value(QStringLiteral("id")).toString();
            if (workspace_id.isEmpty() || core_ == nullptr || !core_->isRunning()) {
                replyToPage(request, {}, QStringLiteral("nincs azonosító vagy a mag nem fut"));
                return;
            }
            core_->workspaceDelete(workspace_id,
                                   [this, request](const QJsonObject& reply, const QString& error) {
                                       if (!error.isEmpty()) {
                                           replyToPage(request, {}, error);
                                           return;
                                       }
                                       replyToPage(request, reply);
                                   });
            return;
        }
        // ---- the portable bundle (ADR-059) -------------------------------------------------------
        // The path comes from the caller: the page's own button will open a dialog and hand the path here, so the
        // bridge stays free of native UI (a `QFileDialog` inside a request would block the page's answer).
        if (method == QStringLiteral("profile.export") ||
            method == QStringLiteral("profile.import")) {
            const QString path = args.value(QStringLiteral("path")).toString();
            if (path.isEmpty() || core_ == nullptr || !core_->isRunning()) {
                replyToPage(request, {}, QStringLiteral("nincs útvonal vagy a mag nem fut"));
                return;
            }
            const auto handler = [this, request](const QJsonObject& reply, const QString& error) {
                if (!error.isEmpty()) {
                    replyToPage(request, {}, error);
                    return;
                }
                replyToPage(request, reply);
            };
            if (method == QStringLiteral("profile.export")) {
                core_->profileExport(path, handler);
            } else {
                core_->profileImport(path, handler);
            }
            return;
        }
        // ---- the database (ADR-064) --------------------------------------------------------------
        if (method == QStringLiteral("db.status")) {
            if (core_ == nullptr || !core_->isRunning()) {
                replyToPage(request, {}, QStringLiteral("a mag nem fut"));
                return;
            }
            const bool sync = args.value(QStringLiteral("sync")).toBool(false);
            core_->dbStatus(sync, [this, request](const QJsonObject& reply, const QString& error) {
                if (!error.isEmpty()) {
                    replyToPage(request, {}, error);
                    return;
                }
                replyToPage(request, reply);
            });
            return;
        }
        // A deliberately unanswered method, for the timeout check: it exists only when the environment
        // asks for it, so a normal run cannot reach it and no page can hang the shell by accident.
        if (qEnvironmentVariableIsSet("ANTIKOLT_BRIDGE_TEST") &&
            method == QStringLiteral("test.hang")) {
            return;
        }
        if (method == QStringLiteral("system.language")) {
            // The page asks which language the shell speaks, so the grid's own strings agree with the chrome
            // (i18n, ADR-079). It is the shell's preference, not a page guess.
            QJsonObject data;
            data.insert(QStringLiteral("language"), language_);
            replyToPage(request, data, QString());
            return;
        }
        if (method == QStringLiteral("system.memory")) {
            // The page asks what the browser costs; the engine measures the live surface, and the budget is the
            // profile's `memory_budget_mb` (F6) — a cached setting from the core, never invented here.
            if (live_engine_ == nullptr || !live_engine_->isRunning() || surface_ == 0) {
                replyToPage(request, {}, QStringLiteral("nincs élő felület"));
                return;
            }
            live_engine_->metrics(surface_, [this, request](const QJsonObject& measured,
                                                            const QString& error) {
                if (!error.isEmpty()) {
                    replyToPage(request, {}, error);
                    return;
                }
                const qint64 used = measured.value(QStringLiteral("engine_working_set_bytes"))
                                        .toString()
                                        .toLongLong();
                // The budget bands (F6, 03-MEMORIA.md §4): a live surface above the profile's budget is the
                // signal for the media-quality limit — the page shows a "memory-saving" hint, overridable there.
                const bool over_budget = memory_budget_bytes_ > 0 && used > memory_budget_bytes_;
                // The profile's warn/hard bands (ADR-020) become a live signal here: the measured working set
                // is classed against them, so the governor (and the page) can tell "fine" from "under pressure".
                const QString pressure = memoryPressureName(used, memory_warn_bytes_, memory_hard_bytes_);
                const bool preview_paused = preview_paused_until_ms_ > 0 &&
                                            QDateTime::currentMSecsSinceEpoch() < preview_paused_until_ms_;
                replyToPage(request,
                            QJsonObject{{QStringLiteral("usedBytes"), static_cast<double>(used)},
                                        {QStringLiteral("budgetBytes"),
                                         static_cast<double>(memory_budget_bytes_)},
                                        {QStringLiteral("overBudget"), over_budget},
                                        {QStringLiteral("pressure"), pressure},
                                        {QStringLiteral("previewPaused"), preview_paused}});
            });
            return;
        }
        replyToPage(request, {}, QStringLiteral("ismeretlen híd-kérdés: %1").arg(method));
        return;
    }
    // An unknown method is a version mismatch, not a crash: say so and keep the surface alive.
    setStatus(QStringLiteral("ismeretlen híd-hívás: %1").arg(method));
}

void MainWindow::replyToPage(qint64 request, const QJsonObject& json, const QString& error) {
    if (request <= 0 || engine_ == nullptr) {
        return;
    }
    // The set is the single source of truth: a second answer (the watchdog and a late path, or two
    // paths) is dropped instead of confusing the engine's request table.
    if (!awaiting_page_reply_.remove(request)) {
        return;
    }
    engine_->bridgeReply(request, json, error);
}

void MainWindow::pushCardsToGrid(qint64 surface) {
    if (surface == 0 || core_ == nullptr || !core_->isRunning()) {
        return;  // no core: the page keeps its own sample cards and the status bar already said so
    }
    core_->cardsList([this, surface](const QJsonObject& reply, const QString& error) {
        if (!error.isEmpty()) {
            setStatus(QStringLiteral("a kártyalista nem érkezett meg: %1 — a rács a mintákat mutatja")
                          .arg(error));
            return;
        }
        const QJsonArray cards = reply.value(QStringLiteral("cards")).toArray();
        QJsonObject payload;
        payload.insert(QStringLiteral("cards"), cards);
        const QJsonDocument document(payload);
        const QString script =
            QStringLiteral("window.antikoltAcceptCards(JSON.parse(\"%1\"))")
                .arg(asJavaScriptStringLiteral(
                    QString::fromUtf8(document.toJson(QJsonDocument::Compact))));
        engine_->evaluate(surface, script, [this, count = cards.size()](const QJsonObject&,
                                                                       const QString& eval_error) {
            if (!eval_error.isEmpty()) {
                setStatus(QStringLiteral("a kártyalista nem ment át a lapra: %1").arg(eval_error));
                return;
            }
            setStatus(QStringLiteral("kártyalista a magtól: %1 kártya").arg(count));
        });
    });
        // The container table travels with the cards, not on the page's own request: the bridge script is installed
        // on load *end*, which can be after the page's deferred script runs, and a table that arrives too late
        // left the legend drawing the fallback (measured: the page showed four containers while the core had
        // five). A push has no such race (ADR-052).
        core_->containersList([this, surface](const QJsonObject& table_reply, const QString& table_error) {
            if (!table_error.isEmpty()) {
                setStatus(QStringLiteral("a konténer-tábla nem érkezett meg: %1").arg(table_error));
                return;
            }
            QJsonObject table_payload;
            table_payload.insert(QStringLiteral("containers"),
                                 table_reply.value(QStringLiteral("containers")));
            const QJsonDocument table_document(table_payload);
            const QString table_script =
                QStringLiteral("window.antikoltAcceptContainers(JSON.parse(\"%1\"))")
                    .arg(asJavaScriptStringLiteral(
                        QString::fromUtf8(table_document.toJson(QJsonDocument::Compact))));
            engine_->evaluate(surface, table_script, [](const QJsonObject&, const QString&) {});
        });
        // The workspace table travels the same way, and for the same reason (ADR-056): the tabs are the core's, and
        // a push has no race with the page's own script.
        core_->workspacesList([this, surface](const QJsonObject& spaces_reply,
                                              const QString& spaces_error) {
            if (!spaces_error.isEmpty()) {
                setStatus(
                    QStringLiteral("a munkaterület-tábla nem érkezett meg: %1").arg(spaces_error));
                return;
            }
            QJsonObject spaces_payload;
            spaces_payload.insert(QStringLiteral("workspaces"),
                                  spaces_reply.value(QStringLiteral("workspaces")));
            const QJsonDocument spaces_document(spaces_payload);
            const QString spaces_script =
                QStringLiteral("window.antikoltAcceptWorkspaces(JSON.parse(\"%1\"))")
                    .arg(asJavaScriptStringLiteral(
                        QString::fromUtf8(spaces_document.toJson(QJsonDocument::Compact))));
            engine_->evaluate(surface, spaces_script, [](const QJsonObject&, const QString&) {});
        });

}
void MainWindow::loadHome() {
    rememberScroll([this] {
    if (!engine_->isRunning()) {
        setStatus(QStringLiteral("a motor még nem fut — nyomd meg a „Motor indítása” gombot"));
        return;
    }
    // Going home means leaving whatever card is open, so no pending URL and no live card id may survive
    // this: a stale id would later be saved as the grid's offset (measured: 700 became 0).
    pending_url_.clear();
    pending_title_.clear();
    current_card_id_.clear();
    ensureSurface();
    });
}

void MainWindow::freezeCard() {
    if (live_engine_ == nullptr || !live_engine_->isRunning() || !card_->hasSurface()) {
        setStatus(QStringLiteral("nincs mit fagyasztani"));
        return;
    }
    // Freezing leaves the grid behind too, so a worker has no business running through it.
    if (worker_state_ != antikolt::PreviewWorker::Idle) {
        abortPreviewWorker(QStringLiteral("a kártya lemezre megy"));
    }
    const qint64 surface = card_->surface();
    // Before the surface goes away: the page's own text, so a search can find a word from the article and not
    // only from the title (ADR-049). Capped here *and* in the store, because a page must not be able to grow the
    // card list. A card with no id yet (the grid) has nothing to attach it to.
    if (core_ != nullptr && core_->isRunning() && !current_card_id_.isEmpty()) {
        live_engine_->evaluate(
            surface,
            QStringLiteral("(function(){var t=(document.body&&document.body.innerText)||'';"
                           "return t.replace(/\\s+/g,' ').trim().slice(0,8000);})()"),
            [this](const QJsonObject& reply, const QString& error) {
                if (!error.isEmpty() || current_card_id_.isEmpty()) {
                    return;
                }
                const QString text = reply.value(QStringLiteral("value")).toString();
                if (text.isEmpty()) {
                    return;
                }
                core_->cardPageText(
                    current_card_id_, text,
                    [](const QJsonObject&, const QString& failure) {
                        if (!failure.isEmpty()) {
                            qWarning("[shell] oldalszöveg mentése nem sikerült: %s",
                                     qPrintable(failure));
                        }
                    });
            });
    }
    // Before the surface goes away: its working set, so the grid can show per-card memory (F6). Measured at the
    // freeze boundary, not continuously — a background measurement would itself cost memory it claims to save.
    if (core_ != nullptr && core_->isRunning() && !current_card_id_.isEmpty()) {
        live_engine_->metrics(surface, [this](const QJsonObject& measured, const QString& error) {
            if (!error.isEmpty() || current_card_id_.isEmpty()) {
                return;
            }
            const qint64 bytes =
                measured.value(QStringLiteral("engine_working_set_bytes")).toString().toLongLong();
            if (bytes > 0) {
                core_->cardMemory(current_card_id_, bytes, [](const QJsonObject&, const QString&) {});
            }
        });
    }
    live_engine_->snapshot(surface, [this, surface](const QJsonObject& result, const QString& error) {
        if (!error.isEmpty()) {
            return_to_grid_ = false;
            setStatus(QStringLiteral("fagyasztás hiba: %1").arg(error));
            return;
        }
        setStatus(QStringLiteral("pillanatkép: %1 bájt · állapot %2 bájt · motor leállítása")
                      .arg(result.value(QStringLiteral("bytes")).toInt())
                      .arg(result.value(QStringLiteral("state_bytes")).toInt()));
        // Order matters: the state is on disk before the surface goes away.
        card_->detach();
        // The grid comes back only once the old surface is really gone: asking for a new surface
        // while the old one still exists is answered with `engine_busy`, and the grid then never
        // loads (measured: create_surface racing destroy_surface, grid_returned=FAILED).
        live_engine_->destroySurface(surface, [this](const QJsonObject&, const QString&) {
            if (return_to_grid_) {
                return_to_grid_ = false;
                loadHome();
            }
        });
    });
}

void MainWindow::showSessions() {
    if (sessions_dock_ == nullptr) {
        sessions_dock_ = new QDockWidget(QStringLiteral("Munkamenetek"), this);
        sessions_dock_->setObjectName(QStringLiteral("sessions"));
        auto* panel = new QWidget(sessions_dock_);
        auto* layout = new QVBoxLayout(panel);
        auto* cleanup = new QPushButton(QStringLiteral("Takarítás most"), panel);
        cleanup->setToolTip(QStringLiteral(
            "A mag által esedékesnek jelölt originök törlése (ADR-033). Ami épp nyitva van, azonnal; a "
            "többi a kártya következő megnyitásakor."));
        connect(cleanup, &QPushButton::clicked, this, &MainWindow::cleanupSessions);
        layout->addWidget(cleanup);
        sessions_table_ = new QTableWidget(0, 6, panel);
        sessions_table_->setHorizontalHeaderLabels(
            {QStringLiteral("Origin"), QStringLiteral("Állapot"), QStringLiteral("Politika"),
             QStringLiteral("Sütik"), QStringLiteral("Lejárt"), QStringLiteral("Törlésnél")});
        sessions_table_->verticalHeader()->setVisible(false);
        sessions_table_->horizontalHeader()->setStretchLastSection(true);
        sessions_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        sessions_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        // The policy is the core's, and so is the decision — but the user must be able to *set* it
        // (ADR-039). A TTL in hours is the unit a person thinks in; the core stores milliseconds.
        auto* rules = new QWidget(panel);
        auto* rules_layout = new QHBoxLayout(rules);
        rules_layout->setContentsMargins(0, 0, 0, 0);
        rules_layout->addWidget(new QLabel(QStringLiteral("Lejárat (óra):"), rules));
        sessions_ttl_ = new QSpinBox(rules);
        sessions_ttl_->setRange(1, 720);
        sessions_ttl_->setValue(24);
        rules_layout->addWidget(sessions_ttl_);
        sessions_lock_wipe_ = new QCheckBox(QStringLiteral("App-zárnál törlés"), rules);
        rules_layout->addWidget(sessions_lock_wipe_);
        auto* save = new QPushButton(QStringLiteral("Mentés"), rules);
        save->setObjectName(QStringLiteral("sessionRulesSave"));
        connect(save, &QPushButton::clicked, this, &MainWindow::saveSessionRules);
        rules_layout->addWidget(save);
        layout->addWidget(rules);
        layout->addWidget(sessions_table_);
        sessions_dock_->setWidget(panel);
        // A companion of what is on screen, not a modal interruption of it.
        addDockWidget(Qt::RightDockWidgetArea, sessions_dock_);
    }
    sessions_dock_->show();
    sessions_dock_->raise();
    refreshSessions();
}

void MainWindow::saveSessionRules() {
    if (core_ == nullptr || !core_->isRunning()) {
        setStatus(QStringLiteral("a mag nem fut, a szabályok nem menthetők"));
        return;
    }
    const qint64 hours = (sessions_ttl_ != nullptr) ? sessions_ttl_->value() : 24;
    const bool lock_wipe = (sessions_lock_wipe_ != nullptr) && sessions_lock_wipe_->isChecked();
    // The core multiplies nothing and decides nothing: it validates and stores what it is told (ADR-033).
    QJsonObject rules;
    rules.insert(QStringLiteral("ttl_ms"), static_cast<double>(hours * 3600 * 1000));
    rules.insert(QStringLiteral("wipe_on_app_lock"), lock_wipe);
    rules.insert(QStringLiteral("container_default"), session_policy_);
    core_->sessionRules(rules, [this](const QJsonObject& saved, const QString& error) {
        if (!error.isEmpty()) {
            setStatus(QStringLiteral("a szabályok nem mentődtek: %1").arg(error));
            return;
        }
        const QJsonObject stored = saved.value(QStringLiteral("rules")).toObject();
        setStatus(QStringLiteral("munkamenet-szabályok: %1 óra%2")
                      .arg(static_cast<qint64>(stored.value(QStringLiteral("ttl_ms")).toDouble() /
                                               3600000.0))
                      .arg(stored.value(QStringLiteral("wipe_on_app_lock")).toBool()
                               ? QStringLiteral(", app-zárnál törlés")
                               : QString()));
        refreshSessions();
    });
}

void MainWindow::setZoomPercent(int percent) {
    const int clamped = std::clamp(percent, 25, 500);
    zoom_percent_ = clamped;
    if (zoom_label_ != nullptr) {
        zoom_label_->setText(QStringLiteral("🔍 %1%").arg(clamped));
    }
    if (live_engine_ == nullptr || !live_engine_->isRunning() || surface_ == 0) {
        setStatus(QStringLiteral("nagyítás: %1% (nincs élő felület)").arg(clamped));
        return;
    }
    live_engine_->setZoom(surface_, clamped, [this, clamped](const QJsonObject&, const QString& error) {
        setStatus(error.isEmpty() ? QStringLiteral("nagyítás: %1%").arg(clamped)
                                  : QStringLiteral("nagyítás hiba: %1").arg(error));
    });
}

void MainWindow::setPageMedia(const QString& media) {
    if (media != QStringLiteral("auto") && media != QStringLiteral("dark") &&
        media != QStringLiteral("light")) {
        return;
    }
    page_media_ = media;
    if (live_engine_ == nullptr || !live_engine_->isRunning() || surface_ == 0) {
        setStatus(QStringLiteral("oldal-téma: %1 (nincs élő felület)").arg(media));
        return;
    }
    live_engine_->setMedia(surface_, media, [this, media](const QJsonObject&, const QString& error) {
        setStatus(error.isEmpty() ? QStringLiteral("oldal-téma: %1").arg(media)
                                  : QStringLiteral("oldal-téma hiba: %1").arg(error));
    });
}

QStringList MainWindow::pageDarkModes() {
    // Order is the menu's order and the test's order: the darkest first, because that is what most people mean by
    // "dark pages"; the softer ones follow for long reading.
    return {QStringLiteral("off"), QStringLiteral("invert"), QStringLiteral("dim"),
            QStringLiteral("warm")};
}

QToolButton* MainWindow::themeButton(const QString& theme) const {
    return theme_buttons_.value(theme, nullptr);
}

void MainWindow::setPageDarkMode(const QString& mode) {
    if (!pageDarkModes().contains(mode)) {
        return;
    }
    page_dark_mode_ = mode;
    // Remembered across runs: a user who asked for dark pages should not have to ask again at every launch.
    // The old boolean key is migrated once, so a profile from the previous build keeps its choice.
    QSettings settings;
    settings.setValue(QStringLiteral("ui/page_dark_mode"), page_dark_mode_);
    settings.remove(QStringLiteral("ui/forced_dark"));
    if (page_dark_button_ != nullptr) {
        // The button's face and tooltip are the state: the mode is visible without opening the menu.
        page_dark_button_->setIcon(PageDarkIcon(page_dark_mode_));
        page_dark_button_->setToolTip(
            QStringLiteral("Oldalak sötétítése: %1\nA weboldal saját sötét témája nélkül is sötét lesz.")
                .arg(pageDarkModeLabel(page_dark_mode_)));
        if (page_dark_button_->menu() != nullptr) {
            for (QAction* action : page_dark_button_->menu()->actions()) {
                action->setChecked(action->objectName() ==
                                   QStringLiteral("pageDarkMode_") + page_dark_mode_);
            }
        }
    }
    if (live_engine_ == nullptr || !live_engine_->isRunning() || surface_ == 0) {
        setStatus(QStringLiteral("sötét oldalak: %1 (nincs élő felület)")
                      .arg(pageDarkModeLabel(page_dark_mode_)));
        return;
    }
    live_engine_->setPageDarkMode(surface_, page_dark_mode_,
                                  [this](const QJsonObject&, const QString& error) {
                                 setStatus(error.isEmpty()
                                               ? QStringLiteral("sötét oldalak: %1").arg(
                                                     pageDarkModeLabel(page_dark_mode_))
                                               : QStringLiteral("sötét oldalak hiba: %1").arg(error));
                             });
}

QString MainWindow::scrollBookPath() const {
    return engine_profile_.isEmpty() ? QString()
                                     : engine_profile_ + QStringLiteral("/scroll.json");
}

void MainWindow::rememberScroll(std::function<void()> then) {
    // The grid is not a card: after going home the live surface is the grid while the card id is stale, and
    // saving then overwrites the place the reader left with the grid's zero (measured: 700 became 0).
    if (live_engine_ == nullptr || !live_engine_->isRunning() || surface_ == 0 ||
        current_card_id_.isEmpty() || surface_ == home_surface_ ||
        engine_profile_.isEmpty()) {
        if (then) {
            then();
        }
        return;
    }
    const QString card_id = current_card_id_;
    // The caller continues only after the offset is stored: an eval racing the navigation that follows would
    // read the *next* page's zero, and the remembered place would silently be 0 (measured exactly that).
    live_engine_->evaluate(surface_, QStringLiteral("String(Math.round(window.scrollY))"),
                      [this, card_id, then](const QJsonObject& reply, const QString& error) {
                          if (error.isEmpty()) {
                              const int y =
                                  reply.value(QStringLiteral("value")).toString().toInt();
                              antikolt::saveScrollPosition(scrollBookPath(), card_id, y);
                          }
                          if (then) {
                              then();
                          }
                      });
}
void MainWindow::producePreview(qint64 surface) {
    if (pending_card_id_.isEmpty() || engine_profile_.isEmpty() || previews_made_ >= kPreviewCap) {
        return;
    }
    if (core_ == nullptr || !core_->isRunning()) {
        return;
    }
    const QString card_id = pending_card_id_;
    pending_card_id_.clear();
    qInfo("[shell] preview: producing for %s (surface %lld)", qUtf8Printable(card_id),
          static_cast<long long>(surface));
    core_->cardsList([this, card_id, surface](const QJsonObject& listed, const QString& error) {
        if (!error.isEmpty()) {
            return;
        }
        QJsonObject record;
        for (const QJsonValue& value : listed.value(QStringLiteral("cards")).toArray()) {
            const QJsonObject card = value.toObject();
            if (card.value(QStringLiteral("id")).toString() == card_id) {
                record = card;
                break;
            }
        }
        if (record.isEmpty()) {
            qInfo("[shell] preview: no record for %s", qUtf8Printable(card_id));
            return;  // unknown card: there is nothing to remember a picture on
        }
        // A card keeps its picture until the picture is *old*: a preview is a photograph of a page, and pages
        // change. Age comes from the file's own modification time (previews.h), so there is no second
        // timestamp to keep in sync; a missing file is always "needs refresh". The name is content-addressed
        // (ADR-077), so the freshness check looks up the *existing* preview by its stored name.
        const QString existing_name = record.value(QStringLiteral("preview_ref")).toString();
        if (!existing_name.isEmpty()) {
            const QString existing_target = engine_profile_ + QStringLiteral("/previews/") + existing_name;
            if (!antikolt::previewNeedsRefresh(existing_target, QDateTime::currentMSecsSinceEpoch(),
                                               kPreviewMaxAgeMs)) {
                return;  // fresh enough; the shell is not a screenshot loop
            }
        }
        // The daily budget outlives this run (a file in the profile), because a limit that resets on every
        // launch is not a limit. A refused preview is not an error: the card keeps whatever it had.
        const QString budget = engine_profile_ + QStringLiteral("/previews/budget.json");
        const QString today = QDate::currentDate().toString(Qt::ISODate);
        if (!antikolt::previewBudgetAllows(budget, today, kPreviewDailyMax)) {
            qInfo("[shell] preview: daily budget spent (%d)", kPreviewDailyMax);
            return;
        }
        const auto count_budget = [budget, today] { antikolt::previewConsumeBudget(budget, today); };
        // The record learns the content name; the grid shows the picture the next time it loads, because the
        // push carries the record and the engine serves the file (ADR-035).
        const auto remember = [this, record](const QString& name) {
            QJsonObject updated = record;
            updated.insert(QStringLiteral("preview_ref"), name);
            core_->cardUpsert(updated, [this, name](const QJsonObject&, const QString& upsert_error) {
                if (!upsert_error.isEmpty()) {
                    qInfo("[shell] preview: upsert failed: %s", qUtf8Printable(upsert_error));
                    setStatus(QStringLiteral("az előnézet neve nem mentődött: %1").arg(upsert_error));
                    return;
                }
                ++previews_made_;
                qInfo("[shell] preview: record updated for %s", qUtf8Printable(name));
                setStatus(QStringLiteral("előnézet kész: %1").arg(name));
            });
        };
        // A capture can stall behind a GPU hiccup (measured: `Page.captureScreenshot` sometimes answers only
        // after the GPU process has restarted twice), so one retry is cheaper than a lost preview. The
        // engine's own request timeout bounds each attempt; after the last one the preview is skipped and
        // the status line says so — a failed preview is not a failed browser.
        const auto capture_to = [this, surface, remember, count_budget](auto&& self, int attempt) -> void {
            live_engine_->capture(
                surface, QStringLiteral("png"), 80,
                [this, remember, count_budget, self, attempt](const QJsonObject& shot,
                                                        const QString& shot_error) {
                    const QString written = shot.value(QStringLiteral("path")).toString();
                    if (!shot_error.isEmpty() || written.isEmpty()) {
                        qInfo("[shell] preview: capture attempt %d failed: %s", attempt,
                              qUtf8Printable(shot_error.isEmpty()
                                                 ? QStringLiteral("no path in the reply")
                                                 : shot_error));
                        if (attempt < kPreviewAttempts) {
                            QTimer::singleShot(1500, this, [self, attempt] {
                                self(self, attempt + 1);
                            });
                        }
                        return;
                    }
                    // The name is the content (ADR-077): hash the captured bytes, then place the file under
                    // that name. Identical bytes already on disk are a shared file, not a second copy.
                    QFile shot_file(written);
                    if (!shot_file.open(QIODevice::ReadOnly)) {
                        qInfo("[shell] preview: cannot read the capture %s", qUtf8Printable(written));
                        setStatus(QStringLiteral("az előnézet nem olvasható: %1").arg(written));
                        return;
                    }
                    const QByteArray bytes = shot_file.readAll();
                    shot_file.close();
                    const QString name = antikolt::previewContentName(bytes, QStringLiteral("png"));
                    const QString target = engine_profile_ + QStringLiteral("/previews/") + name;
                    if (!QFileInfo::exists(target)) {
                        // `capture` writes into `screenshots/`; the preview belongs in `previews/` under its
                        // content name, so the engine's scheme handler finds it.
                        if (!QFile::copy(written, target)) {
                            qInfo("[shell] preview: copy failed to %s", qUtf8Printable(target));
                            setStatus(QStringLiteral("az előnézet nem került a profilba: %1").arg(target));
                            return;
                        }
                        qInfo("[shell] preview: copied to %s", qUtf8Printable(target));
                    } else {
                        qInfo("[shell] preview: %s already on disk (shared)", qUtf8Printable(name));
                    }
                    count_budget();
                    remember(name);
                });
        };
        capture_to(capture_to, 1);
    });
}

bool MainWindow::previewWorkerAutoEnabled() {
    return !qEnvironmentVariableIsSet("ANTIKOLT_NO_PREVIEW_WORKER");
}

bool MainWindow::gridIsShowing() const {
    // The id is tracked, not guessed: `surface_` is whatever is attached, and the grid is the surface the
    // window is *calling* home. A card that is being opened has already cleared both.
    return card_ != nullptr && card_->hasSurface() && surface_ != 0 && surface_ == home_surface_;
}

bool MainWindow::startPreviewWorker(const QString& cardId, const QString& url, const QString& container) {
    // The worker photographs a card, so it runs in that card's container engine: the preview sees the page
    // exactly as the card would — the container's proxy, cookies and permission policy (ADR-071). A card
    // without a container runs in the home engine.
    if (QDateTime::currentMSecsSinceEpoch() < preview_paused_until_ms_) {
        return false;
    }
    worker_engine_ = engineFor(container);
    if (worker_engine_ == nullptr || !worker_engine_->isRunning() || engine_profile_.isEmpty()) {
        return false;
    }
    const QString budget = engine_profile_ + QStringLiteral("/previews/budget.json");
    const QString today = QDate::currentDate().toString(Qt::ISODate);
    // The four conditions are previews.h's, not this function's: the window must not be able to forget
    // one of them (ADR-041, point 1). The fifth is the profile's: Saver/Ultra turn background preview
    // refresh off (ADR-020 `preview_refresh`), and the worker must honour that too.
    const bool budget_ok = antikolt::previewBudgetAllows(budget, today, kPreviewDailyMax);
    const bool has_candidate =
        !cardId.isEmpty() && !url.isEmpty() && previews_made_ < kPreviewCap;
    if (!preview_refresh_ ||
        !antikolt::previewWorkerCanStart(worker_state_, !gridIsShowing(), budget_ok, has_candidate)) {
        return false;
    }

    worker_state_ = antikolt::PreviewWorker::Loading;
    worker_card_id_ = cardId;
    worker_page_url_ = url;
    // The preview name is the *content*, not the card id (ADR-077): it is computed once the capture lands,
    // in `onPreviewWorkerLoaded`, where the bytes exist to hash.
    worker_surface_ = 0;
    worker_deadline_->start(workerDeadlineMs());
    qInfo("[shell] preview worker: %s", qUtf8Printable(cardId));

    // Its own partition (`persist:preview`), so a preview never touches a card's cookies, and hidden, so a
    // window never appears over the grid while the user is reading it (ADR-041, point 2).
    // The engine pointer is captured by value: `finishPreviewWorker` nulls `worker_engine_`, and the reply
    // can arrive after the deadline already fired — reading the member then would dereference null.
    EngineClient* engine = worker_engine_;
    engine->createSurface(
        QStringLiteral("persist:preview"), kWorkerViewportWidth, kWorkerViewportHeight, 100, 0,
        [this, engine](const QJsonObject& reply, const QString& error) {
            if (!error.isEmpty()) {
                finishPreviewWorker(
                    false, QStringLiteral("a munkás felülete nem jött létre: %1").arg(error));
                return;
            }
            const qint64 created =
                static_cast<qint64>(reply.value(QStringLiteral("surface")).toDouble());
            if (created <= 0) {
                finishPreviewWorker(false, QStringLiteral("a motor nem adott felület-azonosítót"));
                return;
            }
            // Remembered before the state test: a job that was abandoned while the engine was creating the
            // surface still has to have that surface destroyed, and the self test proves it was.
            worker_last_surface_ = created;
            if (worker_state_ == antikolt::PreviewWorker::Idle) {
                // The job was abandoned while the engine was creating the surface (the user opened a card):
                // the fresh surface belongs to nobody, so it is destroyed here instead of leaking.
                engine->destroySurface(created, nullptr);
                return;
            }
            worker_surface_ = created;
            engine->navigate(created, worker_page_url_,
                             [this](const QJsonObject&, const QString& navigate_error) {
                                 if (navigate_error.isEmpty()) {
                                     return;
                                 }
                                 if (worker_state_ == antikolt::PreviewWorker::Loading) {
                                     finishPreviewWorker(
                                         false,
                                         QStringLiteral("navigáció: %1").arg(navigate_error));
                                 }
                             });
        },
        /*hidden=*/true);
    return true;
}

void MainWindow::onPreviewWorkerLoaded() {
    if (worker_state_ != antikolt::PreviewWorker::Loading) {
        return;  // a second load event is not a second job
    }
    worker_state_ = antikolt::PreviewWorker::Capturing;
    qInfo("[shell] preview worker: %s betöltve, felvétel", qUtf8Printable(worker_card_id_));
    worker_engine_->capture(
        worker_surface_, QStringLiteral("png"), 80,
        [this](const QJsonObject& shot, const QString& error) {
            // A job that ended while the capture was in flight (deadline, or the user opened a card) must
            // not write a record afterwards: the state is the single source of truth.
            if (worker_state_ != antikolt::PreviewWorker::Capturing) {
                return;
            }
            const QString written = shot.value(QStringLiteral("path")).toString();
            if (!error.isEmpty() || written.isEmpty()) {
                finishPreviewWorker(false,
                                    QStringLiteral("felvétel: %1")
                                        .arg(error.isEmpty() ? QStringLiteral("nincs fájl")
                                                             : error));
                return;
            }
            // The name is the content (ADR-077): hash the captured bytes, then place the file under that
            // name. Identical bytes already on disk are a shared file, not a second copy.
            QFile shot_file(written);
            if (!shot_file.open(QIODevice::ReadOnly)) {
                finishPreviewWorker(false, QStringLiteral("a felvétel nem olvasható"));
                return;
            }
            const QByteArray bytes = shot_file.readAll();
            shot_file.close();
            worker_preview_name_ = antikolt::previewContentName(bytes, QStringLiteral("png"));
            const QString target =
                engine_profile_ + QStringLiteral("/previews/") + worker_preview_name_;
            if (!QFileInfo::exists(target)) {
                // `capture` writes into `screenshots/`; the preview belongs under its content name, in the
                // directory the engine's `antikolt://blob/` handler serves (ADR-035).
                if (!QFile::copy(written, target)) {
                    finishPreviewWorker(false, QStringLiteral("a fájl nem került a profilba"));
                    return;
                }
                qInfo("[shell] preview worker: kép a profilban (%s)",
                      qUtf8Printable(worker_preview_name_));
            } else {
                qInfo("[shell] preview worker: %s már a lemezen (megosztott)",
                      qUtf8Printable(worker_preview_name_));
            }
            // Counted only once a picture exists: a budget spent on a failure would be a lie.
            const QString budget = engine_profile_ + QStringLiteral("/previews/budget.json");
            antikolt::previewConsumeBudget(budget, QDate::currentDate().toString(Qt::ISODate));
            if (core_ == nullptr || !core_->isRunning()) {
                finishPreviewWorker(true, QString());  // the file is a preview, the record can catch up
                return;
            }
            // The record learns the name; the grid shows the picture the next time it loads, because the
            // push carries the record and the engine serves the file (ADR-035).
            const QString card_id = worker_card_id_;
            core_->cardsList([this, card_id](const QJsonObject& listed, const QString& list_error) {
                if (worker_state_ != antikolt::PreviewWorker::Capturing) {
                    return;
                }
                if (!list_error.isEmpty()) {
                    finishPreviewWorker(false, QStringLiteral("a kártyalista nem jött meg: %1").arg(list_error));
                    return;
                }
                QJsonObject record;
                for (const QJsonValue& value : listed.value(QStringLiteral("cards")).toArray()) {
                    const QJsonObject card = value.toObject();
                    if (card.value(QStringLiteral("id")).toString() == card_id) {
                        record = card;
                        break;
                    }
                }
                if (record.isEmpty()) {
                    finishPreviewWorker(false, QStringLiteral("nincs ilyen kártya: %1").arg(card_id));
                    return;
                }
                QJsonObject updated = record;
                updated.insert(QStringLiteral("preview_ref"), worker_preview_name_);
                core_->cardUpsert(updated, [this](const QJsonObject&, const QString& upsert_error) {
                    if (worker_state_ != antikolt::PreviewWorker::Capturing) {
                        return;
                    }
                    if (!upsert_error.isEmpty()) {
                        finishPreviewWorker(
                            false, QStringLiteral("a rekord nem mentődött: %1").arg(upsert_error));
                        return;
                    }
                    ++previews_made_;
                    setStatus(QStringLiteral("háttér-előnézet kész: %1").arg(worker_preview_name_));
                    finishPreviewWorker(true, QString());
                });
            });
        });
}

void MainWindow::finishPreviewWorker(bool produced, const QString& reason) {
    if (worker_state_ == antikolt::PreviewWorker::Idle) {
        return;
    }
    const QString card_id = worker_card_id_;
    const qint64 surface = worker_surface_;
    // Idle first: every late reply (capture, record, load) is recognised as belonging to a dead job.
    worker_state_ = antikolt::PreviewWorker::Idle;
    worker_surface_ = 0;
    worker_card_id_.clear();
    worker_page_url_.clear();
    worker_preview_name_.clear();
    if (worker_deadline_ != nullptr) {
        worker_deadline_->stop();
    }
    if (surface > 0 && worker_engine_ != nullptr && worker_engine_->isRunning()) {
        worker_engine_->destroySurface(surface, nullptr);
    }
    worker_engine_ = nullptr;
    if (!produced) {
        qInfo("[shell] preview worker: leállítva (%s)", qUtf8Printable(reason));
        if (!reason.isEmpty()) {
            setStatus(QStringLiteral("háttér-előnézet kimaradt: %1").arg(reason));
        }
    }
    emit previewWorkerFinished(card_id, produced);
}

void MainWindow::abortPreviewWorker(const QString& reason) {
    if (worker_state_ == antikolt::PreviewWorker::Idle) {
        return;
    }
    // The rule is previews.h's: a worker that is running yields to a live card, always.
    if (!antikolt::previewWorkerMustAbort(worker_state_, /*has_live_card=*/true)) {
        return;
    }
    finishPreviewWorker(false, reason);
}

void MainWindow::schedulePreviewWork() {
    if (!previewWorkerAutoEnabled() || worker_state_ != antikolt::PreviewWorker::Idle) {
        return;
    }
    if (!gridIsShowing() || engine_ == nullptr || !engine_->isRunning() || core_ == nullptr ||
        !core_->isRunning() || engine_profile_.isEmpty() || previews_made_ >= kPreviewCap) {
        return;
    }
    const QString budget = engine_profile_ + QStringLiteral("/previews/budget.json");
    if (!antikolt::previewBudgetAllows(budget, QDate::currentDate().toString(Qt::ISODate),
                                       kPreviewDailyMax)) {
        return;
    }
    core_->cardsList([this](const QJsonObject& listed, const QString& error) {
        if (!error.isEmpty() || !gridIsShowing() ||
            worker_state_ != antikolt::PreviewWorker::Idle) {
            return;  // the world moved on while the core answered
        }
        const QJsonArray cards = listed.value(QStringLiteral("cards")).toArray();
        // Which card is worth a picture is `previews.h`'s decision: missing first, then stale, fresh never,
        // and a page that cannot be photographed is not a candidate at all.
        const QString next = antikolt::nextPreviewCard(
            cards, engine_profile_ + QStringLiteral("/previews"),
            QDateTime::currentMSecsSinceEpoch(), kPreviewMaxAgeMs);
        if (next.isEmpty()) {
            return;
        }
        QString url;
        QString container;
        for (const QJsonValue& value : cards) {
            const QJsonObject card = value.toObject();
            if (card.value(QStringLiteral("id")).toString() == next) {
                url = card.value(QStringLiteral("url")).toString();
                container = card.value(QStringLiteral("container")).toString();
                break;
            }
        }
        startPreviewWorker(next, url, container);
    });
}

void MainWindow::cleanupPreviews() {
    if (core_ == nullptr || !core_->isRunning() || engine_profile_.isEmpty()) {
        return;
    }
    // The core owns the cards, so it decides which names are still referenced; the shell only decides which
    // files are unreachable. A preview whose card is gone is a file nobody can ever see again.
    core_->cardsList([this](const QJsonObject& listed, const QString& error) {
        if (!error.isEmpty()) {
            return;
        }
        QSet<QString> live_names;
        for (const QJsonValue& value : listed.value(QStringLiteral("cards")).toArray()) {
            const QString name = value.toObject().value(QStringLiteral("preview_ref")).toString();
            if (!name.isEmpty()) {
                live_names.insert(name);
            }
        }
        const QStringList removed = antikolt::removeOrphanPreviews(
            engine_profile_ + QStringLiteral("/previews"), live_names);
        if (!removed.isEmpty()) {
            qInfo("[shell] preview: %lld orphan file(s) removed", static_cast<long long>(removed.size()));
            setStatus(QStringLiteral("%1 árva előnézet törölve").arg(removed.size()));
        }
    });
}

void MainWindow::refreshSessions() {
    if (sessions_table_ == nullptr) {
        return;
    }
    if (core_ == nullptr || !core_->isRunning()) {
        sessions_table_->setRowCount(0);
        setStatus(QStringLiteral("a munkamenet-naplóhoz előbb indítsd el a magot"));
        return;
    }
    core_->sessionList([this](const QJsonObject& listed, const QString& error) {
        if (!error.isEmpty()) {
            setStatus(QStringLiteral("a munkamenet-napló nem érkezett meg: %1").arg(error));
            return;
        }
        // The editors show what the core holds, not what the shell remembers: the panel is a view of the
        // policy, so an edit that was refused never looks saved (ADR-039). The container default travels
        // back unchanged on save, because the shell has no opinion about it.
        const QJsonObject rules = listed.value(QStringLiteral("rules")).toObject();
        session_policy_ = rules.value(QStringLiteral("container_default")).toString();
        if (session_policy_.isEmpty()) {
            session_policy_ = QStringLiteral("sweep");
        }
        if (sessions_ttl_ != nullptr) {
            const qint64 ttl_ms = static_cast<qint64>(rules.value(QStringLiteral("ttl_ms")).toDouble());
            if (ttl_ms > 0) {
                sessions_ttl_->setValue(static_cast<int>(std::lround(ttl_ms / 3600000.0)));
            }
        }
        if (sessions_lock_wipe_ != nullptr) {
            sessions_lock_wipe_->setChecked(rules.value(QStringLiteral("wipe_on_app_lock")).toBool());
        }
        const QJsonArray rows = listed.value(QStringLiteral("sessions")).toArray();
        // What the policy would do right now, asked from the core that owns the clock and the rules:
        // the shell does not recompute the TTL (ADR-033).
        core_->sessionDue(
            QDateTime::currentMSecsSinceEpoch(), false,
            [this, rows](const QJsonObject& due, const QString& due_error) {
                QSet<QString> due_origins;
                if (due_error.isEmpty()) {
                    for (const QJsonValue& value :
                         due.value(QStringLiteral("due")).toArray()) {
                        const QJsonObject entry = value.toObject();
                        due_origins.insert(entry.value(QStringLiteral("session"))
                                               .toObject()
                                               .value(QStringLiteral("origin"))
                                               .toString());
                    }
                }
                sessions_table_->setRowCount(rows.size());
                int row = 0;
                for (const QJsonValue& value : rows) {
                    const QJsonObject entry = value.toObject();
                    const QString origin = entry.value(QStringLiteral("origin")).toString();
                    sessions_table_->setItem(row, 0, new QTableWidgetItem(origin));
                    sessions_table_->setItem(
                        row, 1,
                        new QTableWidgetItem(entry.value(QStringLiteral("state")).toString()));
                    // The one place a per-origin override is set (the open item of ADR-039): a drop-down of
                    // keep/sweep/wipe, saved straight to the core's row. The row's `policy` already wins over the
                    // container default in the core (`SessionRules::resolve`), so this only exposes that decision.
                    auto* policy_combo = new QComboBox(sessions_table_);
                    policy_combo->addItems({QStringLiteral("keep"), QStringLiteral("sweep"),
                                            QStringLiteral("wipe")});
                    policy_combo->setCurrentText(
                        entry.value(QStringLiteral("policy")).toString(QStringLiteral("sweep")));
                    connect(policy_combo, &QComboBox::currentTextChanged, this,
                            [this, entry](const QString& policy) {
                                if (core_ == nullptr || !core_->isRunning()) {
                                    return;
                                }
                                QJsonObject updated = entry;
                                updated.insert(QStringLiteral("policy"), policy);
                                updated.remove(QStringLiteral("size"));
                                updated.remove(QStringLiteral("loses_login"));
                                core_->sessionUpsert(
                                    updated, [this](const QJsonObject&, const QString& error) {
                                        setStatus(error.isEmpty()
                                                      ? QStringLiteral("munkamenet-politika mentve")
                                                      : QStringLiteral("munkamenet-politika hiba: %1")
                                                            .arg(error));
                                    });
                            });
                    sessions_table_->setCellWidget(row, 2, policy_combo);
                    sessions_table_->setItem(
                        row, 3,
                        new QTableWidgetItem(QString::number(
                            entry.value(QStringLiteral("cookie_count")).toInt())));
                    sessions_table_->setItem(
                        row, 4,
                        new QTableWidgetItem(due_origins.contains(origin)
                                                 ? QStringLiteral("lejárt")
                                                 : QStringLiteral("—")));
                    // What a cleanup would remove for this row, as the core measured it (41st slice): the
                    // amount on one line, and the friction it costs in one word.
                    const QJsonObject size = entry.value(QStringLiteral("size")).toObject();
                    const double bytes = size.value(QStringLiteral("bytes")).toDouble();
                    QString loss =
                        QStringLiteral("%1 süti · %2 kB")
                            .arg(size.value(QStringLiteral("cookies")).toInt())
                            .arg(QString::number(bytes / 1024.0, 'f', 1));
                    if (entry.value(QStringLiteral("loses_login")).toBool()) {
                        loss += QStringLiteral(" · belépés");
                    }
                    sessions_table_->setItem(row, 5, new QTableWidgetItem(loss));
                    ++row;
                }
                sessions_table_->resizeColumnsToContents();
                setStatus(QStringLiteral("munkamenet-napló: %1 sor, ebből %2 lejárt")
                              .arg(rows.size())
                              .arg(due_origins.size()));
            });
    });
}

int MainWindow::sessionPanelTtlHours() const {
    return (sessions_ttl_ != nullptr) ? sessions_ttl_->value() : 0;
}

bool MainWindow::sessionPanelLockWipe() const {
    return (sessions_lock_wipe_ != nullptr) && sessions_lock_wipe_->isChecked();
}
QString MainWindow::sessionPanelSummary() const {
    if (sessions_table_ == nullptr) {
        return QString();
    }
    QStringList lines;
    for (int row = 0; row < sessions_table_->rowCount(); ++row) {
        QStringList cells;
        for (int column = 0; column < sessions_table_->columnCount(); ++column) {
            // The policy column is a drop-down, not a text item; read its current value so the summary
            // (and the self test) still see what the panel shows.
            if (auto* combo = qobject_cast<QComboBox*>(sessions_table_->cellWidget(row, column))) {
                cells.append(combo->currentText());
                continue;
            }
            const QTableWidgetItem* item = sessions_table_->item(row, column);
            cells.append(item != nullptr ? item->text() : QStringLiteral("?"));
        }
        lines.append(cells.join(QStringLiteral(" | ")));
    }
    return lines.join(QLatin1Char('\n'));
}

int MainWindow::sessionPanelRowCount() const {
    return sessions_table_ == nullptr ? 0 : sessions_table_->rowCount();
}

void MainWindow::cleanupSessions() {
    if (core_ == nullptr || !core_->isRunning()) {
        setStatus(QStringLiteral("a takarításhoz előbb indítsd el a magot"));
        return;
    }
    // The orphaned previews are housekeeping, not policy: they are removed whether or not anything is due.
    cleanupPreviews();
    core_->sessionDue(
        QDateTime::currentMSecsSinceEpoch(), false,
        [this](const QJsonObject& due, const QString& error) {
            if (!error.isEmpty()) {
                setStatus(QStringLiteral("a takarítási terv nem érkezett meg: %1").arg(error));
                return;
            }
            const QJsonArray rows = due.value(QStringLiteral("due")).toArray();
            int queued = 0;
            /// The live origin's plan, held back until the page says whether it is playing: wiping the storage of
            /// the card on screen mid-playback cuts the sound (docs/HU/02 §4.2 — a playing card keeps its session).
            QString live_origin;
            PendingWipe live_pending;
            QStringList live_storages;
            for (const QJsonValue& value : rows) {
                const QJsonObject entry = value.toObject();
                const QJsonObject session = entry.value(QStringLiteral("session")).toObject();
                const QJsonObject plan = entry.value(QStringLiteral("plan")).toObject();
                const QString origin = session.value(QStringLiteral("origin")).toString();
                if (origin.isEmpty()) {
                    continue;
                }
                // The plan is the list of what may be deleted; the shell only reads it (ADR-033).
                QStringList storages;
                for (const char* field : {"cookies", "local_storage", "indexed_db",
                                          "service_workers", "cache_storage", "session_storage",
                                          "code_cache"}) {
                    if (plan.value(QString::fromLatin1(field)).toBool()) {
                        storages.append(QString::fromLatin1(field));
                    }
                }
                const PendingWipe pending{session.value(QStringLiteral("container_id")).toString(),
                                          storages};
                if (!live_origin_.isEmpty() && live_origin_ == origin && surface_ != 0 &&
                    live_origin.isEmpty()) {
                    live_origin = origin;
                    live_pending = pending;
                    live_storages = storages;
                } else {
                    wipe_on_open_.insert(origin, pending);
                    ++queued;
                }
            }

            const auto report = [this, queued](int immediate) {
                setStatus(QStringLiteral("takarítás: %1 azonnal, %2 a kártya következő megnyitásakor")
                              .arg(immediate)
                              .arg(queued));
                if (sessions_dock_ != nullptr && sessions_dock_->isVisible()) {
                    refreshSessions();
                }
            };

            if (live_origin.isEmpty()) {
                report(0);
                return;
            }

            // The live origin: ask the page first. A card that plays sound or video keeps its session — this was
            // the last place that ignored it, so housekeeping could silence a video in the foreground.
            live_engine_->evaluate(
                surface_, LiveMediaPlayingScript(),
                [this, live_origin, live_pending, live_storages, report](const QJsonObject& reply,
                                                                        const QString& mediaError) {
                    const bool playing = mediaError.isEmpty() &&
                                         reply.value(QStringLiteral("value")).toString() ==
                                             QStringLiteral("playing");
                    if (playing) {
                        setStatus(QStringLiteral(
                            "takarítás: a kártya médiát játszik — az élő munkamenet megmarad"));
                        if (sessions_dock_ != nullptr && sessions_dock_->isVisible()) {
                            refreshSessions();
                        }
                        return;
                    }
                    live_engine_->clearOriginData(
                        surface_, live_origin, live_storages,
                        [this, live_origin, container = live_pending.container, report](
                            const QJsonObject&, const QString& failure) {
                            if (!failure.isEmpty()) {
                                setStatus(QStringLiteral("takarítás hiba (%1): %2")
                                              .arg(live_origin, failure));
                                return;
                            }
                            core_->sessionClose(container, live_origin,
                                                [this, report](const QJsonObject&, const QString&) {
                                                    report(1);
                                                });
                        });
                });
        });
}


void MainWindow::exportProfile() {
    if (core_ == nullptr || !core_->isRunning()) {
        setStatus(QStringLiteral("a mag nem fut — nincs mit menteni"));
        return;
    }
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Profil mentése"),
        documents + QStringLiteral("/antikolt-profile.json"),
        QStringLiteral("Antikolt profil (*.json)"));
    if (path.isEmpty()) {
        return;
    }
    core_->profileExport(path, [this, path](const QJsonObject& reply, const QString& error) {
        if (!error.isEmpty()) {
            setStatus(QStringLiteral("profil mentése nem sikerült: %1").arg(error));
            return;
        }
        setStatus(QStringLiteral("profil mentve: %1 (%2 kártya, %3 konténer)")
                      .arg(path)
                      .arg(reply.value(QStringLiteral("cards")).toInt())
                      .arg(reply.value(QStringLiteral("containers")).toInt()));
    });
}

void MainWindow::printToPdf() {
    // Save the live card as a PDF (F7 "nyomtatás"): only a real card has a document worth printing — the grid is
    // the browser's own chrome, not a page.
    if (surface_ == 0 || surface_ == home_surface_ || live_engine_ == nullptr ||
        !live_engine_->isRunning()) {
        setStatus(QStringLiteral("nincs élő kártya — nincs mit menteni"));
        return;
    }
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Oldal mentése PDF-be"),
        documents + QStringLiteral("/antikolt-page.pdf"),
        QStringLiteral("PDF (*.pdf)"));
    if (path.isEmpty()) {
        return;
    }
    live_engine_->printPdf(surface_, path, [this, path](const QJsonObject& reply, const QString& error) {
        if (!error.isEmpty() || !reply.value(QStringLiteral("ok")).toBool()) {
            setStatus(QStringLiteral("a PDF mentése nem sikerült: %1")
                          .arg(error.isEmpty() ? QStringLiteral("ismeretlen hiba") : error));
            return;
        }
        setStatus(QStringLiteral("PDF mentve: %1").arg(path));
    });
}

void MainWindow::openFind() {
    if (find_bar_ == nullptr) {
        return;
    }
    find_bar_->setVisible(true);
    find_edit_->setFocus();
    find_edit_->selectAll();
}

void MainWindow::closeFind() {
    if (find_bar_ != nullptr) {
        find_bar_->setVisible(false);
    }
    if (live_engine_ != nullptr && surface_ != 0) {
        live_engine_->stopFind(surface_);
    }
    find_last_text_.clear();
    find_last_count_ = 0;
}

void MainWindow::findNext() {
    if (find_edit_ == nullptr || live_engine_ == nullptr || surface_ == 0) {
        return;
    }
    const QString text = find_edit_->text();
    if (text.isEmpty()) {
        return;
    }
    // Same query again → advance; a changed query → a fresh search.
    const bool fresh = text != find_last_text_;
    find_last_text_ = text;
    live_engine_->find(surface_, text, /*find_next=*/!fresh, /*forward=*/true);
}

void MainWindow::findPrevious() {
    if (find_edit_ == nullptr || live_engine_ == nullptr || surface_ == 0) {
        return;
    }
    const QString text = find_edit_->text();
    if (text.isEmpty()) {
        return;
    }
    const bool fresh = text != find_last_text_;
    find_last_text_ = text;
    live_engine_->find(surface_, text, /*find_next=*/!fresh, /*forward=*/false);
}

void MainWindow::onFindResult(qint64 surface, int count, int active) {
    // Only the surface the user is looking at drives the counter.
    if (surface != surface_) {
        return;
    }
    find_last_count_ = count;
    if (find_status_ != nullptr) {
        find_status_->setText(count == 0 ? QStringLiteral("nincs találat")
                                         : QStringLiteral("%1 / %2").arg(active).arg(count));
    }
}

void MainWindow::importProfile() {
    if (core_ == nullptr || !core_->isRunning()) {
        setStatus(QStringLiteral("a mag nem fut — nincs hová betölteni"));
        return;
    }
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Profil betöltése"),
        documents, QStringLiteral("Antikolt profil (*.json)"));
    if (path.isEmpty()) {
        return;
    }
    core_->profileImport(path, [this, path](const QJsonObject& reply, const QString& error) {
        if (!error.isEmpty()) {
            setStatus(QStringLiteral("profil betöltése nem sikerült: %1").arg(error));
            return;
        }
        setStatus(QStringLiteral("profil betöltve: %1 (%2 kártya, %3 konténer)")
                      .arg(path)
                      .arg(reply.value(QStringLiteral("cards")).toInt())
                      .arg(reply.value(QStringLiteral("containers")).toInt()));
        // An imported profile may carry a different memory budget (F6): refresh the cached copy.
        refreshUserPrefs();
        // The grid must show the imported rows, not the profile it replaced.
        if (home_surface_ != 0) {
            pushCardsToGrid(home_surface_);
        }
    });
}

QString memoryPressureName(qint64 used_bytes, qint64 warn_bytes, qint64 hard_bytes) {
    if (hard_bytes > 0 && used_bytes >= hard_bytes) {
        return QStringLiteral("hard");
    }
    if (warn_bytes > 0 && used_bytes >= warn_bytes) {
        return QStringLiteral("warn");
    }
    return QStringLiteral("normal");
}

void MainWindow::refreshUserPrefs() {
    if (core_ == nullptr || !core_->isRunning()) {
        return;
    }
    core_->userPrefs([this](const QJsonObject& reply, const QString& error) {
        if (!error.isEmpty()) {
            return;
        }
        const QJsonObject prefs = reply.value(QStringLiteral("prefs")).toObject();
        const qint64 mb = static_cast<qint64>(prefs.value(QStringLiteral("memory_budget_mb")).toDouble());
        if (mb >= 64 && mb <= 4096) {
            memory_budget_bytes_ = mb * 1024 * 1024;
        }
        // The hibernation window is a profile setting too (ADR-072). The self-test override always wins;
        // otherwise the timer follows the core. The clock is re-armed only when a card is live, so a change
        // here affects the next window, not a running one.
        const qint64 idle = static_cast<qint64>(prefs.value(QStringLiteral("idle_hibernate_ms")).toDouble());
        if (qEnvironmentVariableIntValue("ANTIKOLT_HIBERNATE_MS") <= 0 && idle >= 60000 && idle <= 3600000) {
            idle_hibernate_ms_ = idle;
            if (idle_ != nullptr) {
                idle_->setInterval(static_cast<int>(idle_hibernate_ms_));
            }
        }
        // The derived profile params (ADR-020): the background preview worker follows `preview_refresh`, and the
        // engine lifetime (T1/T2) is cached for the raster home. A `Manual` profile has no params (null), so the
        // factory behaviour stays on.
        const QJsonObject profile = reply.value(QStringLiteral("profile")).toObject();
        // The warn/hard bands (ADR-020) come from the derived profile. A positive test-only override
        // (`ANTIKOLT_MEMORY_WARN_BYTES`/`ANTIKOLT_MEMORY_HARD_BYTES`) replaces them, so a self test can force the
        // measured working set across a boundary — the same pattern as `ANTIKOLT_HIBERNATE_MS`.
        const qint64 warn_override = qEnvironmentVariableIntValue("ANTIKOLT_MEMORY_WARN_BYTES");
        const qint64 hard_override = qEnvironmentVariableIntValue("ANTIKOLT_MEMORY_HARD_BYTES");
        if (!profile.isEmpty()) {
            preview_refresh_ = profile.value(QStringLiteral("preview_refresh")).toBool(true);
            const QJsonValue shutdown = profile.value(QStringLiteral("engine_warm_idle_shutdown_ms"));
            engine_shutdown_ms_ = shutdown.isNull() ? -1 : static_cast<qint64>(shutdown.toDouble());
            memory_warn_bytes_ =
                warn_override > 0
                    ? warn_override
                    : static_cast<qint64>(profile.value(QStringLiteral("memory_warn_bytes")).toDouble());
            memory_hard_bytes_ =
                hard_override > 0
                    ? hard_override
                    : static_cast<qint64>(profile.value(QStringLiteral("memory_hard_bytes")).toDouble());
            // The top-level video display cap (ADR-020/ADR-076) comes from the same derived profile. A
            // `manual` profile carries no params, so its factory 0 (no cap) is the honest default.
            video_max_height_ =
                static_cast<int>(profile.value(QStringLiteral("video_max_height")).toDouble());
        } else {
            preview_refresh_ = true;
            engine_shutdown_ms_ = -1;
            memory_warn_bytes_ = warn_override > 0 ? warn_override : 850LL * 1024 * 1024;
            memory_hard_bytes_ = hard_override > 0 ? hard_override : 1000LL * 1024 * 1024;
            video_max_height_ = 0;
        }
        // A profile change re-applies the cap to the live card immediately, so the user's switch is not
        // silently deferred to the next card load. The grid has no videos, but the same call is harmless there.
        if (live_engine_ != nullptr && surface_ != 0) {
            live_engine_->setVideoCap(surface_, video_max_height_, nullptr);
        }
        // An open dock follows the core's answer, not a stale copy of it.
        if (settings_dock_ != nullptr && settings_dock_->isVisible()) {
            fillSettingsDock(prefs);
        }
    });
}

void MainWindow::showSettings() {
    if (settings_dock_ == nullptr) {
        settings_dock_ = new QDockWidget(t(QStringLiteral("Beállítások")), this);
        settings_dock_->setObjectName(QStringLiteral("settings"));
        auto* panel = new QWidget(settings_dock_);
        auto* layout = new QVBoxLayout(panel);

        // The units a person thinks in (cards, minutes, hours, MiB); the core stores the raw numbers and
        // validates them (ADR-072). A dock like the session panel, not a modal.
        auto* live = new QWidget(panel);
        auto* live_layout = new QHBoxLayout(live);
        live_layout->setContentsMargins(0, 0, 0, 0);
        settings_live_label_ = new QLabel(t(QStringLiteral("Élő kártyák (max):")), live);
        live_layout->addWidget(settings_live_label_);
        prefs_live_cards_ = new QSpinBox(live);
        prefs_live_cards_->setRange(1, 12);
        live_layout->addWidget(prefs_live_cards_);
        layout->addWidget(live);

        auto* idle = new QWidget(panel);
        auto* idle_layout = new QHBoxLayout(idle);
        idle_layout->setContentsMargins(0, 0, 0, 0);
        settings_idle_label_ = new QLabel(t(QStringLiteral("Hibernálás tétlenség után (perc):")), idle);
        idle_layout->addWidget(settings_idle_label_);
        prefs_idle_minutes_ = new QSpinBox(idle);
        prefs_idle_minutes_->setRange(1, 60);
        idle_layout->addWidget(prefs_idle_minutes_);
        layout->addWidget(idle);

        auto* cold = new QWidget(panel);
        auto* cold_layout = new QHBoxLayout(cold);
        cold_layout->setContentsMargins(0, 0, 0, 0);
        settings_cold_label_ = new QLabel(t(QStringLiteral("Hideg kártya (óra):")), cold);
        cold_layout->addWidget(settings_cold_label_);
        prefs_cold_hours_ = new QSpinBox(cold);
        prefs_cold_hours_->setRange(1, 720);
        cold_layout->addWidget(prefs_cold_hours_);
        layout->addWidget(cold);

        auto* budget = new QWidget(panel);
        auto* budget_layout = new QHBoxLayout(budget);
        budget_layout->setContentsMargins(0, 0, 0, 0);
        settings_budget_label_ = new QLabel(t(QStringLiteral("Memória-büdzsé (MiB):")), budget);
        budget_layout->addWidget(settings_budget_label_);
        prefs_budget_mb_ = new QSpinBox(budget);
        prefs_budget_mb_->setRange(64, 4096);
        budget_layout->addWidget(prefs_budget_mb_);
        layout->addWidget(budget);

        // The resource profile (F6, `09-CELHARDVER.md` §3): a named preset fills the hibernation and budget on
        // write; "Kézi" leaves the fields above authoritative.
        auto* profile = new QWidget(panel);
        auto* profile_layout = new QHBoxLayout(profile);
        profile_layout->setContentsMargins(0, 0, 0, 0);
        settings_profile_label_ = new QLabel(t(QStringLiteral("Erőforrás-profil:")), profile);
        profile_layout->addWidget(settings_profile_label_);
        prefs_profile_ = new QComboBox(profile);
        prefs_profile_->addItem(QStringLiteral("Gyors"), QStringLiteral("fast"));
        prefs_profile_->addItem(QStringLiteral("Kiegyensúlyozott"), QStringLiteral("balanced"));
        prefs_profile_->addItem(QStringLiteral("Takarékos"), QStringLiteral("saver"));
        prefs_profile_->addItem(QStringLiteral("Ultra"), QStringLiteral("ultra"));
        prefs_profile_->addItem(QStringLiteral("Kézi"), QStringLiteral("manual"));
        prefs_profile_->setToolTip(QStringLiteral(
            "A névvel ellátott profil preset: felülírja a hibernálást és a büdzsét. A Kézi a fentieket hagyja."));
        profile_layout->addWidget(prefs_profile_);
        layout->addWidget(profile);

        // The shell language (i18n, ADR-078): a runtime switch, applied in place — no restart.
        auto* language = new QWidget(panel);
        auto* language_layout = new QHBoxLayout(language);
        language_layout->setContentsMargins(0, 0, 0, 0);
        settings_language_label_ = new QLabel(t(QStringLiteral("Nyelv:")), language);
        language_layout->addWidget(settings_language_label_);
        prefs_language_ = new QComboBox(language);
        prefs_language_->addItem(QStringLiteral("Magyar"), QStringLiteral("hu"));
        prefs_language_->addItem(QStringLiteral("English"), QStringLiteral("en"));
        prefs_language_->setCurrentIndex(prefs_language_->findData(language_));
        connect(prefs_language_, &QComboBox::currentIndexChanged, this,
                [this](int) { setLanguage(prefs_language_->currentData().toString()); });
        language_layout->addWidget(prefs_language_);
        layout->addWidget(language);

        auto* save = new QPushButton(QStringLiteral("Mentés"), panel);
        save->setObjectName(QStringLiteral("prefsSave"));
        connect(save, &QPushButton::clicked, this, &MainWindow::saveUserPrefs);
        layout->addWidget(save);

        // The editable home (item 3): a user-authored home.html overrides the grid. One button brings the built-in
        // grid back, because a broken custom page must never be a dead end.
        auto* home_edit = new QPushButton(QStringLiteral("Egyéni kezdőlap szerkesztése"), panel);
        home_edit->setObjectName(QStringLiteral("editCustomHome"));
        home_edit->setToolTip(QStringLiteral(
            "Létrehozza a kiinduló home.html-t (ha még nincs), és megnyitja a szerkesztőben."));
        connect(home_edit, &QPushButton::clicked, this, [this] {
            createCustomHomeTemplate();
            QDesktopServices::openUrl(QUrl::fromLocalFile(coreDataDir() + QStringLiteral("/home.html")));
        });
        layout->addWidget(home_edit);
        auto* home_reset = new QPushButton(QStringLiteral("Egyéni kezdőlap visszaállítása"), panel);
        home_reset->setObjectName(QStringLiteral("resetCustomHome"));
        home_reset->setToolTip(QStringLiteral(
            "Törli a profil home.html-jét, és visszatölti a beépített kártyarácskot."));
        connect(home_reset, &QPushButton::clicked, this, &MainWindow::resetCustomHome);
        layout->addWidget(home_reset);
        layout->addStretch(1);

        settings_dock_->setWidget(panel);
        addDockWidget(Qt::RightDockWidgetArea, settings_dock_);
    }
    settings_dock_->show();
    settings_dock_->raise();
    // Fill from the core's current row, not from a shell-side guess.
    if (core_ != nullptr && core_->isRunning()) {
        core_->userPrefs([this](const QJsonObject& reply, const QString& error) {
            if (!error.isEmpty()) {
                return;
            }
            fillSettingsDock(reply.value(QStringLiteral("prefs")).toObject());
        });
    }
}

void MainWindow::fillSettingsDock(const QJsonObject& prefs) {
    if (prefs_live_cards_ != nullptr) {
        prefs_live_cards_->setValue(prefs.value(QStringLiteral("max_live_cards")).toInt(5));
    }
    if (prefs_idle_minutes_ != nullptr) {
        prefs_idle_minutes_->setValue(static_cast<int>(
            std::lround(prefs.value(QStringLiteral("idle_hibernate_ms")).toDouble() / 60000.0)));
    }
    if (prefs_cold_hours_ != nullptr) {
        prefs_cold_hours_->setValue(static_cast<int>(
            std::lround(prefs.value(QStringLiteral("cold_card_ms")).toDouble() / 3600000.0)));
    }
    if (prefs_budget_mb_ != nullptr) {
        prefs_budget_mb_->setValue(prefs.value(QStringLiteral("memory_budget_mb")).toInt(512));
    }
    if (prefs_profile_ != nullptr) {
        const QString profile =
            prefs.value(QStringLiteral("resource_profile")).toString(QStringLiteral("balanced"));
        const int index = prefs_profile_->findData(profile);
        prefs_profile_->setCurrentIndex(index >= 0 ? index : 1);
    }
}

void MainWindow::saveUserPrefs() {
    if (core_ == nullptr || !core_->isRunning()) {
        setStatus(QStringLiteral("a mag nem fut, a beállítások nem menthetők"));
        return;
    }
    QJsonObject prefs;
    prefs.insert(QStringLiteral("max_live_cards"), prefs_live_cards_ != nullptr ? prefs_live_cards_->value() : 5);
    prefs.insert(QStringLiteral("idle_hibernate_ms"),
                 static_cast<qint64>((prefs_idle_minutes_ != nullptr ? prefs_idle_minutes_->value() : 10) *
                                     60000LL));
    prefs.insert(QStringLiteral("cold_card_ms"),
                 static_cast<qint64>((prefs_cold_hours_ != nullptr ? prefs_cold_hours_->value() : 24) *
                                     3600000LL));
    prefs.insert(QStringLiteral("memory_budget_mb"),
                 prefs_budget_mb_ != nullptr ? prefs_budget_mb_->value() : 512);
    prefs.insert(QStringLiteral("resource_profile"),
                 prefs_profile_ != nullptr ? prefs_profile_->currentData().toString()
                                           : QStringLiteral("balanced"));
    core_->userPrefsSet(prefs, [this](const QJsonObject&, const QString& error) {
        if (!error.isEmpty()) {
            setStatus(QStringLiteral("a beállítások nem mentődtek: %1").arg(error));
            return;
        }
        setStatus(QStringLiteral("beállítások mentve"));
        // Re-read what the core accepted: the cache (budget, hibernation window) follows the stored row.
        refreshUserPrefs();
    });
}

void MainWindow::resetCustomHome() {
    const QString home = coreDataDir() + QStringLiteral("/home.html");
    if (QFile::exists(home)) {
        QFile::remove(home);
    }
    setStatus(QStringLiteral("az egyéni kezdőlap visszaállítva"));
    // Reload the grid: without `home.html`, `homePageUrl()` serves the built-in `home/index.html` again.
    loadHome();
}

namespace {
/// The starter page the editor writes when the user has no `home.html` yet. It loads the card package next to it
/// and renders the shell's cards through `window.antikoltAcceptCards` — a working base to edit, not a blank file.
constexpr const char kCustomHomeTemplate[] = R"html(<!doctype html>
<html lang="hu">
<head>
<meta charset="utf-8">
<title>Saját kezdőlap</title>
<style>
:root { --surface:#fff; --surface-raised:#f4f4f5; --border:#d4d4d8; --text:#18181b; --text-muted:#71717a; --accent:#2f6feb; --radius-card:12px; --radius-control:8px; --shadow-1:0 1px 2px rgb(0 0 0/8%); --shadow-2:0 4px 12px rgb(0 0 0/14%); --motion-fast:120ms; --ease-out:cubic-bezier(.16,1,.3,1); }
body { font-family: Inter, "Segoe UI", system-ui, sans-serif; background:#fafafa; color:var(--text); margin:0; padding:24px; }
.grid { display:grid; grid-template-columns:repeat(auto-fill,minmax(240px,1fr)); gap:16px; }
</style>
</head>
<body>
<h1>Saját kezdőlap</h1>
<p>A kártyák a <code>&lt;antikolt-card&gt;</code> komponenssel jelennek meg — a saját lapod tetszőleges HTML/CSS-t rakhat köré.</p>
<div class="grid" id="cards"></div>
<script src="antikolt-card.js" defer></script>
<script>
window.antikoltAcceptCards = function (cards) {
  var host = document.getElementById("cards");
  host.replaceChildren();
  for (var i = 0; i < cards.length; i++) {
    var element = document.createElement("antikolt-card");
    element.card = cards[i];
    host.append(element);
  }
};
</script>
</body>
</html>
)html";

/// Where the built-in `home/` lives (the same candidates `homePageUrl` uses), so the shell can copy `antikolt-card.js`
/// next to the user's `home.html`.
QString homeDirectory() {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/home"),
        appDir + QStringLiteral("/../home"),
        appDir + QStringLiteral("/../../../home"),
        QDir::currentPath() + QStringLiteral("/home"),
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate + QStringLiteral("/index.html"))) {
            return QDir::cleanPath(candidate);
        }
    }
    return QString();
}
}  // namespace

void MainWindow::createCustomHomeTemplate() {
    const QString target = coreDataDir() + QStringLiteral("/home.html");
    if (QFile::exists(target)) {
        return;  // never overwrite what the user already wrote
    }
    // The card package the template loads must sit next to the home.html it is served from.
    const QString home = homeDirectory();
    if (!home.isEmpty()) {
        const QString card = home + QStringLiteral("/antikolt-card.js");
        const QString card_target = coreDataDir() + QStringLiteral("/antikolt-card.js");
        if (QFile::exists(card) && !QFile::exists(card_target)) {
            QFile::copy(card, card_target);
        }
    }
    QFile file(target);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(kCustomHomeTemplate);
    }
}

void MainWindow::captureGrid() {
    if (!engine_->isRunning() || home_surface_ == 0) {
        setStatus(QStringLiteral("nincs mit raszterizálni"));
        return;
    }
    // `capture` writes into `screenshots/`; the raster home wants one stable file under the profile.
    engine_->capture(home_surface_, QStringLiteral("png"), 80,
                     [this](const QJsonObject& shot, const QString& error) {
                         const QString written = shot.value(QStringLiteral("path")).toString();
                         if (!error.isEmpty() || written.isEmpty()) {
                             setStatus(QStringLiteral("a rács raszterizálása nem sikerült: %1")
                                           .arg(error.isEmpty() ? QStringLiteral("nincs path a válaszban")
                                                                : error));
                             return;
                         }
                         const QString target = engine_profile_ + QStringLiteral("/home-raster.png");
                         QFile::remove(target);
                         if (!QFile::copy(written, target)) {
                             setStatus(QStringLiteral("a raszter nem került a profilba: %1").arg(target));
                             return;
                         }
                         setStatus(QStringLiteral("raszter kész: %1").arg(target));
                     });
}

void MainWindow::enterRasterMode() {
    if (raster_mode_ || !engine_->isRunning() || home_surface_ == 0) {
        return;
    }
    // Capture the card rectangles first (so a later click on the raster can open the right card), then rasterise
    // and stop the engine only when the raster is on disk: a failed capture must not leave a blank window.
    captureCardPositions([this] {
        engine_->capture(home_surface_, QStringLiteral("png"), 80,
                         [this](const QJsonObject& shot, const QString& error) {
                             const QString written = shot.value(QStringLiteral("path")).toString();
                             const QString target = engine_profile_ + QStringLiteral("/home-raster.png");
                             if (!error.isEmpty() || written.isEmpty()) {
                                 return;
                             }
                             QFile::remove(target);
                             if (!QFile::copy(written, target)) {
                                 return;
                             }
                             engine_->shutdown();
                             raster_mode_ = true;
                             // The grid surface died with the engine process: clear it, so the restart creates a
                             // fresh surface instead of navigating a stale id into the new process.
                             if (card_->hasSurface()) {
                                 card_->detach();
                             }
                             // Show the raster over the (now empty) card area, so the user sees the grid, not a
                             // blank window.
                             raster_full_ = QPixmap(target);
                             updateRasterPixmap();
                             if (card_stack_ != nullptr) {
                                 card_stack_->setCurrentWidget(raster_label_);
                             }
                             setStatus(QStringLiteral("raszter-kezdőlap (a motor leállítva)"));
                         });
    });
}

void MainWindow::wakeFromRaster() {
    if (!raster_mode_) {
        return;
    }
    raster_mode_ = false;
    if (card_stack_ != nullptr) {
        card_stack_->setCurrentWidget(card_);
    }
    if (!engine_->isRunning()) {
        engine_->start(engineExecutable(), engine_profile_, false, engine_profile_ + QStringLiteral("/previews"));
        engine_->handshake();
    }
    // The `engineReady` welcome reloads the grid (ensureSurface), so nothing else is needed here.
}

void MainWindow::updateRasterPixmap() {
    if (raster_label_ == nullptr || raster_full_.isNull() || raster_label_->width() <= 0 ||
        raster_label_->height() <= 0) {
        raster_drawn_size_ = QSize();
        return;
    }
    const QPixmap scaled =
        raster_full_.scaled(raster_label_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    raster_label_->setPixmap(scaled);
    raster_drawn_size_ = scaled.size();
}

void MainWindow::armEngineIdleTimer() {
    if (engine_idle_ == nullptr) {
        engine_idle_ = new QTimer(this);
        engine_idle_->setSingleShot(true);
        connect(engine_idle_, &QTimer::timeout, this, &MainWindow::enterRasterMode);
    }
    // `-1` is "keep it warm" (T1): no timer. Otherwise the raster home activates after the profile's threshold.
    if (engine_shutdown_ms_ < 0) {
        engine_idle_->stop();
        return;
    }
    engine_idle_->start(static_cast<int>(engine_shutdown_ms_));
}

void MainWindow::captureCardPositions(std::function<void()> done) {
    raster_cards_.clear();
    if (home_surface_ == 0 || !engine_->isRunning()) {
        if (done) {
            done();
        }
        return;
    }
    // Each visible `<antikolt-card>` reports its id, url, title and rectangle (CSS pixels, viewport-relative), and
    // the viewport size comes along for the raster click mapping. The virtualised grid renders only what is on
    // screen, which is exactly what the raster shows.
    engine_->evaluate(
        home_surface_,
        QStringLiteral(
            "JSON.stringify({vw:window.innerWidth,vh:window.innerHeight,cards:[...document.querySelectorAll("
            "'antikolt-card')].map(function(el){var r=el.getBoundingClientRect();"
            "return {id:(el.card&&el.card.id)||'',url:(el.card&&el.card.url)||'',"
            "title:(el.card&&el.card.title)||'',x:r.left,y:r.top,w:r.width,h:r.height};})})"),
        [this, done](const QJsonObject& reply, const QString& error) {
            if (error.isEmpty()) {
                const QJsonObject captured =
                    QJsonDocument::fromJson(reply.value(QStringLiteral("value")).toString().toUtf8()).object();
                raster_css_w_ = captured.value(QStringLiteral("vw")).toDouble();
                raster_css_h_ = captured.value(QStringLiteral("vh")).toDouble();
                const QJsonArray cards = captured.value(QStringLiteral("cards")).toArray();
                for (const QJsonValue& value : cards) {
                    const QJsonObject rect = value.toObject();
                    RasterCard card;
                    card.id = rect.value(QStringLiteral("id")).toString();
                    card.url = rect.value(QStringLiteral("url")).toString();
                    card.title = rect.value(QStringLiteral("title")).toString();
                    card.x = rect.value(QStringLiteral("x")).toDouble();
                    card.y = rect.value(QStringLiteral("y")).toDouble();
                    card.w = rect.value(QStringLiteral("w")).toDouble();
                    card.h = rect.value(QStringLiteral("h")).toDouble();
                    if (!card.id.isEmpty()) {
                        raster_cards_.append(card);
                    }
                }
            }
            if (done) {
                done();
            }
        });
}

QString MainWindow::cardAtRasterPoint(qreal x, qreal y) const {
    for (const RasterCard& card : raster_cards_) {
        if (x >= card.x && x < card.x + card.w && y >= card.y && y < card.y + card.h) {
            return card.id;
        }
    }
    return QString();
}

void MainWindow::buildToolBar() {
    auto* toolbar = addToolBar(QStringLiteral("Fő"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);

    action_start_engine_ =
        toolbar->addAction(t(QStringLiteral("Motor indítása")), this, &MainWindow::startEngine);
    action_home_ = toolbar->addAction(t(QStringLiteral("⌂ Kezdőlap")), this, &MainWindow::loadHome);
    action_freeze_ = toolbar->addAction(t(QStringLiteral("❄ Fagyasztás")), this, &MainWindow::freezeCard);
    action_sessions_ =
        toolbar->addAction(t(QStringLiteral("🗂 Munkamenetek")), this, &MainWindow::showSessions);
    action_settings_ =
        toolbar->addAction(t(QStringLiteral("⚙ Beállítások")), this, &MainWindow::showSettings);

    // Portable profile bundle (ADR-059): the shell owns the native file dialog; the core does the export/import.
    // The dialog cannot be clicked by the harness, so the button is the product's entry point while `--profiletest`
    // measures the underlying path through the bridge.
    auto* profile_button = new QToolButton(this);
    profile_button_ = profile_button;
    profile_button->setObjectName(QStringLiteral("profileButton"));
    profile_button->setText(t(QStringLiteral("💾 Profil")));
    profile_button->setPopupMode(QToolButton::InstantPopup);
    profile_button->setAutoRaise(true);
    profile_button->setCursor(Qt::PointingHandCursor);
    profile_button->setToolTip(
        QStringLiteral("A profil egyetlen fájlba mentése vagy onnan betöltése (ADR-059)."));
    profile_button->setAccessibleName(QStringLiteral("Profil"));
    auto* profile_menu = new QMenu(profile_button);
    auto* export_action = profile_menu->addAction(QStringLiteral("Mentés másként…"));
    export_action->setObjectName(QStringLiteral("profileExportAction"));
    connect(export_action, &QAction::triggered, this, &MainWindow::exportProfile);
    auto* import_action = profile_menu->addAction(QStringLiteral("Betöltés…"));
    import_action->setObjectName(QStringLiteral("profileImportAction"));
    connect(import_action, &QAction::triggered, this, &MainWindow::importProfile);
    profile_button->setMenu(profile_menu);
    toolbar->addWidget(profile_button);
    // Save-as-PDF (F7 "nyomtatás"): the shell owns the native dialog, the engine writes the PDF. The dialog cannot
    // be clicked by the harness, so `--printtest` measures the underlying `print_pdf` path.
    auto* print_button = new QToolButton(this);
    print_button_ = print_button;
    print_button->setObjectName(QStringLiteral("printButton"));
    print_button->setText(t(QStringLiteral("🖨️ PDF")));
    print_button->setPopupMode(QToolButton::InstantPopup);
    print_button->setAutoRaise(true);
    print_button->setCursor(Qt::PointingHandCursor);
    print_button->setToolTip(QStringLiteral("Az élő oldal mentése PDF-be (F7)."));
    print_button->setAccessibleName(QStringLiteral("PDF mentése"));
    auto* print_menu = new QMenu(print_button);
    auto* save_action = print_menu->addAction(QStringLiteral("Oldal mentése PDF-be…"));
    save_action->setObjectName(QStringLiteral("printSaveAction"));
    connect(save_action, &QAction::triggered, this, &MainWindow::printToPdf);
    print_button->setMenu(print_menu);
    toolbar->addWidget(print_button);
    // Site themes (ADR-037): the page decides what `prefers-color-scheme` means; this only reports it.
    action_site_theme_ =
        toolbar->addAction(t(QStringLiteral("🌓 Oldal-téma: %1")).arg(QStringLiteral("auto")));
    action_site_theme_->setObjectName(QStringLiteral("siteThemeAction"));
    connect(action_site_theme_, &QAction::triggered, this, [this] {
        const QStringList modes = {QStringLiteral("auto"), QStringLiteral("dark"),
                                   QStringLiteral("light")};
        const int next = (modes.indexOf(page_media_) + 1) % modes.size();
        setPageMedia(modes.at(next));
        action_site_theme_->setText(t(QStringLiteral("🌓 Oldal-téma: %1")).arg(page_media_));
    });

    // Zoom (ADR-038): the label is the state, the two buttons are the only way to change it.
    auto* zoomOut = toolbar->addAction(QStringLiteral("−"));
    zoomOut->setObjectName(QStringLiteral("zoomOutAction"));
    connect(zoomOut, &QAction::triggered, this, [this] { setZoomPercent(zoom_percent_ - 25); });
    zoom_label_ = toolbar->addAction(QStringLiteral("🔍 %1%").arg(zoom_percent_));
    zoom_label_->setObjectName(QStringLiteral("zoomLabel"));
    zoom_label_->setEnabled(false);
    auto* zoomIn = toolbar->addAction(QStringLiteral("+"));
    zoomIn->setObjectName(QStringLiteral("zoomInAction"));
    connect(zoomIn, &QAction::triggered, this, [this] { setZoomPercent(zoom_percent_ + 25); });

    // Dark pages (ADR-044/ADR-045): the site theme query only *tells* a page what the system prefers, so a page
    // without a dark theme of its own stays white. This is the user's switch for that case — with the modes the
    // user asked for (near-black, soft grey, warm grey), off by default, remembered, re-applied on every load.
    // Icon + menu + tooltip: the button's face is the mode in force, the words live in the tooltip.
    page_dark_button_ = new QToolButton(this);
    page_dark_button_->setObjectName(QStringLiteral("pageDarkButton"));
    page_dark_button_->setPopupMode(QToolButton::InstantPopup);
    page_dark_button_->setAutoRaise(true);
    page_dark_button_->setCursor(Qt::PointingHandCursor);
    page_dark_button_->setMenu(new QMenu(page_dark_button_));
    for (const QString& mode : pageDarkModes()) {
        auto* action = page_dark_button_->menu()->addAction(pageDarkModeLabel(mode));
        action->setObjectName(QStringLiteral("pageDarkMode_") + mode);
        action->setCheckable(true);
        action->setChecked(mode == page_dark_mode_);
        action->setIcon(PageDarkIcon(mode));
        connect(action, &QAction::triggered, this, [this, mode] { setPageDarkMode(mode); });
    }
    page_dark_button_->setIcon(PageDarkIcon(page_dark_mode_));
    page_dark_button_->setToolTip(
        QStringLiteral("Oldalak sötétítése: %1\nA weboldal saját sötét témája nélkül is sötét lesz.")
            .arg(pageDarkModeLabel(page_dark_mode_)));
    page_dark_button_->setAccessibleName(QStringLiteral("Oldalak sötétítése"));
    toolbar->addWidget(page_dark_button_);

    auto* spacer = new QWidget(this);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

    // Theme switching is part of the identity (light / grey / dark), not a settings-page afterthought — and it
    // is *icons* with tooltips (docs/HU/08-DIZAJN.md §6): four words next to each other were the widest thing on
    // the toolbar for the least information. The action's text stays for screen readers and tooltips.
    auto* themeGroup = new QWidget(this);
    auto* themeLayout = new QHBoxLayout(themeGroup);
    themeLayout->setContentsMargins(0, 0, 0, 0);
    themeLayout->setSpacing(2);
    auto* themeActions = new QActionGroup(this);
    themeActions->setExclusive(true);
    for (const QString& name : themeNames()) {
        auto* button = new QToolButton(themeGroup);
        button->setObjectName(QStringLiteral("themeButton_") + name);
        button->setAutoRaise(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setCheckable(true);
        button->setChecked(name == currentTheme_);
        button->setIcon(ThemeIcon(paletteFor(name)));
        button->setIconSize(QSize(20, 20));
        button->setToolTip(QStringLiteral("%1 (%2)").arg(themeLabel(name), name));
        button->setAccessibleName(themeLabel(name));
        theme_buttons_.insert(name, button);
        themeLayout->addWidget(button);

        // One action per theme, so the group stays exclusive without a second source of truth.
        auto* action = new QAction(themeLabel(name), button);
        action->setCheckable(true);
        action->setChecked(name == currentTheme_);
        themeActions->addAction(action);
        connect(button, &QToolButton::clicked, this, [this, name, action] {
            action->setChecked(true);
            applyTheme(name);
        });
        connect(action, &QAction::triggered, this, [this, name] { applyTheme(name); });
    }
    toolbar->addWidget(themeGroup);
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

    // The raster home: a plain image of the grid, shown while the engine is stopped (F6). It shares a stack
    // with the live card, so the shell swaps one for the other; the click is mapped back to a card below.
    raster_label_ = new QLabel(root);
    raster_label_->setObjectName(QStringLiteral("antikoltRaster"));
    raster_label_->setAlignment(Qt::AlignCenter);
    raster_label_->setScaledContents(false);
    raster_label_->setCursor(Qt::PointingHandCursor);
    raster_label_->installEventFilter(this);

    card_stack_ = new QStackedLayout;
    card_stack_->setContentsMargins(0, 0, 0, 0);
    card_stack_->addWidget(card_);
    card_stack_->addWidget(raster_label_);
    auto* stack_host = new QWidget(root);
    stack_host->setLayout(card_stack_);

    auto* hint = new QLabel(root);
    hint->setObjectName(QStringLiteral("antikoltHint"));
    hint->setText(QStringLiteral(
        "A motor külön folyamatban fut; a kártya natív ablaka ebbe az ablakba van ágyazva, így a "
        "kép és a bemenet az operációs rendszeren megy át, nem a protokollon."));

    // Find bar (F7 "Ctrl+F"): hidden until the user asks; Enter = next, Shift+Enter = previous, Esc = close.
    find_bar_ = new QWidget(root);
    find_bar_->setObjectName(QStringLiteral("antikoltFindBar"));
    find_bar_->setVisible(false);
    auto* find_layout = new QHBoxLayout(find_bar_);
    find_layout->setContentsMargins(0, 0, 0, 0);
    find_layout->setSpacing(6);
    find_edit_ = new QLineEdit(find_bar_);
    find_edit_->setObjectName(QStringLiteral("antikoltFindEdit"));
    find_edit_->setAccessibleName(t(QStringLiteral("Keresés az oldalon")));
    find_edit_->setPlaceholderText(t(QStringLiteral("Keresés az oldalon…")));
    find_edit_->setClearButtonEnabled(true);
    find_status_ = new QLabel(find_bar_);
    find_status_->setObjectName(QStringLiteral("antikoltFindStatus"));
    find_status_->setAccessibleName(QStringLiteral("Találatok száma"));
    auto* find_close = new QToolButton(find_bar_);
    find_close->setObjectName(QStringLiteral("antikoltFindClose"));
    find_close->setText(QStringLiteral("✕"));
    find_close->setAccessibleName(QStringLiteral("Keresés bezárása"));
    find_close->setAutoRaise(true);
    find_close->setCursor(Qt::PointingHandCursor);
    connect(find_close, &QToolButton::clicked, this, &MainWindow::closeFind);
    find_layout->addWidget(find_edit_, 1);
    find_layout->addWidget(find_status_, 0);
    find_layout->addWidget(find_close, 0);
    connect(find_edit_, &QLineEdit::returnPressed, this, &MainWindow::findNext);
    auto* find_prev = new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Return), find_edit_);
    connect(find_prev, &QShortcut::activated, this, &MainWindow::findPrevious);
    auto* find_esc = new QShortcut(QKeySequence(Qt::Key_Escape), find_edit_);
    connect(find_esc, &QShortcut::activated, this, &MainWindow::closeFind);
    auto* find_ctrl_f = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_F), this);
    find_ctrl_f->setContext(Qt::WindowShortcut);
    connect(find_ctrl_f, &QShortcut::activated, this, &MainWindow::openFind);

    layout->addWidget(find_bar_, 0);
    layout->addWidget(stack_host, 1);
    layout->addWidget(hint, 0);

    setCentralWidget(root);
}

void MainWindow::setStatus(const QString& text) {
    if (status_ != nullptr) {
        status_->setText(text);
    }
    statusBar()->showMessage(text);
}
