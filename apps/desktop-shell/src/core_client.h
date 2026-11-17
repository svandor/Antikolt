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

    /// Start the core service. `dataDir` is where it keeps `cards.json`.
    bool start(const QString& executablePath, const QString& dataDir);
    bool isRunning() const;

    void handshake();
    void cardsList(ReplyHandler handler);
    void cardOpen(const QString& cardId, ReplyHandler handler = {});
    /// Create or replace one card (`card_upsert`): the whole record travels, so a pin, a note or a new
    /// card is one call. The core validates it and answers with the saved card.
    void cardUpsert(const QJsonObject& card, ReplyHandler handler = {});
    /// The session ledger and its policy (`docs/HU/10-MUNKAMENET.md`).
    ///
    /// `sessionDue` is the evaluation, not a guess: the core decides which rows are due and hands back
    /// the plan with each of them (ADR-033), so the shell never recomputes a policy.
    void sessionList(ReplyHandler handler = {});
    void sessionUpsert(const QJsonObject& session, ReplyHandler handler = {});
    void sessionDue(qint64 nowMs, bool appLocked, ReplyHandler handler = {});
    void sessionClose(const QString& containerId, const QString& origin, ReplyHandler handler = {});
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
