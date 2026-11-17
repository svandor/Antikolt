// SPDX-License-Identifier: MPL-2.0

#include "main_window.h"

#include <QActionGroup>
#include <QCheckBox>
#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QHash>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

#include "core_client.h"
#include "engine_client.h"
#include "previews.h"
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
                // The emulation belongs to the page that is showing, so a navigation forgets it: the
                // remembered preference is applied again on every load (ADR-037).
                if (page_media_ != QStringLiteral("auto") && engine_ != nullptr) {
                    engine_->setMedia(surface, page_media_, nullptr);
                }
                // Same for the zoom: it is a preference of this browser window, not of one page (ADR-038).
                if (zoom_percent_ != 100 && engine_ != nullptr) {
                    engine_->setZoom(surface, zoom_percent_, nullptr);
                }
                // The grid must show the cards the core owns, so the list is pushed once the page is
                // there to receive it. Only the home surface gets it; a real card is not a grid.
                if (surface == home_surface_) {
                    live_origin_.clear();
                    pushCardsToGrid(surface);
                    return;
                }
                // A card finished loading: if the policy queued this origin (see cleanupSessions), this
                // is the moment to wipe it — on its own surface, with the plan's storage list.
                live_origin_ = originOfUrl(url);
                producePreview(surface);
                const auto queued = wipe_on_open_.find(live_origin_);
                if (live_origin_.isEmpty() || queued == wipe_on_open_.end()) {
                    return;
                }
                const QString origin = live_origin_;
                const PendingWipe pending = queued.value();
                wipe_on_open_.erase(queued);
                engine_->clearOriginData(
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
    connect(engine_, &EngineClient::loadFailed, this,
            [this](qint64 surface, const QString& url, const QString& reason) {
                setStatus(QStringLiteral("betöltési hiba (#%1): %2 — %3").arg(surface).arg(url, reason));
            });

    // The card owns the surface lifecycle locally; the window mirrors it for the status bar.
    connect(card_, &LiveCard::surfaceAttached, this, [this](qint64 surface) {
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
        engine_->navigate(surface, target);
        if (opening_home) {
            // The grid is the only page allowed to talk to the shell. Installing it here (before the
            // load finishes) is enough: the engine re-injects it whenever the page reloads.
            engine_->installBridge(surface);
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
        setStatus(QStringLiteral("kártya #%1 lemezre mentve · memória felszabadítva").arg(surface));
    });
    connect(engine_, &EngineClient::bridgeCall, this, &MainWindow::handleBridgeCall);

    // The hibernation clock: ten minutes of no user activity put the live card back on disk, which is
    // the memory promise in one timer. `ANTIKOLT_HIBERNATE_MS` shortens it for the self test; without
    // that a check would have to wait ten minutes to prove anything.
    const int configured_ms = qEnvironmentVariableIntValue("ANTIKOLT_HIBERNATE_MS");
    idle_ = new QTimer(this);
    idle_->setSingleShot(true);
    idle_->setInterval(configured_ms > 0 ? configured_ms : 10 * 60 * 1000);
    connect(idle_, &QTimer::timeout, this, &MainWindow::onIdleTimeout);
    // Activity is what the user does, not what the engine reports: mouse and keyboard, wherever they
    // land inside the shell. The page's own input arrives in the engine's process, so the card also
    // wakes the clock through the bridge (a page that is not touched never sends anything).
    qApp->installEventFilter(this);

    setStatus(QStringLiteral("motor: nincs elindítva · kártyák: 0"));
    applyTheme(QStringLiteral("grey"));
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
    hibernating_ = true;
    // The one documented exception a user would feel: a page that is playing sound or video keeps its
    // card alive (docs/HU/02 §4.2). Everything else goes to disk, unpinned or not.
    engine_->evaluate(
        surface_,
        QStringLiteral("(function(){var m=document.querySelectorAll('video,audio');"
                       "for(var i=0;i<m.length;i++){if(!m[i].paused&&!m[i].ended)return 'playing';}"
                       "return 'idle';})()"),
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

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
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
    closeLiveCard();
    if (core_ != nullptr) {
        core_->shutdown();
    }
    if (engine_ != nullptr) {
        engine_->shutdown();
    }
}

void MainWindow::closeLiveCard() {
    if (engine_ == nullptr || card_ == nullptr) {
        return;
    }
    const qint64 live = card_->surface();
    if (live <= 0) {
        return;
    }
    // Ask for the close and let `shutdown()` be the barrier: it waits for the engine process to exit,
    // and the engine cannot exit before it has closed the browser. Pumping Qt events here instead
    // re-enters widget destruction and crashed the process on the way out.
    engine_->destroySurface(live, nullptr);
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

    if (!engine_->start(enginePath, profile)) {
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
        });
    }
    if (!core_->isRunning()) {
        const QString corePath = coreExecutable();
        const QString coreData =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
            QStringLiteral("/core");
        QDir().mkpath(coreData);
        if (corePath.isEmpty()) {
            setStatus(QStringLiteral("a mag nem található (ANTIKOLT_CORE) — a rács egyelőre mintaadatot mutat"));
        } else if (core_->start(corePath, coreData)) {
            core_->handshake();
        }
    }
}

void MainWindow::ensureSurface() {
    if (!engine_->isRunning()) {
        return;
    }
    if (card_->hasSurface()) {
        engine_->navigate(card_->surface(), pending_url_.isEmpty() ? homePageUrl() : pending_url_);
        return;
    }
    // One partition per card: two cards never share cookies or storage by accident (ADR-009).
    const QString partition =
        pending_url_.isEmpty()
            ? QStringLiteral("persist:home")
            : QStringLiteral("persist:card_%1")
                  .arg(QString::fromLatin1(
                      QCryptographicHash::hash(pending_url_.toUtf8(), QCryptographicHash::Sha1)
                          .toHex()
                          .left(12)));
    if (!card_->attach(engine_, partition)) {
        setStatus(QStringLiteral("a kártya nem tudott felületet kérni a motortól"));
    }
}

void MainWindow::openCard(const QString& url, const QString& title) {
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
    engine_->snapshot(previous, [this, previous, title](const QJsonObject& result,
                                                        const QString& error) {
        if (error.isEmpty()) {
            setStatus(QStringLiteral("előző kártya mentve (%1 bájt) · váltás: %2")
                          .arg(result.value(QStringLiteral("bytes")).toInt())
                          .arg(title.isEmpty() ? pending_url_ : title));
        } else {
            setStatus(QStringLiteral("az előző kártya mentése nem sikerült: %1").arg(error));
        }
        card_->detach();
        engine_->destroySurface(previous, [this](const QJsonObject&, const QString&) {
            ensureSurface();
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
        openCard(url, title);
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
            const QString query = args.value(QStringLiteral("q")).toString().trimmed().toLower();
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
                    const QString haystack =
                        (card.value(QStringLiteral("title")).toString() + QLatin1Char(' ') +
                         card.value(QStringLiteral("url")).toString() + QLatin1Char(' ') +
                         card.value(QStringLiteral("notes")).toString())
                            .toLower();
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
        // A deliberately unanswered method, for the timeout check: it exists only when the environment
        // asks for it, so a normal run cannot reach it and no page can hang the shell by accident.
        if (qEnvironmentVariableIsSet("ANTIKOLT_BRIDGE_TEST") &&
            method == QStringLiteral("test.hang")) {
            return;
        }
        if (method == QStringLiteral("system.memory")) {
            // The page asks what the browser costs; the engine measures the live surface. The budget is
            // not a measurement and is not invented here: until profiles carry a memory budget (F6) it is
            // reported as 0, and the page shows the used figure alone.
            if (engine_ == nullptr || !engine_->isRunning() || surface_ == 0) {
                replyToPage(request, {}, QStringLiteral("nincs élő felület"));
                return;
            }
            engine_->metrics(surface_, [this, request](const QJsonObject& measured,
                                                       const QString& error) {
                if (!error.isEmpty()) {
                    replyToPage(request, {}, error);
                    return;
                }
                const qint64 used = measured.value(QStringLiteral("engine_working_set_bytes"))
                                        .toString()
                                        .toLongLong();
                replyToPage(request, QJsonObject{{QStringLiteral("usedBytes"), static_cast<double>(used)},
                                                 {QStringLiteral("budgetBytes"), 0}});
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
}

void MainWindow::loadHome() {
    if (!engine_->isRunning()) {
        setStatus(QStringLiteral("a motor még nem fut — nyomd meg a „Motor indítása” gombot"));
        return;
    }
    // Going home means leaving whatever card is open, so no pending URL may survive this.
    pending_url_.clear();
    pending_title_.clear();
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
        engine_->destroySurface(surface, [this](const QJsonObject&, const QString&) {
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
        sessions_table_ = new QTableWidget(0, 5, panel);
        sessions_table_->setHorizontalHeaderLabels(
            {QStringLiteral("Origin"), QStringLiteral("Állapot"), QStringLiteral("Politika"),
             QStringLiteral("Sütik"), QStringLiteral("Lejárt")});
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
    if (engine_ == nullptr || !engine_->isRunning() || surface_ == 0) {
        setStatus(QStringLiteral("nagyítás: %1% (nincs élő felület)").arg(clamped));
        return;
    }
    engine_->setZoom(surface_, clamped, [this, clamped](const QJsonObject&, const QString& error) {
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
    if (engine_ == nullptr || !engine_->isRunning() || surface_ == 0) {
        setStatus(QStringLiteral("oldal-téma: %1 (nincs élő felület)").arg(media));
        return;
    }
    engine_->setMedia(surface_, media, [this, media](const QJsonObject&, const QString& error) {
        setStatus(error.isEmpty() ? QStringLiteral("oldal-téma: %1").arg(media)
                                  : QStringLiteral("oldal-téma hiba: %1").arg(error));
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
        if (record.isEmpty() || !record.value(QStringLiteral("preview_ref")).toString().isEmpty()) {
            qInfo("[shell] preview: no record or already has one (%s)",
                  record.isEmpty() ? "no record" : "has preview_ref");
            return;  // unknown card, or one that already has a picture
        }
        const QString name = card_id + QStringLiteral(".png");
        const QString target = engine_profile_ + QStringLiteral("/previews/") + name;
        // The record learns the name; the grid shows the picture the next time it loads, because the push
        // carries the record and the engine serves the file (ADR-035).
        const auto remember = [this, record, name](const QString&) {
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
        if (QFileInfo::exists(target)) {
            remember(name);
            return;
        }
        // A capture can stall behind a GPU hiccup (measured: `Page.captureScreenshot` sometimes answers only
        // after the GPU process has restarted twice), so one retry is cheaper than a lost preview. The
        // engine's own request timeout bounds each attempt; after the last one the preview is skipped and
        // the status line says so — a failed preview is not a failed browser.
        const auto capture_to = [this, surface, target, remember](auto&& self, int attempt) -> void {
            engine_->capture(
                surface, QStringLiteral("png"), 80,
                [this, target, remember, self, attempt](const QJsonObject& shot,
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
                    // `capture` writes into `screenshots/`; the preview belongs in `previews/` under the
                    // name the card carries, so the engine's scheme handler finds it.
                    QFile::remove(target);
                    if (!QFile::copy(written, target)) {
                        qInfo("[shell] preview: copy failed to %s", qUtf8Printable(target));
                        setStatus(QStringLiteral("az előnézet nem került a profilba: %1").arg(target));
                        return;
                    }
                    qInfo("[shell] preview: copied to %s", qUtf8Printable(target));
                    remember(target);
                });
        };
        capture_to(capture_to, 1);
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
                    sessions_table_->setItem(
                        row, 2,
                        new QTableWidgetItem(entry.value(QStringLiteral("policy")).toString()));
                    sessions_table_->setItem(
                        row, 3,
                        new QTableWidgetItem(QString::number(
                            entry.value(QStringLiteral("cookie_count")).toInt())));
                    sessions_table_->setItem(
                        row, 4,
                        new QTableWidgetItem(due_origins.contains(origin)
                                                 ? QStringLiteral("lejárt")
                                                 : QStringLiteral("—")));
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
            int immediate = 0;
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
                if (!live_origin_.isEmpty() && live_origin_ == origin && surface_ != 0) {
                    // The origin is on screen: wipe it now, on the surface it owns.
                    engine_->clearOriginData(
                        surface_, origin, storages,
                        [this, origin, container = pending.container](const QJsonObject&,
                                                                     const QString& failure) {
                            if (!failure.isEmpty()) {
                                setStatus(QStringLiteral("takarítás hiba (%1): %2")
                                              .arg(origin, failure));
                                return;
                            }
                            core_->sessionClose(container, origin,
                                                [this](const QJsonObject&, const QString&) {
                                                    refreshSessions();
                                                });
                        });
                    ++immediate;
                } else {
                    wipe_on_open_.insert(origin, pending);
                }
            }
            setStatus(QStringLiteral("takarítás: %1 azonnal, %2 a kártya következő megnyitásakor")
                          .arg(immediate)
                          .arg(wipe_on_open_.size()));
            if (sessions_dock_ != nullptr && sessions_dock_->isVisible()) {
                refreshSessions();
            }
        });
}


void MainWindow::buildToolBar() {    auto* toolbar = addToolBar(QStringLiteral("Fő"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);

    toolbar->addAction(QStringLiteral("Motor indítása"), this, &MainWindow::startEngine);
    toolbar->addAction(QStringLiteral("⌂ Kezdőlap"), this, &MainWindow::loadHome);
    toolbar->addAction(QStringLiteral("❄ Fagyasztás"), this, &MainWindow::freezeCard);
    toolbar->addAction(QStringLiteral("🗂 Munkamenetek"), this, &MainWindow::showSessions);
    // Site themes (ADR-037): the page decides what `prefers-color-scheme` means; this only reports it.
    auto* siteTheme = toolbar->addAction(QStringLiteral("🌓 Oldal-téma: auto"));
    siteTheme->setObjectName(QStringLiteral("siteThemeAction"));
    connect(siteTheme, &QAction::triggered, this, [this, siteTheme] {
        const QStringList modes = {QStringLiteral("auto"), QStringLiteral("dark"),
                                   QStringLiteral("light")};
        const int next = (modes.indexOf(page_media_) + 1) % modes.size();
        setPageMedia(modes.at(next));
        siteTheme->setText(QStringLiteral("🌓 Oldal-téma: %1").arg(page_media_));
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
