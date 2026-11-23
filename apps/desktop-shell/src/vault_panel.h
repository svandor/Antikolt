// SPDX-License-Identifier: MPL-2.0
//
// The vault panel: the shell's window onto the password vault (M7). It shows metadata only — the
// list never carries a username or a password — and asks the core to unlock before listing anything.

#pragma once

#include <QWidget>

class CoreClient;
class QLineEdit;
class QPushButton;
class QTableWidget;

class VaultPanel : public QWidget {
    Q_OBJECT

public:
    explicit VaultPanel(QWidget* parent = nullptr);

    /// The core to talk to; the panel keeps no copy of any secret, it asks and forgets.
    void setClient(CoreClient* client);
    /// Ask the core for the metadata list and repaint the table.
    void refresh();
    /// Unlock with the password currently in the field, then refresh.
    void unlock();

    // The harness's window into the panel: set the typed password, count the rows, and ask whether a
    // given string (a secret) leaked into the visible text.
    void setPassword(const QString& text);
    int rowCount() const;
    bool showsText(const QString& text) const;

private:
    CoreClient* client_ = nullptr;
    QLineEdit* password_ = nullptr;
    QPushButton* unlock_ = nullptr;
    QTableWidget* table_ = nullptr;
};
