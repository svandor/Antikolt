// SPDX-License-Identifier: MPL-2.0
//
// The shell's client for the engine host.
//
// The contract is the one in crates/antikolt-ipc and proto/README.md: length-prefixed JSON envelopes
// over the child process's standard IO. The Rust crate is the reference implementation and holds the
// tests; this is the same wire format on the Qt side, using Qt's own JSON so the shell needs no extra
// dependency.
//
// Rules:
//   * the engine host's stdout carries frames only, so nothing here writes to it;
//   * every request has a unique id and a timeout: a silent engine must not freeze the UI;
//   * an event that arrives before a reply is kept, not dropped.

#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>

class QProcess;
class QTimer;

class EngineClient : public QObject {
    Q_OBJECT

public:
    using ReplyHandler = std::function<void(const QJsonObject& result, const QString& error)>;

    explicit EngineClient(QObject* parent = nullptr);
    ~EngineClient() override;

    /// Start the engine host in server mode. `profilePath` becomes its cache directory.
    bool start(const QString& executablePath, const QString& profilePath, bool hidden = false);

    bool isRunning() const;

    /// Engine identity reported by the `welcome` handshake (empty until it arrives).
    QJsonObject engineInfo() const { return engine_info_; }

    /// Send `hello` and wait for `welcome` asynchronously.
    void handshake();

    void createSurface(const QString& partition, int width, int height, int deviceScalePercent,
                       ReplyHandler handler);
    void navigate(qint64 surface, const QString& url, ReplyHandler handler = {});
    void snapshot(qint64 surface, ReplyHandler handler);
    void destroySurface(qint64 surface, ReplyHandler handler = {});
    void shutdown();

signals:
    void engineReady(const QJsonObject& engineInfo);
    void loadFinished(qint64 surface, const QString& url, int httpStatus);
    void loadFailed(qint64 surface, const QString& url, const QString& reason);
    void surfaceCreated(qint64 surface);
    void surfaceClosed(qint64 surface);
    void failed(const QString& message);

private slots:
    void onReadyRead();
    void onProcessError();
    void onProcessFinished(int exitCode);

private:
    qint64 send(const QString& op, const QJsonObject& fields, ReplyHandler handler);
    void handleEnvelope(const QJsonObject& envelope);
    void handleEvent(const QJsonObject& event);
    void failPending(const QString& message);

    QProcess* process_ = nullptr;
    QByteArray buffer_;
    qint64 next_id_ = 1;
    QHash<qint64, ReplyHandler> pending_;
    QJsonObject engine_info_;
};
