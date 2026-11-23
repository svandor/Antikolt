// SPDX-License-Identifier: MPL-2.0
//
// The shell's client for the core service.
//
// The core owns the data (cards first, sessions and policies later), so the shell asks it rather than
// keeping a copy. Same transport as the engine link — length-prefixed JSON over a child process's
// stdio — because one framing means one set of hostile-input rules, but a different, smaller message
// set: these are cards, not surfaces.

#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>

class QProcess;

class CoreClient : public QObject {
    Q_OBJECT

public:
    using ReplyHandler = std::function<void(const QJsonObject& data, const QString& error)>;

    explicit CoreClient(QObject* parent = nullptr);
    ~CoreClient() override;

    /// Start the core service. `dataDir` is the profile directory: the cards live in `antikolt.sqlite3` there, and
    /// `cards.json` next to it is the export the core keeps up to date (ADR-069).
    bool start(const QString& executablePath, const QString& dataDir);
    bool isRunning() const;

    void handshake();
    void cardsList(ReplyHandler handler);
    void cardOpen(const QString& cardId, ReplyHandler handler = {});
    /// Create or replace one card (`card_upsert`): the whole record travels, so a pin, a note or a new
    /// card is one call. The core validates it and answers with the saved card.
    void cardUpsert(const QJsonObject& card, ReplyHandler handler = {});
    /// Store the page's text captured when a card was frozen (`card_page_text`, ADR-049).
    ///
    /// A separate op on purpose: the shell does not own the whole card record, so it must not send one it only
    /// half knows — a pin would otherwise overwrite the container or the note it never read.
    /// The container table (ADR-052). The page draws the legend and the card's selector from it, so a rename in
    /// the core is what the next reload shows.
    void containersList(ReplyHandler handler);
    void containerUpsert(const QJsonObject& container, ReplyHandler handler = {});
    void containerDelete(const QString& containerId, const QString& moveTo = QString(),
                         ReplyHandler handler = {});
    /// The workspace table (ADR-056): the tabs the grid shows, and the fact that a card's workspace is a stored
    /// field rather than an index the page invents.
    void workspacesList(ReplyHandler handler);
    void workspaceUpsert(const QJsonObject& workspace, ReplyHandler handler = {});
    void workspaceDelete(const QString& workspaceId, ReplyHandler handler = {});
    /// The portable bundle (ADR-059): the user's data in one file, so a profile can move between machines.
    void profileExport(const QString& path, ReplyHandler handler = {});
    void profileImport(const QString& path, ReplyHandler handler = {});
    /// The database mirror (ADR-064): `sync` writes the JSON stores into the SQLite file and checks the first card.
    void dbStatus(bool sync, ReplyHandler handler = {});
    void cardPageText(const QString& cardId, const QString& text, ReplyHandler handler = {});
    /// Store one card's last-known working set in bytes (`card_memory`, F6, per-card breakdown).
    void cardMemory(const QString& cardId, qint64 bytes, ReplyHandler handler = {});
    /// Remove one card (ADR-061). The page's add/delete loop needs it, and so does a user who mistyped a URL.
    void cardDelete(const QString& cardId, ReplyHandler handler = {});
    /// The session ledger and its policy (`docs/HU/10-MUNKAMENET.md`).
    ///
    /// `sessionDue` is the evaluation, not a guess: the core decides which rows are due and hands back
    /// the plan with each of them (ADR-033), so the shell never recomputes a policy.
    void sessionList(ReplyHandler handler = {});
    void sessionUpsert(const QJsonObject& session, ReplyHandler handler = {});
    void sessionDue(qint64 nowMs, bool appLocked, ReplyHandler handler = {});
    /// Replace the container-wide session rules (`session_rules`, ADR-039). The core validates them.
    void sessionRules(const QJsonObject& rules, ReplyHandler handler = {});
    /// The one row of user preferences (ADR-072): live-card cap, hibernation timings and the memory budget (F6).
    void userPrefs(ReplyHandler handler = {});
    /// Replace the user preferences; the core validates them (ADR-072).
    void userPrefsSet(const QJsonObject& prefs, ReplyHandler handler = {});
    /// Resolve the `Auto` profile for the current conditions (F6, ADR-020): the core maps power + pressure to a
    /// built-in kind and returns its params, so the shell does not re-derive ADR-020's table itself.
    void resolveProfile(bool onAcPower, const QString& pressure, ReplyHandler handler = {});
    void sessionClose(const QString& containerId, const QString& origin, ReplyHandler handler = {});
    /// The password vault (M7, ADR-082/083/084/085). `vaultList` is metadata only; `vaultGet` returns
    /// plaintext and therefore needs the vault open. The core is the one writer and the only cipher.
    void vaultStatus(ReplyHandler handler = {});
    void vaultUnlock(const QString& password, ReplyHandler handler = {});
    void vaultLock(ReplyHandler handler = {});
    void vaultList(ReplyHandler handler = {});
    void vaultCreate(const QJsonObject& credential, ReplyHandler handler = {});
    void vaultGet(const QString& credentialId, ReplyHandler handler = {});
    void vaultDelete(const QString& credentialId, ReplyHandler handler = {});
    void shutdown();

signals:
    void ready(const QJsonObject& coreInfo);
    void failed(const QString& message);

private slots:
    void onReadyRead();
    void onProcessFinished(int exitCode);

private:
    qint64 send(const QString& op, const QJsonObject& fields, ReplyHandler handler);
    void failPending(const QString& message);

    QProcess* process_ = nullptr;
    QByteArray buffer_;
    qint64 next_id_ = 1;
    QHash<qint64, ReplyHandler> pending_;
    bool destroying_ = false;
};
