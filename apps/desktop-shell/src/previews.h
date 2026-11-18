// SPDX-License-Identifier: MPL-2.0
//
// The preview directory's housekeeping.
//
// A preview image is written next to the profile (`previews/<name>`) and referenced by name from the card
// record (ADR-035). Nothing else knows about it, so when a card goes away its picture would stay on disk
// forever. This is the only place that decides which files are orphans: the caller passes the names the core
// still references, and everything else is deleted.
//
// Kept header-only and free of Qt widgets so the self test can drive it against a scratch directory without
// an engine or a core service.

#pragma once

#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <array>

namespace antikolt {

/// The extensions a preview may have; the same list the engine's scheme handler serves (ADR-035).
inline const std::array<const char*, 4>& previewExtensions() {
    static const std::array<const char*, 4> extensions{"png", "jpg", "jpeg", "webp"};
    return extensions;
}

/// Delete every preview file in `previews_dir` whose name is *not* in `live_names`.
///
/// Returns the names it removed, so the caller can say what it did instead of guessing. A missing directory
/// is not an error: it means the browser has never produced a preview.
inline QStringList removeOrphanPreviews(const QString& previews_dir, const QSet<QString>& live_names) {
    QStringList removed;
    QDir dir(previews_dir);
    if (!dir.exists()) {
        return removed;
    }
    QStringList filters;
    for (const char* extension : previewExtensions()) {
        filters << QStringLiteral("*.") + QLatin1String(extension);
    }
    const QFileInfoList files = dir.entryInfoList(filters, QDir::Files);
    for (const QFileInfo& file : files) {
        if (live_names.contains(file.fileName())) {
            continue;
        }
        if (dir.remove(file.fileName())) {
            removed << file.fileName();
        }
    }
    removed.sort();
    return removed;
}

/// Whether a preview must be regenerated: it is missing, or it is older than `max_age_ms`.
///
/// Age is the file's own modification time — a preview is a photograph of a page, so "when was this
/// photographed" is exactly what the filesystem already records, and a second timestamp on the card record
/// would be another thing to keep in sync. A modification time in the future (a clock that moved) is treated
/// as fresh: re-capturing on every open would be worse than a slightly odd timestamp.
inline bool previewNeedsRefresh(const QString& path, qint64 now_ms, qint64 max_age_ms) {
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        return true;
    }
    const qint64 modified_ms = info.lastModified().toMSecsSinceEpoch();
    if (modified_ms <= 0 || now_ms <= modified_ms) {
        return false;
    }
    return (now_ms - modified_ms) > max_age_ms;
}

/// How many previews today's budget has already allowed.
///
/// The budget is a tiny file in the profile (`{"day":"2026-11-17","count":7}`), because the counter must
/// survive a restart: a screenshot loop that resets every launch is not a budget. A counter from another day
/// reads as zero — the day string is the whole state, so no clock arithmetic is stored.
inline int previewUsedToday(const QString& budget_path, const QString& today) {
    QFile file(budget_path);
    if (!file.open(QIODevice::ReadOnly)) {
        return 0;
    }
    const QJsonObject state = QJsonDocument::fromJson(file.readAll()).object();
    if (state.value(QStringLiteral("day")).toString() != today) {
        return 0;
    }
    return state.value(QStringLiteral("count")).toInt();
}

/// Count one preview that this day's budget allowed, replacing the file atomically.
inline void previewConsumeBudget(const QString& budget_path, const QString& today) {
    const QJsonObject state{{QStringLiteral("day"), today},
                            {QStringLiteral("count"), previewUsedToday(budget_path, today) + 1}};
    const QString temporary = budget_path + QStringLiteral(".tmp");
    QFile file(temporary);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    file.write(QJsonDocument(state).toJson(QJsonDocument::Compact));
    file.close();
    QFile::remove(budget_path);
    QFile::rename(temporary, budget_path);
}

/// Whether today's budget still allows one more preview.
inline bool previewBudgetAllows(const QString& budget_path, const QString& today, int daily_max) {
    return previewUsedToday(budget_path, today) < daily_max;
}

}  // namespace antikolt
