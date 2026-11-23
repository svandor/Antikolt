// SPDX-License-Identifier: MPL-2.0
//
// The shell's user-facing language switch (HU/EN).
//
// Hungarian is the authoritative language (the source strings are Hungarian), so the map below is Hungarian →
// English and the default is Hungarian. The switch is a *runtime* table, not Qt's `QTranslator`: the chrome
// strings are few and this keeps the translation in one header the self test can drive without a `.qm` build
// step. Internal error messages in `core_client`/`engine_client` already use Qt's `tr()`; they are a separate
// concern and are left as-is for now.
//
// Kept header-only and free of Qt widgets so `--languagetest` can prove the mapping without a window.

#pragma once

#include <QHash>
#include <QString>

namespace antikolt {

/// The two languages the shell speaks. `hu` is the default and the fallback for anything unrecognised.
inline const QString& kDefaultLanguage() {
    static const QString hu = QStringLiteral("hu");
    return hu;
}

/// Translate a Hungarian source string into the requested language. `en` returns the mapped English; any other
/// value (including the default `hu`, and unknown strings) returns the source unchanged — a missing translation
/// is never a crash or an empty label, it is the Hungarian the string already was.
inline QString uiText(const QString& hu, const QString& lang) {
    if (lang != QStringLiteral("en")) {
        return hu;
    }
    static const QHash<QString, QString> en = [] {
        QHash<QString, QString> map;
        // Toolbar.
        map.insert(QStringLiteral("Motor indítása"), QStringLiteral("Start engine"));
        map.insert(QStringLiteral("⌂ Kezdőlap"), QStringLiteral("⌂ Home"));
        map.insert(QStringLiteral("❄ Fagyasztás"), QStringLiteral("❄ Freeze"));
        map.insert(QStringLiteral("🗂 Munkamenetek"), QStringLiteral("🗂 Sessions"));
        map.insert(QStringLiteral("⚙ Beállítások"), QStringLiteral("⚙ Settings"));
        map.insert(QStringLiteral("💾 Profil"), QStringLiteral("💾 Profile"));
        map.insert(QStringLiteral("🖨️ PDF"), QStringLiteral("🖨️ PDF"));
        map.insert(QStringLiteral("🌓 Oldal-téma: %1"), QStringLiteral("🌓 Page theme: %1"));
        // Settings dock.
        map.insert(QStringLiteral("Beállítások"), QStringLiteral("Settings"));
        map.insert(QStringLiteral("Élő kártyák (max):"), QStringLiteral("Live cards (max):"));
        map.insert(QStringLiteral("Hibernálás tétlenség után (perc):"),
                   QStringLiteral("Hibernate after idle (minutes):"));
        map.insert(QStringLiteral("Hideg kártya (óra):"), QStringLiteral("Cold card (hours):"));
        map.insert(QStringLiteral("Memória-büdzsé (MiB):"), QStringLiteral("Memory budget (MiB):"));
        map.insert(QStringLiteral("Erőforrás-profil:"), QStringLiteral("Resource profile:"));
        map.insert(QStringLiteral("Nyelv:"), QStringLiteral("Language:"));
        // Tray.
        map.insert(QStringLiteral("Antikolt — fut"), QStringLiteral("Antikolt — running"));
        map.insert(QStringLiteral("Megnyitás"), QStringLiteral("Open"));
        map.insert(QStringLiteral("Kilépés"), QStringLiteral("Quit"));
        // Find bar.
        map.insert(QStringLiteral("Keresés az oldalon"), QStringLiteral("Find in page"));
        map.insert(QStringLiteral("Keresés az oldalon…"), QStringLiteral("Find in page…"));
        return map;
    }();
    return en.value(hu, hu);
}

}  // namespace antikolt
