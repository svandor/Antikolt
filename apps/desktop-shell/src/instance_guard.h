// SPDX-License-Identifier: MPL-2.0
//
// One Antikolt per user session.
//
// The desktop shortcut, the Start menu entry and the tray icon all mean "the same Antikolt". A second
// launch must raise the window that is already running instead of starting a second engine, a second
// profile and a second set of Chromium processes — two browsers that disagree about the card store are
// worse than one that is merely hidden.
//
// The guard is a named local socket: the first process listens on the name, a later one connects, asks the
// first to show itself and leaves. Nothing is written to the registry and a crash cannot leave the name
// unusable for long: the stale socket is removed before the retry (see `InstanceGuard::acquire`).

#pragma once

#include <QObject>
#include <QString>

class QLocalServer;

namespace antikolt {

/// The per-user socket name the shell claims. Empty when the user name cannot be determined.
QString instanceNameForThisUser();

/// Claims one name for this process, and listens for "show yourself" requests from later launches.
class InstanceGuard : public QObject {
    Q_OBJECT

public:
    enum class Result {
        /// This process owns the name: it is the Antikolt the user will see.
        Primary,
        /// Another process owns the name; it was asked to show itself. This one must exit quietly.
        Secondary,
        /// The socket could not be created or reached (exotic session, no local sockets). Run anyway,
        /// with the guard switched off — a browser that refuses to start is worse than a duplicate.
        Failed,
    };

    explicit InstanceGuard(QObject* parent = nullptr);
    ~InstanceGuard() override;

    /// Try to own `name`. Call once, before the window exists.
    Result acquire(const QString& name);

signals:
    /// A later launch asked this instance to come to the front.
    void showRequested();

private:
    bool startServer(const QString& name);
    /// Connect to `name` and ask its owner to show itself. True when somebody answered.
    bool askPrimaryToShow(const QString& name);

    QLocalServer* server_ = nullptr;
    QString name_;
};

}  // namespace antikolt
