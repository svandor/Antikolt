// SPDX-License-Identifier: MPL-2.0

#include "instance_guard.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QProcessEnvironment>

namespace antikolt {

namespace {

/// What a later launch says to the one that owns the name.
constexpr char kShowCommand[] = "show";

/// How long a launch waits for the owner's socket. Local, so this is generous; the value only matters
/// when a dead process left the name behind and Windows still needs a moment to answer.
constexpr int kConnectTimeoutMs = 300;

}  // namespace

QString instanceNameForThisUser() {
    const QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    QString user = environment.value(QStringLiteral("USERNAME"));
    if (user.isEmpty()) {
        user = environment.value(QStringLiteral("USER"));
    }
    if (user.isEmpty()) {
        return {};
    }
    // The user name keeps two accounts on one machine apart, which is what "one Antikolt" means: one per
    // person, not one per workstation.
    return QStringLiteral("antikolt-shell-") + user;
}

InstanceGuard::InstanceGuard(QObject* parent) : QObject(parent) {}

InstanceGuard::~InstanceGuard() {
    if (server_ == nullptr) {
        return;
    }
    server_->close();
    QLocalServer::removeServer(name_);
}

InstanceGuard::Result InstanceGuard::acquire(const QString& name) {
    if (name.isEmpty()) {
        return Result::Failed;
    }
    name_ = name;
    // Asking first is what makes the order reliable: on Windows two processes can both create a named-pipe
    // instance under one name, so "listen succeeded" alone proves nothing. A live owner answers this
    // connection; a name left behind by a crash does not, and only then is it removed and claimed.
    if (askPrimaryToShow(name)) {
        return Result::Secondary;
    }
    QLocalServer::removeServer(name);
    if (startServer(name)) {
        return Result::Primary;
    }
    return Result::Failed;
}

bool InstanceGuard::startServer(const QString& name) {
    if (server_ == nullptr) {
        server_ = new QLocalServer(this);
    }
    if (!server_->listen(name)) {
        return false;
    }
    connect(server_, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* socket = server_->nextPendingConnection()) {
            connect(socket, &QLocalSocket::disconnected, socket, &QLocalSocket::deleteLater);
            // One word over a local socket, from a human clicking a shortcut: waiting for that word is
            // simpler and more reliable than building the read out of readyRead/disconnected, which loses it
            // when the sender has already written and left by the time the event loop gets here (measured:
            // the launch connected and disconnected inside one loop turn and the message was never read).
            // The wait is bounded, and it happens only when somebody actually connects.
            if (socket->bytesAvailable() == 0) {
                socket->waitForReadyRead(kConnectTimeoutMs);
            }
            const QByteArray command = socket->readAll().trimmed();
            if (command == QByteArray(kShowCommand)) {
                qInfo("[shell] another launch asked for the window");
                emit showRequested();
            }
            socket->disconnectFromServer();
        }
    });
    return true;
}

bool InstanceGuard::askPrimaryToShow(const QString& name) {
    QLocalSocket probe;
    probe.connectToServer(name);
    if (!probe.waitForConnected(kConnectTimeoutMs)) {
        return false;
    }
    probe.write(kShowCommand);
    probe.flush();
    probe.waitForBytesWritten(kConnectTimeoutMs);
    probe.disconnectFromServer();
    return true;
}

}  // namespace antikolt
