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
        map.insert(QStringLiteral("Gyors"), QStringLiteral("Fast"));
        map.insert(QStringLiteral("Kiegyensúlyozott"), QStringLiteral("Balanced"));
        map.insert(QStringLiteral("Takarékos"), QStringLiteral("Saver"));
        map.insert(QStringLiteral("Ultra"), QStringLiteral("Ultra"));
        map.insert(QStringLiteral("Automatikus"), QStringLiteral("Auto"));
        map.insert(QStringLiteral("Kézi"), QStringLiteral("Manual"));
        // Tray.
        map.insert(QStringLiteral("Antikolt — fut"), QStringLiteral("Antikolt — running"));
        map.insert(QStringLiteral("Megnyitás"), QStringLiteral("Open"));
        map.insert(QStringLiteral("Kilépés"), QStringLiteral("Quit"));
        // Find bar.
        map.insert(QStringLiteral("Keresés az oldalon"), QStringLiteral("Find in page"));
        map.insert(QStringLiteral("Keresés az oldalon…"), QStringLiteral("Find in page…"));
        // Status bar (the `setStatus` sources). Format placeholders are preserved verbatim.
        map.insert(QStringLiteral("kártya felület: %1 · %2"), QStringLiteral("card surface: %1 · %2"));
        map.insert(QStringLiteral("kártya #%1 lemezre mentve · memória felszabadítva"),
                   QStringLiteral("card #%1 saved to disk · memory freed"));
        map.insert(QStringLiteral("motor: nincs elindítva · kártyák: 0"),
                   QStringLiteral("engine: not started · cards: 0"));
        map.insert(QStringLiteral("a tálcán fut · megnyitás: a tálcaikon"),
                   QStringLiteral("running in the tray · open: the tray icon"));
        map.insert(QStringLiteral("előhozva a tálcáról"), QStringLiteral("restored from the tray"));
        map.insert(QStringLiteral("kitűzött kártya — a hibernálás kimarad"),
                   QStringLiteral("pinned card — hibernation skipped"));
        map.insert(QStringLiteral("bejelentkezés folyamatban — a hibernálás vár"),
                   QStringLiteral("sign-in in progress — hibernation waits"));
        map.insert(QStringLiteral("a kártya médiát játszik — a hibernálás vár"),
                   QStringLiteral("the card is playing media — hibernation waits"));
        map.insert(QStringLiteral("tétlenség: a kártya lemezre megy"),
                   QStringLiteral("idle: the card goes to disk"));
        map.insert(QStringLiteral("memória-nyomás: a kemény sáv fölött"),
                   QStringLiteral("memory pressure: above the hard band"));
        map.insert(QStringLiteral("memória-nyomás: a figyelmeztető sáv fölött — a preview szünetel"),
                   QStringLiteral("memory pressure: above the warn band — preview paused"));
        map.insert(QStringLiteral("motor hiba: %1"), QStringLiteral("engine error: %1"));
        map.insert(QStringLiteral("az oldal összeomlott (#%1) — újratöltés (állapot %2, kód %3)"),
                   QStringLiteral("the page crashed (#%1) — reloading (status %2, code %3)"));
        map.insert(QStringLiteral("motor: %1 %2 · képességek: %3"),
                   QStringLiteral("engine: %1 %2 · capabilities: %3"));
        map.insert(QStringLiteral("betöltve (#%1, %2): %3"), QStringLiteral("loaded (#%1, %2): %3"));
        map.insert(QStringLiteral("takarítás hiba (%1): %2"), QStringLiteral("cleanup error (%1): %2"));
        map.insert(QStringLiteral("betöltési hiba (#%1): %2 — %3"),
                   QStringLiteral("load error (#%1): %2 — %3"));
        map.insert(QStringLiteral("bejelentkezési ablak nyitva: %1"),
                   QStringLiteral("sign-in window open: %1"));
        map.insert(QStringLiteral("elutasított felugró ablak: %1"),
                   QStringLiteral("popup refused: %1"));
        map.insert(QStringLiteral("a bejelentkezési ablak bezárult"),
                   QStringLiteral("the sign-in window closed"));
        map.insert(QStringLiteral("konténer-motor indítva: %1"),
                   QStringLiteral("container engine started: %1"));
        map.insert(QStringLiteral("nem találom a motorházat (állítsd be az ANTIKOLT_ENGINE változót)"),
                   QStringLiteral("cannot find the engine host (set ANTIKOLT_ENGINE)"));
        map.insert(QStringLiteral("motor indítása: %1"), QStringLiteral("starting engine: %1"));
        map.insert(QStringLiteral("mag hiba: %1"), QStringLiteral("core error: %1"));
        map.insert(QStringLiteral("mag: %1 %2"), QStringLiteral("core: %1 %2"));
        map.insert(QStringLiteral("a mag nem található (ANTIKOLT_CORE) — a rács egyelőre mintaadatot mutat"),
                   QStringLiteral("core not found (ANTIKOLT_CORE) — the grid shows sample data for now"));
        map.insert(QStringLiteral("az előző kártya mentése nem sikerült: %1"),
                   QStringLiteral("saving the previous card failed: %1"));
        map.insert(QStringLiteral("a kártya nem tudott felületet kérni a motortól"),
                   QStringLiteral("the card could not ask the engine for a surface"));
        map.insert(QStringLiteral("a kártyának nincs címe"), QStringLiteral("the card has no title"));
        map.insert(QStringLiteral("a motor még nem fut — nyomd meg a „Motor indítása” gombot"),
                   QStringLiteral("the engine is not running — press the “Start engine” button"));
        map.insert(QStringLiteral("előző kártya mentve (%1 bájt) · váltás: %2"),
                   QStringLiteral("previous card saved (%1 bytes) · switching: %2"));
        map.insert(QStringLiteral("a lap kérése nem kapott választ időben (#%1)"),
                   QStringLiteral("the page request timed out (#%1)"));
        map.insert(QStringLiteral("a kezdőlap cím nélküli kártyát kért megnyitni (#%1)"),
                   QStringLiteral("the home page asked to open an untitled card (#%1)"));
        map.insert(QStringLiteral("ismeretlen híd-hívás: %1"), QStringLiteral("unknown bridge call: %1"));
        map.insert(QStringLiteral("a kártyalista nem érkezett meg: %1 — a rács a mintákat mutatja"),
                   QStringLiteral("the card list did not arrive: %1 — the grid shows the samples"));
        map.insert(QStringLiteral("a kártyalista nem ment át a lapra: %1"),
                   QStringLiteral("the card list did not reach the page: %1"));
        map.insert(QStringLiteral("kártyalista a magtól: %1 kártya"),
                   QStringLiteral("card list from the core: %1 cards"));
        map.insert(QStringLiteral("a konténer-tábla nem érkezett meg: %1"),
                   QStringLiteral("the container table did not arrive: %1"));
        map.insert(QStringLiteral("nincs mit fagyasztani"), QStringLiteral("nothing to freeze"));
        map.insert(QStringLiteral("fagyasztás hiba: %1"), QStringLiteral("freeze error: %1"));
        map.insert(QStringLiteral("pillanatkép: %1 bájt · állapot %2 bájt · motor leállítása"),
                   QStringLiteral("snapshot: %1 bytes · state %2 bytes · stopping engine"));
        map.insert(QStringLiteral("a mag nem fut, a szabályok nem menthetők"),
                   QStringLiteral("the core is not running, the rules cannot be saved"));
        map.insert(QStringLiteral("a szabályok nem mentődtek: %1"),
                   QStringLiteral("the rules did not save: %1"));
        map.insert(QStringLiteral("munkamenet-szabályok: %1 óra%2"),
                   QStringLiteral("session rules: %1 hour%2"));
        map.insert(QStringLiteral("nagyítás: %1% (nincs élő felület)"),
                   QStringLiteral("zoom: %1% (no live surface)"));
        map.insert(QStringLiteral("oldal-téma: %1 (nincs élő felület)"),
                   QStringLiteral("page theme: %1 (no live surface)"));
        map.insert(QStringLiteral("sötét oldalak: %1 (nincs élő felület)"),
                   QStringLiteral("dark pages: %1 (no live surface)"));
        map.insert(QStringLiteral("az előnézet neve nem mentődött: %1"),
                   QStringLiteral("the preview name did not save: %1"));
        map.insert(QStringLiteral("előnézet kész: %1"), QStringLiteral("preview ready: %1"));
        map.insert(QStringLiteral("az előnézet nem olvasható: %1"),
                   QStringLiteral("the preview cannot be read: %1"));
        map.insert(QStringLiteral("az előnézet nem került a profilba: %1"),
                   QStringLiteral("the preview did not reach the profile: %1"));
        map.insert(QStringLiteral("háttér-előnézet kész: %1"),
                   QStringLiteral("background preview ready: %1"));
        map.insert(QStringLiteral("háttér-előnézet kimaradt: %1"),
                   QStringLiteral("background preview skipped: %1"));
        map.insert(QStringLiteral("%1 árva előnézet törölve"),
                   QStringLiteral("%1 orphan previews removed"));
        map.insert(QStringLiteral("a munkamenet-naplóhoz előbb indítsd el a magot"),
                   QStringLiteral("start the core first for the session ledger"));
        map.insert(QStringLiteral("a munkamenet-napló nem érkezett meg: %1"),
                   QStringLiteral("the session ledger did not arrive: %1"));
        map.insert(QStringLiteral("munkamenet-napló: %1 sor, ebből %2 lejárt"),
                   QStringLiteral("session ledger: %1 rows, %2 expired"));
        map.insert(QStringLiteral("a takarításhoz előbb indítsd el a magot"),
                   QStringLiteral("start the core first for cleanup"));
        map.insert(QStringLiteral("a takarítási terv nem érkezett meg: %1"),
                   QStringLiteral("the cleanup plan did not arrive: %1"));
        map.insert(QStringLiteral("takarítás: %1 azonnal, %2 a kártya következő megnyitásakor"),
                   QStringLiteral("cleanup: %1 now, %2 when the card next opens"));
        map.insert(QStringLiteral("takarítás: a kártya médiát játszik — az élő munkamenet megmarad"),
                   QStringLiteral("cleanup: the card is playing media — the live session stays"));
        map.insert(QStringLiteral("a mag nem fut — nincs mit menteni"),
                   QStringLiteral("the core is not running — nothing to save"));
        map.insert(QStringLiteral("profil mentése nem sikerült: %1"),
                   QStringLiteral("saving the profile failed: %1"));
        map.insert(QStringLiteral("profil mentve: %1 (%2 kártya, %3 konténer)"),
                   QStringLiteral("profile saved: %1 (%2 cards, %3 containers)"));
        map.insert(QStringLiteral("nincs élő kártya — nincs mit menteni"),
                   QStringLiteral("no live card — nothing to save"));
        map.insert(QStringLiteral("a PDF mentése nem sikerült: %1"),
                   QStringLiteral("saving the PDF failed: %1"));
        map.insert(QStringLiteral("PDF mentve: %1"), QStringLiteral("PDF saved: %1"));
        map.insert(QStringLiteral("a mag nem fut — nincs hová betölteni"),
                   QStringLiteral("the core is not running — nowhere to load into"));
        map.insert(QStringLiteral("profil betöltése nem sikerült: %1"),
                   QStringLiteral("loading the profile failed: %1"));
        map.insert(QStringLiteral("profil betöltve: %1 (%2 kártya, %3 konténer)"),
                   QStringLiteral("profile loaded: %1 (%2 cards, %3 containers)"));
        map.insert(QStringLiteral("a mag nem fut, a beállítások nem menthetők"),
                   QStringLiteral("the core is not running, the settings cannot be saved"));
        map.insert(QStringLiteral("a beállítások nem mentődtek: %1"),
                   QStringLiteral("the settings did not save: %1"));
        map.insert(QStringLiteral("beállítások mentve"), QStringLiteral("settings saved"));
        map.insert(QStringLiteral("az egyéni kezdőlap visszaállítva"),
                   QStringLiteral("the custom home page restored"));
        map.insert(QStringLiteral("nincs mit raszterizálni"), QStringLiteral("nothing to rasterise"));
        map.insert(QStringLiteral("a rács raszterizálása nem sikerült: %1"),
                   QStringLiteral("rasterising the grid failed: %1"));
        map.insert(QStringLiteral("a raszter nem került a profilba: %1"),
                   QStringLiteral("the raster did not reach the profile: %1"));
        map.insert(QStringLiteral("raszter kész: %1"), QStringLiteral("raster ready: %1"));
        map.insert(QStringLiteral("raszter-kezdőlap (a motor leállítva)"),
                   QStringLiteral("raster home (engine stopped)"));
        // Bookmark import (M7).
        map.insert(QStringLiteral("Könyvjelzők importálása…"), QStringLiteral("Import bookmarks…"));
        map.insert(QStringLiteral("a mag nem fut, az import nem mehet"),
                   QStringLiteral("the core is not running, import cannot proceed"));
        map.insert(QStringLiteral("a könyvjelző-fájl nem olvasható: %1"),
                   QStringLiteral("the bookmarks file cannot be read: %1"));
        map.insert(QStringLiteral("nincs könyvjelző a fájlban"), QStringLiteral("no bookmarks in the file"));
        map.insert(QStringLiteral("könyvjelzők importálva: %1 (kihagyva %2)"),
                   QStringLiteral("bookmarks imported: %1 (skipped %2)"));
        // Session ledger dock + policy editor.
        map.insert(QStringLiteral("kitakarva: %1"), QStringLiteral("cleaned up: %1"));
        map.insert(QStringLiteral("kitakarva, de a napló nem (%1)"),
                   QStringLiteral("cleaned up, but the ledger did not (%1)"));
        map.insert(QStringLiteral("munkamenet-politika mentve"), QStringLiteral("session policy saved"));
        map.insert(QStringLiteral("munkamenet-politika hiba: %1"),
                   QStringLiteral("session policy error: %1"));
        map.insert(QStringLiteral("a munkaterület-tábla nem érkezett meg: %1"),
                   QStringLiteral("the workspace table did not arrive: %1"));
        map.insert(QStringLiteral("Takarítás most"), QStringLiteral("Clean up now"));
        map.insert(QStringLiteral("Állapot"), QStringLiteral("Status"));
        map.insert(QStringLiteral("Politika"), QStringLiteral("Policy"));
        map.insert(QStringLiteral("Sütik"), QStringLiteral("Cookies"));
        map.insert(QStringLiteral("Lejárt"), QStringLiteral("Expired"));
        map.insert(QStringLiteral("Törlésnél"), QStringLiteral("On wipe"));
        map.insert(QStringLiteral("Lejárat (óra):"), QStringLiteral("Expiry (hours):"));
        map.insert(QStringLiteral("App-zárnál törlés"), QStringLiteral("Wipe on app close"));
        map.insert(QStringLiteral("lejárt"), QStringLiteral("expired"));
        map.insert(QStringLiteral("%1 süti · %2 kB"), QStringLiteral("%1 cookies · %2 kB"));
        map.insert(QStringLiteral(" · belépés"), QStringLiteral(" · sign-in"));
        // Dialogs, buttons and tooltips.
        map.insert(QStringLiteral("Mentés"), QStringLiteral("Save"));
        map.insert(QStringLiteral("Profil mentése"), QStringLiteral("Save profile"));
        map.insert(QStringLiteral("Oldal mentése PDF-be"), QStringLiteral("Save page as PDF"));
        map.insert(QStringLiteral("Profil betöltése"), QStringLiteral("Load profile"));
        map.insert(QStringLiteral("Egyéni kezdőlap szerkesztése"), QStringLiteral("Edit custom home page"));
        map.insert(QStringLiteral("Egyéni kezdőlap visszaállítása"),
                   QStringLiteral("Reset custom home page"));
        map.insert(QStringLiteral("Létrehozza a kiinduló home.html-t (ha még nincs), és megnyitja a szerkesztőben."),
                   QStringLiteral("Creates the starting home.html (if missing) and opens it in the editor."));
        map.insert(QStringLiteral("Törli a profil home.html-jét, és visszatölti a beépített kártyarácskot."),
                   QStringLiteral("Deletes the profile's home.html and restores the built-in card grid."));
        map.insert(QStringLiteral("A profil egyetlen fájlba mentése vagy onnan betöltése (ADR-059)."),
                   QStringLiteral("Save the profile to a single file or load it from one (ADR-059)."));
        map.insert(QStringLiteral("Mentés másként…"), QStringLiteral("Save as…"));
        map.insert(QStringLiteral("Betöltés…"), QStringLiteral("Load…"));
        map.insert(QStringLiteral("Könyvjelzők importálása"), QStringLiteral("Import bookmarks"));
        map.insert(QStringLiteral("HTML fájlok (*.html *.htm)"),
                   QStringLiteral("HTML files (*.html *.htm)"));
        map.insert(QStringLiteral("Az élő oldal mentése PDF-be (F7)."),
                   QStringLiteral("Save the live page as PDF (F7)."));
        map.insert(QStringLiteral("PDF mentése"), QStringLiteral("Save PDF"));
        map.insert(QStringLiteral("Oldal mentése PDF-be…"), QStringLiteral("Save page as PDF…"));
        map.insert(QStringLiteral("Oldalak sötétítése: %1\nA weboldal saját sötét témája nélkül is sötét lesz."),
                   QStringLiteral("Darken pages: %1\nDark even without the website's own dark theme."));
        map.insert(QStringLiteral("Oldalak sötétítése"), QStringLiteral("Darken pages"));
        map.insert(QStringLiteral("Találatok száma"), QStringLiteral("Number of matches"));
        map.insert(QStringLiteral("Keresés bezárása"), QStringLiteral("Close find"));
        map.insert(QStringLiteral("Fő"), QStringLiteral("Main"));
        map.insert(QStringLiteral("nincs találat"), QStringLiteral("no matches"));
        // Theme and page-dark labels.
        map.insert(QStringLiteral("Világos téma"), QStringLiteral("Light theme"));
        map.insert(QStringLiteral("Szürke téma (alap)"), QStringLiteral("Grey theme (default)"));
        map.insert(QStringLiteral("Sötét téma"), QStringLiteral("Dark theme"));
        map.insert(QStringLiteral("AMOLED fekete téma"), QStringLiteral("AMOLED black theme"));
        map.insert(QStringLiteral("Sötét (invertálás)"), QStringLiteral("Dark (inverted)"));
        map.insert(QStringLiteral("Sötét szürke"), QStringLiteral("Dark grey"));
        map.insert(QStringLiteral("Sötét meleg"), QStringLiteral("Dark warm"));
        map.insert(QStringLiteral("Kikapcsolva"), QStringLiteral("Off"));
        // Zoom / page-theme / dark-pages live and error variants (the no-live-surface forms already exist).
        map.insert(QStringLiteral("nagyítás: %1%"), QStringLiteral("zoom: %1%"));
        map.insert(QStringLiteral("nagyítás hiba: %1"), QStringLiteral("zoom error: %1"));
        map.insert(QStringLiteral("oldal-téma: %1"), QStringLiteral("page theme: %1"));
        map.insert(QStringLiteral("oldal-téma hiba: %1"), QStringLiteral("page theme error: %1"));
        map.insert(QStringLiteral("sötét oldalak: %1"), QStringLiteral("dark pages: %1"));
        map.insert(QStringLiteral("sötét oldalak hiba: %1"), QStringLiteral("dark pages error: %1"));
        map.insert(QStringLiteral(", app-zárnál törlés"), QStringLiteral(", wipe on app close"));
        map.insert(QStringLiteral("A böngésző a tálcán fut tovább. Megnyitás és kilépés: a tálcaikon menüje."),
                   QStringLiteral("The browser keeps running in the tray. Open and quit: the tray icon menu."));
        return map;
    }();
    return en.value(hu, hu);
}

}  // namespace antikolt
