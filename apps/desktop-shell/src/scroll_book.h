// SPDX-License-Identifier: MPL-2.0
//
// Where the reader left each card.
//
// A card is a place, and coming back to the top of a long article is losing the reader's place. The
// position is one integer per card, kept in a small file in the profile (`scroll.json`) rather than on the
// card record: it changes on every visit, while the record is the user's data and should not be rewritten
// just because someone scrolled. Nothing here touches the network or the page — the shell asks the engine
// for `window.scrollY` and hands the number over.

#pragma once

#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace antikolt {

/// The positions stored in `path`. A missing or unreadable file is an empty book, not an error.
inline QHash<QString, int> loadScrollBook(const QString& path) {
    QHash<QString, int> book;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return book;
    }
    const QJsonObject stored = QJsonDocument::fromJson(file.readAll()).object();
    for (auto it = stored.constBegin(); it != stored.constEnd(); ++it) {
        const int y = it.value().toInt();
        if (y > 0) {
            book.insert(it.key(), y);
        }
    }
    return book;
}

/// The stored offset for one card, or 0 when it has none (the top of the page).
inline int scrollPositionFor(const QString& path, const QString& card_id) {
    if (card_id.isEmpty()) {
        return 0;
    }
    return loadScrollBook(path).value(card_id, 0);
}

/// Remember where the reader stopped. A non-positive offset *forgets* the card instead of storing "top":
/// the book then only holds positions that mean something, and reopening a fresh card starts at the top.
inline void saveScrollPosition(const QString& path, const QString& card_id, int y) {
    if (card_id.isEmpty()) {
        return;
    }
    QHash<QString, int> book = loadScrollBook(path);
    if (y > 0) {
        book.insert(card_id, y);
    } else {
        book.remove(card_id);
    }
    QJsonObject stored;
    for (auto it = book.constBegin(); it != book.constEnd(); ++it) {
        stored.insert(it.key(), it.value());
    }
    const QString temporary = path + QStringLiteral(".tmp");
    QFile file(temporary);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    file.write(QJsonDocument(stored).toJson(QJsonDocument::Compact));
    file.close();
    QFile::remove(path);
    QFile::rename(temporary, path);
}

}  // namespace antikolt
