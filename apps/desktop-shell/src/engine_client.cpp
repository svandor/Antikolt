// SPDX-License-Identifier: MPL-2.0

#include "engine_client.h"
#include <cstdio>
#include <cstdlib>

#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>
#include <QtDebug>

SurfacePolicy surfacePolicyFromEntry(const QJsonObject& entry) {
    SurfacePolicy policy;
    const QString mode = entry.value(QStringLiteral("proxy_mode")).toString();
    const QString host = entry.value(QStringLiteral("proxy_host")).toString();
    const int port = entry.value(QStringLiteral("proxy_port")).toInt();
    if (mode == QStringLiteral("direct")) {
        policy.proxy = QStringLiteral("direct");
    } else if (mode == QStringLiteral("fixed") && !host.isEmpty() && port > 0) {
        // A host that already names a scheme (`socks5://…`) is honoured as written; a bare host is an HTTP proxy,
        // which is what a `host:port` setting means everywhere else.
        policy.proxy = host.contains(QStringLiteral("://"))
                           ? host
                           : QStringLiteral("http://%1:%2").arg(host).arg(port);
        policy.proxyBypass = entry.value(QStringLiteral("proxy_bypass")).toString();
    }
    policy.permissions = entry.value(QStringLiteral("permissions")).toObject();
    return policy;
}

namespace {

/// One frame: 4-byte little-endian length, then the JSON payload.
constexpr int kHeaderSize = 4;
constexpr int kRequestTimeoutMs = 20000;

/// The engine-link protocol this shell speaks (crates/antikolt-ipc `PROTOCOL_VERSION`).
///
/// Version 2 adds the page's request/reply channel (`bridge_reply`); the two processes ship together, so
/// there is exactly one version in the tree and a mismatch is reported instead of half-working (ADR-034).
constexpr int kProtocolVersion = 2;

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

EngineClient::EngineClient(QObject* parent) : QObject(parent) {}

EngineClient::~EngineClient() {
    // The window that owns us is already partly destroyed, so no callback and no signal may fire any
    // more: a queued reply calling back into a dead MainWindow was an access violation on exit.
    destroying_ = true;
    pending_.clear();
    blockSignals(true);
    if (process_ != nullptr) {
        process_->kill();
        process_->waitForFinished(3000);
    }
}

bool EngineClient::start(const QString& executablePath, const QString& profilePath, bool hidden,
                         const QString& previewsPath) {
    if (process_ != nullptr) {
        return true;
    }
    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process_, &QProcess::readyReadStandardOutput, this, &EngineClient::onReadyRead);
    connect(process_, &QProcess::errorOccurred, this, &EngineClient::onProcessError);
    connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus) { onProcessFinished(exitCode); });

    // Diagnostics from the engine go to stderr; surface them in the shell log rather than hiding them.
    connect(process_, &QProcess::readyReadStandardError, this, [this] {
        const QByteArray text = process_->readAllStandardError().trimmed();
        if (!text.isEmpty()) {
            qInfo().noquote() << "[engine]" << QString::fromUtf8(text);
        }
    });

    QStringList arguments{QStringLiteral("--stdio"), QStringLiteral("--user-data-dir"), profilePath};
    if (hidden) {
        arguments.append(QStringLiteral("--hidden"));
    }
    if (!previewsPath.isEmpty()) {
        arguments.append(QStringLiteral("--previews-dir"));
        arguments.append(previewsPath);
    }
    process_->start(executablePath, arguments);
    if (!process_->waitForStarted(5000)) {
        emit failed(tr("a motorház nem indult el: %1").arg(executablePath));
        return false;
    }
    return true;
}

bool EngineClient::isRunning() const {
    return process_ != nullptr && process_->state() == QProcess::Running;
}

qint64 EngineClient::send(const QString& op, const QJsonObject& fields, ReplyHandler handler) {
    if (!isRunning()) {
        if (handler) {
            handler({}, tr("a motorház nem fut"));
        }
        return 0;
    }

    QJsonObject message = fields;
    message.insert(QStringLiteral("op"), op);

    const qint64 id = next_id_++;
    QJsonObject envelope;
    envelope.insert(QStringLiteral("version"), kProtocolVersion);
    envelope.insert(QStringLiteral("id"), static_cast<double>(id));
    envelope.insert(QStringLiteral("message"), message);

    if (handler) {
        pending_.insert(id, handler);
        // A silent engine must never freeze the UI: every request gets a deadline.
        QTimer::singleShot(kRequestTimeoutMs, this, [this, id] {
            if (pending_.contains(id)) {
                ReplyHandler callback = pending_.take(id);
                callback({}, tr("időtúllépés a motorház válaszára"));
            }
        });
    }

    // Diagnostic channel (stderr), off by default: a wedged engine used to freeze the shell with no trace. With
    // ANTIKOLT_ENGINE_TRACE=1 these markers name the exact blocking call; the frame itself is small and the markers
    // never touch stdout's frame stream.
    const bool trace = [] {
        const char* value = std::getenv("ANTIKOLT_ENGINE_TRACE");
        return value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0;
    }();
    const auto mark = [trace](const char* stage, const QString& name, qint64 request, qint64 bytes) {
        if (!trace) {
            return;
        }
        std::fprintf(stderr, "[engine-client] op=%s id=%lld %s bytes=%lld\n", qUtf8Printable(name),
                     static_cast<long long>(request), stage, static_cast<long long>(bytes));
        std::fflush(stderr);
    };
    const QByteArray bytes = frame(envelope);
    mark("write", op, id, bytes.size());
    process_->write(bytes);
    mark("wrote", op, id, bytes.size());
    process_->waitForBytesWritten(1000);
    mark("flushed", op, id, bytes.size());
    return id;
}

void EngineClient::handshake() {
    QJsonObject fields;
    fields.insert(QStringLiteral("protocol"), kProtocolVersion);
    fields.insert(QStringLiteral("engine"), QStringLiteral("antikolt-shell"));
    send(QStringLiteral("hello"), fields, nullptr);
}

void EngineClient::createSurface(const QString& partition, int width, int height,
                                 int deviceScalePercent, quintptr parentWindow,
                                 ReplyHandler handler, bool hidden, const SurfacePolicy& policy) {
    QJsonObject viewport;
    viewport.insert(QStringLiteral("width"), width);
    viewport.insert(QStringLiteral("height"), height);
    viewport.insert(QStringLiteral("device_scale_percent"), deviceScalePercent);

    QJsonObject config;
    config.insert(QStringLiteral("partition"), partition);
    config.insert(QStringLiteral("viewport"), viewport);
    config.insert(QStringLiteral("is_primary"), true);
    // The container and its policy (ADR-071). Additive, optional keys: an engine that does not know them ignores them,
    // which is what keeps this wire change compatible (proto/README.md rule).
    if (!policy.container.isEmpty()) {
        config.insert(QStringLiteral("container"), policy.container);
    }
    if (!policy.proxy.isEmpty()) {
        config.insert(QStringLiteral("proxy"), policy.proxy);
    }
    if (!policy.proxyBypass.isEmpty()) {
        config.insert(QStringLiteral("proxy_bypass"), policy.proxyBypass);
    }
    if (!policy.permissions.isEmpty()) {
        config.insert(QStringLiteral("permissions"), policy.permissions);
    }
    if (hidden) {
        // Additive, optional field (ADR-041): the engine keeps the surface's own window unseen, which is
        // what lets a preview be photographed while the user is looking at the grid.
        config.insert(QStringLiteral("hidden"), true);
    }
    if (parentWindow != 0) {
        // Kept as a JSON number on purpose: the protocol defines the handle as an integer, and the
        // engine host rejects anything above 2^53 rather than guessing.
        config.insert(QStringLiteral("parent_window"),
                      static_cast<double>(static_cast<quint64>(parentWindow)));
    }

    QJsonObject fields;
    fields.insert(QStringLiteral("config"), config);
    send(QStringLiteral("create_surface"), fields, std::move(handler));
}

void EngineClient::setBounds(qint64 surface, int x, int y, int width, int height,
                             ReplyHandler handler) {
    QJsonObject bounds;
    bounds.insert(QStringLiteral("x"), x);
    bounds.insert(QStringLiteral("y"), y);
    bounds.insert(QStringLiteral("width"), width);
    bounds.insert(QStringLiteral("height"), height);

    QJsonObject fields;
    fields.insert(QStringLiteral("surface"), static_cast<double>(surface));
    fields.insert(QStringLiteral("bounds"), bounds);
    send(QStringLiteral("set_bounds"), fields, std::move(handler));
}

void EngineClient::focus(qint64 surface, bool focused, ReplyHandler handler) {
    QJsonObject fields;
    fields.insert(QStringLiteral("surface"), static_cast<double>(surface));
    fields.insert(QStringLiteral("focused"), focused);
    send(QStringLiteral("focus"), fields, std::move(handler));
}

void EngineClient::evaluate(qint64 surface, const QString& script, ReplyHandler handler) {
    QJsonObject fields;
    fields.insert(QStringLiteral("surface"), static_cast<double>(surface));
    fields.insert(QStringLiteral("script"), script);
    send(QStringLiteral("eval"), fields, std::move(handler));
}

void EngineClient::fillCredential(qint64 surface, const QString& username, const QString& password,
                                  ReplyHandler handler) {
    QJsonObject fields;
    fields.insert(QStringLiteral("surface"), static_cast<double>(surface));
    fields.insert(QStringLiteral("username"), username);
    fields.insert(QStringLiteral("password"), password);
    send(QStringLiteral("fill_credential"), fields, std::move(handler));
}

void EngineClient::webauthnSetup(qint64 surface, ReplyHandler handler) {
    QJsonObject fields;
    fields.insert(QStringLiteral("surface"), static_cast<double>(surface));
    send(QStringLiteral("webauthn_setup"), fields, std::move(handler));
}

void EngineClient::webauthnCredentials(qint64 surface, const QString& authenticatorId,
                                       ReplyHandler handler) {
    QJsonObject fields;
    fields.insert(QStringLiteral("surface"), static_cast<double>(surface));
    fields.insert(QStringLiteral("authenticator_id"), authenticatorId);
    send(QStringLiteral("webauthn_credentials"), fields, std::move(handler));
}

void EngineClient::webauthnAddCredential(qint64 surface, const QString& authenticatorId,
                                         ReplyHandler handler) {
    QJsonObject fields;
    fields.insert(QStringLiteral("surface"), static_cast<double>(surface));
    fields.insert(QStringLiteral("authenticator_id"), authenticatorId);
    send(QStringLiteral("webauthn_add_credential"), fields, std::move(handler));
}

void EngineClient::installBridge(qint64 surface, ReplyHandler handler) {
    QJsonObject fields;
    fields.insert(QStringLiteral("surface"), static_cast<double>(surface));
    send(QStringLiteral("install_bridge"), fields, std::move(handler));
}

void EngineClient::navigate(qint64 surface, const QString& url, ReplyHandler handler) {
    QJsonObject fields;
    fields.insert(QStringLiteral("surface"), static_cast<double>(surface));
    fields.insert(QStringLiteral("url"), url);
    send(QStringLiteral("navigate"), fields, std::move(handler));
}

void EngineClient::snapshot(qint64 surface, ReplyHandler handler) {
    QJsonObject fields;
    fields.insert(QStringLiteral("surface"), static_cast<double>(surface));
    send(QStringLiteral("snapshot"), fields, std::move(handler));
}

void EngineClient::capture(qint64 surface, const QString& format, int quality,
                           ReplyHandler handler) {
    QJsonObject fields;
    fields.insert(QStringLiteral("surface"), static_cast<double>(surface));
    fields.insert(QStringLiteral("format"), format);
    fields.insert(QStringLiteral("quality"), quality);
    send(QStringLiteral("capture"), fields, std::move(handler));
}

void EngineClient::clearOriginData(qint64 surface, const QString& origin,
                                   const QStringList& storages, ReplyHandler handler) {
    QJsonObject fields;
    fields.insert(QStringLiteral("surface"), static_cast<double>(surface));
    fields.insert(QStringLiteral("origin"), origin);
    QJsonArray list;
    for (const QString& storage : storages) {
        list.append(storage);
    }
    fields.insert(QStringLiteral("storages"), list);
    send(QStringLiteral("clear_origin_data"), fields, std::move(handler));
}

void EngineClient::destroySurface(qint64 surface, ReplyHandler handler) {
    QJsonObject fields;
    fields.insert(QStringLiteral("surface"), static_cast<double>(surface));
    send(QStringLiteral("destroy_surface"), fields, std::move(handler));
}

void EngineClient::scroll(qint64 surface, int x, int y, bool animate, ReplyHandler handler) {
    QJsonObject fields;
    fields.insert(QStringLiteral("surface"), static_cast<double>(surface));
    fields.insert(QStringLiteral("x"), x);
    fields.insert(QStringLiteral("y"), y);
    fields.insert(QStringLiteral("animate"), animate);
    send(QStringLiteral("scroll"), fields, std::move(handler));
}

void EngineClient::setZoom(qint64 surface, int factorPercent, ReplyHandler handler) {
    QJsonObject fields;
    fields.insert(QStringLiteral("surface"), static_cast<double>(surface));
    fields.insert(QStringLiteral("factor_percent"), factorPercent);
    send(QStringLiteral("set_zoom"), fields, std::move(handler));
}

void EngineClient::setMedia(qint64 surface, const QString& media, ReplyHandler handler) {
    QJsonObject fields;
    fields.insert(QStringLiteral("surface"), static_cast<double>(surface));
    fields.insert(QStringLiteral("media"), media);
    send(QStringLiteral("set_media"), fields, std::move(handler));
}

void EngineClient::setPageDarkMode(qint64 surface, const QString& mode, ReplyHandler handler) {
    QJsonObject fields;
    fields.insert(QStringLiteral("surface"), static_cast<double>(surface));
    fields.insert(QStringLiteral("mode"), mode);
    send(QStringLiteral("set_forced_dark"), fields, std::move(handler));
}

void EngineClient::setVideoCap(qint64 surface, int maxHeight, ReplyHandler handler) {
    QJsonObject fields;
    fields.insert(QStringLiteral("surface"), static_cast<double>(surface));
    fields.insert(QStringLiteral("max_height"), maxHeight);
    send(QStringLiteral("set_video_cap"), fields, std::move(handler));
}

void EngineClient::metrics(qint64 surface, ReplyHandler handler) {
    QJsonObject fields;
    fields.insert(QStringLiteral("surface"), static_cast<double>(surface));
    send(QStringLiteral("metrics"), fields, std::move(handler));
}

void EngineClient::printPdf(qint64 surface, const QString& path, ReplyHandler handler) {
    QJsonObject fields;
    fields.insert(QStringLiteral("surface"), static_cast<double>(surface));
    fields.insert(QStringLiteral("path"), path);
    send(QStringLiteral("print_pdf"), fields, std::move(handler));
}

void EngineClient::find(qint64 surface, const QString& text, bool findNext, bool forward,
                        ReplyHandler handler) {
    QJsonObject fields;
    fields.insert(QStringLiteral("surface"), static_cast<double>(surface));
    fields.insert(QStringLiteral("text"), text);
    fields.insert(QStringLiteral("find_next"), findNext);
    fields.insert(QStringLiteral("forward"), forward);
    send(QStringLiteral("find"), fields, std::move(handler));
}

void EngineClient::stopFind(qint64 surface, ReplyHandler handler) {
    QJsonObject fields;
    fields.insert(QStringLiteral("surface"), static_cast<double>(surface));
    send(QStringLiteral("stop_find"), fields, std::move(handler));
}

void EngineClient::download(qint64 surface, const QString& url, ReplyHandler handler) {
    QJsonObject fields;
    fields.insert(QStringLiteral("surface"), static_cast<double>(surface));
    fields.insert(QStringLiteral("url"), url);
    send(QStringLiteral("download"), fields, std::move(handler));
}

void EngineClient::bridgeReply(qint64 request, const QJsonObject& json, const QString& error) {
    QJsonObject fields;
    fields.insert(QStringLiteral("request"), static_cast<double>(request));
    if (error.isEmpty()) {
        // The engine forwards this string to the page verbatim; JSON keeps the answer typed.
        fields.insert(QStringLiteral("json"), QString::fromUtf8(
                                                   QJsonDocument(json).toJson(QJsonDocument::Compact)));
    } else {
        fields.insert(QStringLiteral("error"), error);
    }
    send(QStringLiteral("bridge_reply"), fields, nullptr);
}

void EngineClient::shutdown() {
    if (!isRunning()) {
        return;
    }
    send(QStringLiteral("shutdown"), {}, nullptr);
    if (process_ != nullptr) {
        process_->waitForFinished(5000);
    }
}

void EngineClient::onReadyRead() {
    buffer_.append(process_->readAllStandardOutput());

    // Parse as many complete frames as the buffer holds; a partial frame simply waits.
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
            emit failed(tr("hibás keret a motorháztól: %1").arg(error.errorString()));
            continue;
        }
        handleEnvelope(document.object());
    }
}

void EngineClient::handleEnvelope(const QJsonObject& envelope) {
    const QJsonObject message = envelope.value(QStringLiteral("message")).toObject();
    const QString op = message.value(QStringLiteral("op")).toString();

    if (op == QStringLiteral("welcome")) {
        engine_info_ = message.value(QStringLiteral("engine")).toObject();
        emit engineReady(engine_info_);
        return;
    }
    if (op == QStringLiteral("event")) {
        handleEvent(message.value(QStringLiteral("event")).toObject());
        return;
    }
    if (op == QStringLiteral("reply")) {
        const qint64 to = static_cast<qint64>(message.value(QStringLiteral("to")).toDouble());
        const QJsonObject outcome = message.value(QStringLiteral("outcome")).toObject();
        ReplyHandler callback;
        if (pending_.contains(to)) {
            callback = pending_.take(to);
        }
        if (!callback) {
            return;
        }
        if (outcome.value(QStringLiteral("kind")).toString() == QStringLiteral("ok")) {
            callback(outcome.value(QStringLiteral("json")).toObject(), QString());
        } else {
            callback({}, outcome.value(QStringLiteral("message")).toString());
        }
    }
}

void EngineClient::handleEvent(const QJsonObject& event) {
    const QString type = event.value(QStringLiteral("type")).toString();
    const qint64 surface = static_cast<qint64>(event.value(QStringLiteral("surface")).toDouble());

    if (type == QStringLiteral("load_finished")) {
        emit loadFinished(surface, event.value(QStringLiteral("url")).toString(),
                          event.value(QStringLiteral("http_status")).toInt());
    } else if (type == QStringLiteral("load_failed")) {
        emit loadFailed(surface, event.value(QStringLiteral("url")).toString(),
                        event.value(QStringLiteral("reason")).toString());
    } else if (type == QStringLiteral("surface_created")) {
        emit surfaceCreated(surface);
    } else if (type == QStringLiteral("surface_closed")) {
        emit surfaceClosed(surface);
    } else if (type == QStringLiteral("popup_requested")) {
        emit popupRequested(surface, event.value(QStringLiteral("url")).toString(),
                            event.value(QStringLiteral("disposition")).toString());
    } else if (type == QStringLiteral("popup_closed")) {
        emit popupClosed(surface);
    } else if (type == QStringLiteral("bridge_call")) {
        emit bridgeCall(surface, event.value(QStringLiteral("method")).toString(),
                        event.value(QStringLiteral("args")).toObject(),
                        static_cast<qint64>(event.value(QStringLiteral("request")).toDouble()));
    } else if (type == QStringLiteral("find_result")) {
        emit findResult(surface, event.value(QStringLiteral("count")).toInt(),
                        event.value(QStringLiteral("active")).toInt());
    } else if (type == QStringLiteral("download_finished")) {
        emit downloadFinished(surface, event.value(QStringLiteral("path")).toString(),
                              event.value(QStringLiteral("success")).toBool());
    } else if (type == QStringLiteral("renderer_terminated")) {
        emit rendererTerminated(surface, event.value(QStringLiteral("status")).toInt(),
                                event.value(QStringLiteral("code")).toInt());
    }
}

void EngineClient::onProcessError() {
    emit failed(tr("motorházhálózat hiba: %1").arg(process_->errorString()));
}

void EngineClient::onProcessFinished(int exitCode) {
    failPending(tr("a motorház kilépett (kód %1)").arg(exitCode));
    // A dead engine must not block a restart (T2, raster home): drop the finished QProcess so `start()` spawns a
    // fresh one, and clear the protocol state that belonged to the old process.
    if (process_ != nullptr) {
        process_->deleteLater();
        process_ = nullptr;
    }
    buffer_.clear();
    engine_info_ = QJsonObject();
}

void EngineClient::failPending(const QString& message) {
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
