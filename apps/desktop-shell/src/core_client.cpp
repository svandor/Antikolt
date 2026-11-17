// SPDX-License-Identifier: MPL-2.0

#include "core_client.h"

#include <QJsonDocument>
#include <QProcess>
#include <QTimer>

namespace {

constexpr int kHeaderSize = 4;
constexpr int kRequestTimeoutMs = 10000;

QByteArray frame(const QJsonObject& envelope) {
    const QByteArray payload = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    QByteArray out;
    out.resize(kHeaderSize);
    const quint32 length = static_cast<quint32>(payload.size());
    out[0] = static_cast<char>(length & 0xFF);
    out[1] = static_cast<char>((length >> 8) & 0xFF);
    out[2] = static_cast<char>((length >> 16) & 0xFF);
    out[3] = static_cast<char>((length >> 24) & 0xFF);
    out.append(payload);
    return out;
}

quint32 decodeLength(const char* bytes) {
    return static_cast<quint32>(static_cast<unsigned char>(bytes[0])) |
           (static_cast<quint32>(static_cast<unsigned char>(bytes[1])) << 8) |
           (static_cast<quint32>(static_cast<unsigned char>(bytes[2])) << 16) |
           (static_cast<quint32>(static_cast<unsigned char>(bytes[3])) << 24);
}

}  // namespace

CoreClient::CoreClient(QObject* parent) : QObject(parent) {}

CoreClient::~CoreClient() {
    // Same rule as the engine link: a queued reply must never reach a partly destroyed owner.
    destroying_ = true;
    pending_.clear();
    blockSignals(true);
    if (process_ != nullptr) {
        process_->kill();
        process_->waitForFinished(3000);
    }
}

bool CoreClient::start(const QString& executablePath, const QString& dataDir) {
    if (process_ != nullptr) {
        return true;
    }
    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process_, &QProcess::readyReadStandardOutput, this, &CoreClient::onReadyRead);
    connect(process_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError) { emit failed(process_->errorString()); });
    connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus) { onProcessFinished(exitCode); });
    process_->start(executablePath, {QStringLiteral("--data-dir"), dataDir});
    if (!process_->waitForStarted(5000)) {
        emit failed(tr("a mag nem indult el: %1").arg(executablePath));
        return false;
    }
    return true;
}

bool CoreClient::isRunning() const {
    return process_ != nullptr && process_->state() == QProcess::Running;
}

void CoreClient::handshake() {
    send(QStringLiteral("hello"), {}, [this](const QJsonObject& data, const QString& error) {
        if (!error.isEmpty()) {
            emit failed(error);
            return;
        }
        emit ready(data);
    });
}

void CoreClient::cardsList(ReplyHandler handler) { send(QStringLiteral("cards_list"), {}, std::move(handler)); }

void CoreClient::cardOpen(const QString& cardId, ReplyHandler handler) {
    QJsonObject fields;
    // `id_of_card` keeps the request id (`id`) unambiguous on the wire.
    fields.insert(QStringLiteral("id_of_card"), cardId);
    send(QStringLiteral("card_open"), fields, std::move(handler));
}

void CoreClient::sessionList(ReplyHandler handler) {
    send(QStringLiteral("session_list"), {}, std::move(handler));
}

void CoreClient::sessionUpsert(const QJsonObject& session, ReplyHandler handler) {
    QJsonObject fields;
    fields.insert(QStringLiteral("session"), session);
    send(QStringLiteral("session_upsert"), fields, std::move(handler));
}

void CoreClient::sessionDue(qint64 nowMs, bool appLocked, ReplyHandler handler) {
    QJsonObject fields;
    // The clock is the caller's, not the core's: a test can ask "what is due in 2100" and get an
    // honest answer, and the real shell passes the same wall clock it shows the user.
    fields.insert(QStringLiteral("now_ms"), static_cast<double>(nowMs));
    fields.insert(QStringLiteral("app_locked"), appLocked);
    send(QStringLiteral("session_due"), fields, std::move(handler));
}

void CoreClient::sessionRules(const QJsonObject& rules, ReplyHandler handler) {
    QJsonObject fields;
    fields.insert(QStringLiteral("rules"), rules);
    send(QStringLiteral("session_rules"), fields, std::move(handler));
}
void CoreClient::sessionClose(const QString& containerId, const QString& origin, ReplyHandler handler) {
    QJsonObject fields;
    fields.insert(QStringLiteral("container_id"), containerId);
    fields.insert(QStringLiteral("origin"), origin);
    send(QStringLiteral("session_close"), fields, std::move(handler));
}

void CoreClient::cardUpsert(const QJsonObject& card, ReplyHandler handler) {
    QJsonObject fields;
    fields.insert(QStringLiteral("card"), card);
    send(QStringLiteral("card_upsert"), fields, std::move(handler));
}

void CoreClient::shutdown() {
    if (!isRunning()) {
        return;
    }
    send(QStringLiteral("shutdown"), {}, nullptr);
    if (process_ != nullptr) {
        process_->waitForFinished(5000);
    }
}

qint64 CoreClient::send(const QString& op, const QJsonObject& fields, ReplyHandler handler) {
    if (!isRunning()) {
        if (handler) {
            handler({}, tr("a mag nem fut"));
        }
        return 0;
    }
    QJsonObject message = fields;
    message.insert(QStringLiteral("op"), op);

    const qint64 id = next_id_++;
    QJsonObject envelope;
    envelope.insert(QStringLiteral("version"), 1);
    envelope.insert(QStringLiteral("id"), static_cast<double>(id));
    for (auto it = message.constBegin(); it != message.constEnd(); ++it) {
        envelope.insert(it.key(), it.value());
    }

    if (handler) {
        pending_.insert(id, handler);
        QTimer::singleShot(kRequestTimeoutMs, this, [this, id] {
            if (pending_.contains(id)) {
                ReplyHandler callback = pending_.take(id);
                callback({}, tr("időtúllépés a mag válaszára"));
            }
        });
    }

    process_->write(frame(envelope));
    process_->waitForBytesWritten(1000);
    return id;
}

void CoreClient::onReadyRead() {
    buffer_.append(process_->readAllStandardOutput());
    for (;;) {
        if (buffer_.size() < kHeaderSize) {
            return;
        }
        const quint32 length = decodeLength(buffer_.constData());
        if (buffer_.size() < kHeaderSize + static_cast<int>(length)) {
            return;
        }
        const QByteArray payload = buffer_.mid(kHeaderSize, static_cast<int>(length));
        buffer_.remove(0, kHeaderSize + static_cast<int>(length));

        QJsonParseError error{};
        const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            emit failed(tr("hibás keret a magtól: %1").arg(error.errorString()));
            continue;
        }
        const QJsonObject reply = document.object();
        const QString op = reply.value(QStringLiteral("op")).toString();
        if (op == QStringLiteral("welcome")) {
            // `welcome` is its own message kind: it answers the `hello` request by echoing its id.
            const qint64 to = static_cast<qint64>(reply.value(QStringLiteral("to")).toDouble());
            ReplyHandler callback;
            if (pending_.contains(to)) {
                callback = pending_.take(to);
            }
            if (callback) {
                callback(reply.value(QStringLiteral("core")).toObject(), QString());
            }
            continue;
        }
        const qint64 to = static_cast<qint64>(reply.value(QStringLiteral("to")).toDouble());
        ReplyHandler callback;
        if (pending_.contains(to)) {
            callback = pending_.take(to);
        }
        if (!callback) {
            continue;
        }
        if (reply.value(QStringLiteral("ok")).toBool()) {
            callback(reply.value(QStringLiteral("data")).toObject(), QString());
        } else {
            const QJsonObject failure = reply.value(QStringLiteral("error")).toObject();
            callback({}, failure.value(QStringLiteral("message")).toString());
        }
    }
}

void CoreClient::onProcessFinished(int exitCode) {
    if (destroying_) {
        return;
    }
    failPending(tr("a mag kilépett (kód %1)").arg(exitCode));
}

void CoreClient::failPending(const QString& message) {
    if (destroying_) {
        pending_.clear();
        return;
    }
    const auto callbacks = pending_;
    pending_.clear();
    for (const ReplyHandler& callback : callbacks) {
        callback({}, message);
    }
}
