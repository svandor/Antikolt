// SPDX-License-Identifier: MPL-2.0

#include "vault_panel.h"

#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "core_client.h"

VaultPanel::VaultPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);

    auto* unlock_row = new QHBoxLayout;
    password_ = new QLineEdit(this);
    password_->setEchoMode(QLineEdit::Password);
    password_->setPlaceholderText(tr("Mesterjelszó"));
    unlock_ = new QPushButton(tr("Feloldás"), this);
    unlock_row->addWidget(password_);
    unlock_row->addWidget(unlock_);
    root->addLayout(unlock_row);

    table_ = new QTableWidget(0, 3, this);
    table_->setHorizontalHeaderLabels({tr("Honlap"), tr("Típus"), tr("Címke")});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    root->addWidget(table_);

    connect(unlock_, &QPushButton::clicked, this, &VaultPanel::unlock);
}

void VaultPanel::setClient(CoreClient* client) {
    client_ = client;
}

void VaultPanel::unlock() {
    if (client_ == nullptr) {
        return;
    }
    client_->vaultUnlock(password_->text(), [this](const QJsonObject&, const QString& error) {
        if (error.isEmpty()) {
            refresh();
        }
    });
}

void VaultPanel::refresh() {
    if (client_ == nullptr) {
        return;
    }
    client_->vaultList([this](const QJsonObject& data, const QString& error) {
        if (!error.isEmpty()) {
            return;
        }
        const QJsonArray credentials = data.value(QStringLiteral("credentials")).toArray();
        table_->setRowCount(credentials.size());
        for (int i = 0; i < credentials.size(); ++i) {
            const QJsonObject credential = credentials.at(i).toObject();
            table_->setItem(i, 0,
                            new QTableWidgetItem(credential.value(QStringLiteral("origin")).toString()));
            table_->setItem(i, 1,
                            new QTableWidgetItem(credential.value(QStringLiteral("kind")).toString()));
            table_->setItem(i, 2,
                            new QTableWidgetItem(credential.value(QStringLiteral("label")).toString()));
        }
    });
}

void VaultPanel::setPassword(const QString& text) {
    password_->setText(text);
}

int VaultPanel::rowCount() const {
    return table_->rowCount();
}

bool VaultPanel::showsText(const QString& text) const {
    for (int row = 0; row < table_->rowCount(); ++row) {
        for (int column = 0; column < table_->columnCount(); ++column) {
            const QTableWidgetItem* item = table_->item(row, column);
            if (item != nullptr && item->text().contains(text)) {
                return true;
            }
        }
    }
    return false;
}
