// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QDateTime>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QString>
#include <QVector>

namespace antikolt {

/// One imported bookmark: the anchor text and the URL (M7 bookmark import).
struct BookmarkEntry {
    QString title;
    QString url;
};

/// Parse a Netscape-format bookmarks file — the one export Chrome, Firefox and Edge all share — into its
/// `<A HREF>` entries. The format is not valid XML (`<!DOCTYPE NETSCAPE-Bookmark-file-1>`, bare `<p>`), so a
/// tolerant regex over the anchors is the honest parser rather than a stream parser. Folders, separators and
/// `javascript:` links are skipped; the anchor text is the title, the URL is the href.
inline QVector<BookmarkEntry> parseBookmarksHtml(const QString& html) {
    QVector<BookmarkEntry> entries;
    static const QRegularExpression anchor(
        QStringLiteral("<a\\b[^>]*href\\s*=\\s*[\"']([^\"']+)[\"'][^>]*>(.*?)</a>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression stripTags(QStringLiteral("<[^>]*>"));
    auto it = anchor.globalMatch(html);
    while (it.hasNext()) {
        const auto match = it.next();
        const QString url = match.captured(1).trimmed();
        if (url.isEmpty() || url.startsWith(QStringLiteral("javascript:"), Qt::CaseInsensitive)) {
            continue;
        }
        const QString title = match.captured(2).remove(stripTags).trimmed();
        entries.append(BookmarkEntry{title.isEmpty() ? url : title, url});
    }
    return entries;
}

/// A new card id, Crockford base32 — the same shape the grid's own `newCardId` uses: 10 time characters then 16
/// of randomness, so the core's lexicographic ordering stays by creation.
inline QString newCardId() {
    const QString alphabet = QStringLiteral("0123456789ABCDEFGHJKMNPQRSTVWXYZ");
    const qint64 stamp = QDateTime::currentMSecsSinceEpoch();
    QString out;
    for (int shift = 45; shift >= 0; shift -= 5) {
        out += alphabet.at(static_cast<int>((stamp / (1LL << shift)) & 31) % 32);
    }
    for (int i = 0; i < 16; ++i) {
        out += alphabet.at(QRandomGenerator::global()->bounded(32));
    }
    return out.left(26);
}

}  // namespace antikolt
