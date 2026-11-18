# 16 — Fejlesztési napló

> Kronológiai napló arról, mi készült el, **hogyan lett ellenőrizve**, és mi blokkol. A terv a
> `00`–`15` dokumentumokban van; ez a napló a valóságot rögzíti.

### A panel is mutatja is (42. szelet)

A `session_list` válasz sorai most a mag által mért mennyiséget is hordozzák (`size`: sütik, bájt, service
workerek; `loses_login`), és a panel hatodik oszlopa ezt írja ki: `7 süti · 5.0 kB`, bejelentkezett sornál
`· belépés` toldattal. A felület nem számol semmit: a mag dönt, a shell csak megjelenít.

### Bizonyíték

```
--sessionpanel
panel_row=https://example.org | expired | sweep | 7 | lejárt | 7 süti · 5.0 kB
session_panel=ok
```
## 2026-11-17 (ötvenegyedik szelet) — ki kapjon háttér-előnézetet: a döntés

A háttér-előnézet (I1) két részre bontható: **melyik kártya** kapjon képet, és **mikor** szabad egyáltalán
fényképezni (egy élő felület, a felhasználó nem zavarása). Az első rész tiszta döntés, és kész:

`antikolt::nextPreviewCard(cards, previews_dir, now, max_age_ms)` — a sorrend, amit a felhasználó is így
mondana: **előbb a kép nélküli**, aztán az **elavult** (7 nap), a friss nem kap új képet; a `file:`/nem-http
kártyák kimaradnak (nincs mit lefényképezni). Azonos rangnál a régebben megnyitott kártya megy előre.

### Bizonyíték (tényleges kimenet)

```
antikolt-shell --previewqueue
queue_first=no-preview
queue_second=stale
queue_none=empty
preview_queue=ok        (exit 0)
```

A `--previewqueue` bekerült a `verify.ps1` kapujába.

### Ami még hátra van

- a **munkás** (második, ideiglenes felület a rácson kívül, egyidejűleg egy) — ez érinti az I1-et, ezért
  külön döntés/ADR kell hozzá, mielőtt kód lesz belőle;
- a háttér-indítás feltétele: ne legyen élő kártya, és a napi keret engedje (`previewBudgetAllows`).
## 2026-11-17 (ötvenedik szelet) — MSIX: a Store-ág csomagja

Az ADR-040 két csatornát rögzít; a Store-ághoz eddig csak a döntés volt meg, csomag nem. Elkészült a
`packaging\msix\AppxManifest.xml` (full-trust Win32 csomag, `internetClient` + `runFullTrust`, öt generált
csempélő- és splash-kép) és a `scripts\msix.ps1`, ami a `dist\Antikolt` mappát **változtatás nélkül**
becsomagolja, `makeappx`-szel, és a végén **visszafejtéssel ellenőrzi**, hogy a három folyamat, a lap és a CEF
futtatókörnyezet benne van.

### Bizonyíték (tényleges kimenet)

```
pwsh -File scripts/msix.ps1
  packed   : 200.8 MB
  payload  : complete
  msix=D:\Szerver\Antikolt\dist\Antikolt.msix size_mb=200.8 payload=ok
  exit 0
```

A csomag tömörítve 200.8 MB (a hordozható mappa 442 MB), a payload ellenőrzése: `antikolt-shell.exe`,
`antikolt-engine-host.exe`, `antikolt-core-service.exe`, `AppxManifest.xml`, `home\index.html`, `libcef.dll`,
`icudtl.dat` — mind megvan a visszafejtett csomagban.

### Ami még hátra van (Store-ág)

- **Identity a Partner Centertől**: az `Identity` (`Name`, `Publisher`, `Version`) most helyőrző; a Store a
  beküldéskor adja a valós értékeket, és a `Publisher`-nek egyeznie kell az aláíró tanúsítvánnyal.
- **Helyi telepítési teszt**: `Add-AppxPackage` csak akkor működik, ha a tanúsítvány megbízható a gépen —
  ebben a környezetben a tanúsítványtár írása tiltott (lásd `20-ALASAS.md`), tehát a telepítés **nincs
  igazolva**, csak a csomagolás és a payload.
- **Beküldés**: fiók + Store-ellenőrzés (emberi lépés).
## 2026-11-17 (negyvenkilencedik szelet) — hordozható build, parancsikon, Release, ikon, auto-indítás

Nyolc szelet egy blokkban (42–49), mert a napló-bejegyzések beszúrása csendben elmaradt — a
`Replace` horgonyai nem találtak (a napló fejlécei más sorszámozásúak voltak), és a "docs written" üzenet
ellenére a fájl nem változott. Ez a bejegyzés pótolja, a **tényekkel**, nem az eredeti szándékkal.

### 42. A panel mutatja, mennyi veszne el
`session_list` sorai hordozzák a mag által mért mennyiséget (`size`) és a `loses_login` jelzőt; a panel
hatodik oszlopa: `7 süti · 5.0 kB`, bejelentkezett sornál `· belépés`. Bizonyíték: `--sessionpanel`
`panel_row=… | 7 süti · 5.0 kB`, `session_panel=ok`.

### 43. A mentett jegyzet megjelenik (és a fókusz)
A kártya újrarenderelési **aláírásából hiányzott a jegyzet**, ezért a mentett jegyzet nem frissült a
képernyőn. Az aláírás most tartalmazza; a fókusz-visszaadás implementálva (render utáni, id-alapú). A fókusz
**mérése** nem lehetséges a teszt-ablak fókusz-hiánya miatt (`note_focus=none` / `document.activeElement`
sem létezik) — a próba állítás nélkül marad, ez a naplóban is szerepel.

### 44. Hordozható build + asztali parancsikon (`scripts/dist.ps1`)
`dist\Antikolt`: shell + motorház + mag + lap + Qt (`windeployqt`) + CEF futtatókörnyezet, és desktop
parancsikon. Két hiba csak itt derült ki: a CEF **erőforrásai külön mappában** vannak (`Resources\`:
`icudtl.dat`, `*.pak`, `locales\`), és a lap a hordozható mappában `appDir\home`. Bizonyíték a csomagból:
`grid_source=core`, `grid_count=6`, `grid_matches_core=ok`, `grid_click_open=ok` (exit 0), 443 MB.

### 45. Saját ikon
`assets/antikolt.ico` (négy méret, kódból generálva), `--selftest` → `icon=ok`, a parancsikon
`IconLocation`-ja erre mutat.

### 46. `--capturetest` lezárási összeomlás javítva
`0xC0000005` az eredmények kiírása **után**, 4 futásból 1-szer: a `destroy_surface` válasza egy megszűnő
klienshez érkezett. A teszt most megvárja a választ (max. 2 s). Mérés: **8/8 futás zöld**.

### 47. Release csomag (`dist.ps1 -Release`)
Külön `build-release\` (`-Config Release`), `cargo build --release`; a fejlesztői `build\` érintetlen.
Tanulság: a `cmake` nincs a héj PATH-ján — a script ezért a `build.ps1`-t hívja, nem duplikálja az
eszközláncot. A `check-repo` és a `.gitignore` most a `build-release\`-t is generált kimenetnek veszi
(ezért lett az SPDX-szám 55 → **52**, ami a helyes érték).

### 48. Dupla kattintás = böngésző
Sima indításnál a motor **magától** elindul (a „Motor indítása" gomb csak leállt motor esetén kell).
Bizonyíték a csomagból, kapcsolók nélkül: `protocol server ready`, `op=hello`, `op=install_bridge`.

### 49. Start menü + abszolút útvonalak
A `dist.ps1` Start menü bejegyzést is ír (ugyanaz a `.lnk`), és a célmappát teljes útvonalra normalizálja —
a parancsikon ikonja korábban `…\scripts\..\dist\…` volt.

### A kapu állapota

```
scripts/verify.ps1  →  27/27 önteszt, ALL SELF TESTS PASSED
cargo test          →  78 teszt
check-repo          →  ALL CHECKS PASSED (52 SPDX fájl)
```

### Ami még hátra van

- **Aláírás + telepítő** (SmartScreen az aláíratlan exe-nél; kód-aláíró tanúsítvány kell hozzá);
- funkcionális maradék: háttér-előnézetek (I1), memória-büdzsé (F6), kártyánkénti munkamenet-felülírás,
  a takarítás média-kivétele, Linux/Wayland (F7), széf/passkey;
- a fókusz és a rendered-jegyzet mérése (a helyes mérési mód a naplóban).

## 2026-11-17 (negyvenegyedik szelet) — mennyi veszne el: `SessionSize`

A `LossEstimate` már létezett, de az a **súrlódást** írja le (jelszó, TOTP, captcha) — nem az **mennyiséget**.
A panelhez mindkettő kell: „3 süti" és „újra be kell jelentkezned" két különböző rossz. Ezért a ledger sora
most `SessionSize`-ot ad (`size()`: sütik, bájt, service workerek) és `loses_login()`-t; minden szám a sor
saját mezőiből jön, semmi becslés.

Az első próbálkozás **duplikálta** a `LossEstimate`-et (ugyanaz a név, más mezők) — a fordító `E0428`-mal
állította meg, és ez helyes volt: egy típus, egy jelentés. A végleges név `SessionSize`, mert más kérdésre
válaszol.

### Bizonyíték (a tényleges kimenet)

```
cargo test -p antikolt-core
test session::tests::session_size_counts_what_a_wipe_removes ... ok
test result: ok. 78 passed; 0 failed
```

### Ami még nyitott

- A panel **kijelzése** (`LossEstimate` + `SessionSize` a sorokban) — a mag oldala kész, a huzal és a
  felület következik.
## 2026-11-17 (negyvenedik szelet) — a hely visszaadása, end to end (három hiba)

Az előző három kísérlet a mérésnél akadt el. Most a teszt **átment**, és közben **három valódi hibát**
talált — egyik sem teszt-hiba volt:

1. **A rács, ahova visszatérsz, halott volt.** `ensureSurface()` a meglévő felületet csak *navigálta* a
   home URL-re, de a hidat nem telepítette oda (azt csak a `surfaceAttached` tette, új felületnél). Így a
   kártyáról hazatérve a rács kártyái **nem voltak kattinthatók** — a felhasználó szemében „megállt a
   böngésző". A meglévő felület ágában most `installBridge` + `home_surface_` beállítás történik.
2. **A mentés versenyzett a navigációval.** `rememberScroll()` aszinkron `evaluate`-et indított, és a
   hívó azonnal tovább navigált: az olvasott érték a *következő* lap 0-ja lett. Most a hívó csak a mentés
   **után** folytatja (`rememberScroll(std::function<void()>)`), a `loadHome`/`openCard` láncolva.
3. **A rács nullája felülírta a kártyáét.** Hazatérés után a felület a rács, a `current_card_id_` viszont
   még a kártyáé volt: a következő mentés 0-t írt a 700 helyére (a naplóban `saving 700` majd `saving 0`).
   Most a mentés kimarad, ha az élő felület a rács, és `loadHome` törli a kártya-azonosítót.
4. **A visszaállítás abszolút és késleltetett.** `loadFinished` nem „layout kész", és egy lapra **kétszer**
   is tüzel: a relatív görgetés kétszer futott (`restored_scroll_y=1400`). Most `window.scrollTo(0, N)`,
   300 ms-mal a betöltés után.

### Bizonyíték (a tényleges kimenet)

```
antikolt-shell --scrollrestore <motor>
first_scroll_y=700
restored_scroll_y=700
scroll_restore=ok      (exit code 0)
pwsh -File scripts/verify.ps1 → 27 ellenőrzés, ALL SELF TESTS PASSED
```
## 2026-11-17 (harminckilencedik szelet) — a kártya helye: hol hagyta abba az olvasó

A kártyát újranyitva a lap tetején kezdtünk: egy hosszú cikkben ez nem „visszatérés", hanem elveszett hely.
Most a shell megjegyzi kártyánként a görgetési pozíciót a profil `scroll.json`-jában
(`previews.h` mintájára `scroll_book.h`: `loadScrollBook`, `scrollPositionFor`, `saveScrollPosition`), a kártya
elhagyásakor (`openCard`/`loadHome`) megkérdezi a motortól a `window.scrollY`-t, és visszatéréskor
visszaállítja.

**Miért nem a kártya-rekordon?** Mert ez minden látogatáskor változik, a rekord pedig a felhasználó adata —
nem írjuk át csak azért, mert valaki görgetett. A nulla pozíció **törli** a bejegyzést, tehát a könyv csak
olyan helyeket tart, amik jelentenek valamit.

### Bizonyíték (a tényleges kimenet)

```
antikolt-shell --scrollbook
scroll_before=0
scroll_a=640
scroll_b=120
scroll_a_after_top=0      ← a tetejére állás törli a bejegyzést
scroll_b_after_top=120    ← ...és nem viszi magával a másik kártyát
scroll_reloaded=120       ← lemezről újraolvasva is megvan
scroll_book=ok      (exit code 0)
```

A bekötést a meglévő kártyanyitó tesztek fedik (`--opentest`, `--scrollzoomtest`: a kártya továbbra is
megnyílik és görgethető a visszaállítás mellett); **önálló end-to-end teszt** a visszaállításra még nincs —
ez a következő szelet.

### Kísérlet az end-to-end tesztre — negatív eredménnyel

Megírtam a kétfázisú tesztet (nyit → görget 700-ra → haza → újranyit → olvas). Az első fázis **mérhetően
működik**: `first_scroll_y=700`. A második fázisban azonban a rács újratöltése után a lap DOM-jában nem
találta a kártyát (`clicked_phase1=no-card`), hiába vártam 2,5 másodpercet — vagyis a kártyalista
*ráfestése* a második visszatéréskor később érkezik, mint a teszt kattintana. A tesztet **nem** hagytam a
Kapuban: kivettem a kódból, mert egy bizonytalanul bukó ellenőrzés rosszabb, mint egy hiányzó. A tanulság a
következő szelet kiindulópontja: a rács-kártyalista betöltésére **eseményt** kell várni (a lap jelezze, hogy
megkapta a listát), nem időzítőre.

### Ami még nyitott

- End-to-end teszt a visszaállításra (a fenti negatív eredmény miatt: a rács „kártyalista megérkezett"
  jelzésére várva).
- Kártyánkénti munkamenet-felülírás, `LossEstimate`, memória-büdzsé (F6).
## 2026-11-17 (harmincnyolcadik szelet) — az előnézet-gyártás napi büdzséje tartós

A ≤100/nap korlát eddig futás-szintű számláló volt: újraindítás után nulláról kezdett, tehát nem korlát.
Most a büdzsé a profilban él (`previews/budget.json`: `{"day":"…","count":N}`), a nap az egész állapot — nincs
tárolt óra-aritmetika —, és egy másik napról származó számláló **nullának** olvasódik. A szabály a
`previews.h`-ban: `previewUsedToday`, `previewConsumeBudget`, `previewBudgetAllows`; a shell a felvétel előtt
kérdez, a sikeres mentés után számol. Egy elutasított előnézet nem hiba: a kártya megtartja, amije volt.

### Bizonyíték (a tényleges kimenet)

```
antikolt-shell --previewbudget
budget_start=0
budget_after_three=3
budget_after_five=5
budget_allows_at_three=true
budget_refuses_at_five=true
budget_other_day=0        ← a tegnapi számláló nem a mai
budget_fresh_allows=true
preview_budget=ok      (exit code 0)
```

### Ami még nyitott

- A **háttérben** készített előnézetek (I1): ma csak a felhasználó által megnyitott kártya kap képet.
- A 7 napos frissítési küszöb és a 100-as napi keret **nem** hangolható a felületről.
## 2026-11-17 (harminchetedik szelet) — az előnézet elavul, és akkor újra készül

Az előnézet eddig **egyszer** készült: a kártya képe soha nem frissült. Most a kora dönt: ha a fájl nem
létezik, vagy **7 napnál régebbi**, a következő megnyitás újra lefényképezi. A kor a fájl saját módosítási
ideje (`previews.h: previewNeedsRefresh`) — egy előnézet egy fénykép, tehát a „mikor készült" pontosan az,
amit a fájlrendszer már nyilvántart; egy második időbélyeg a kártya-rekordon csak újabb szinkronizálnivaló
lenne. A jövőbeli módosítási idő (elállított óra) **frissnek** számít, különben minden megnyitás újra
fényképezne.

### Bizonyíték (a tényleges kimenet)

```
antikolt-shell --previewrefresh
refresh_missing=true    ← nincs fájl: újra kell
refresh_fresh=false     ← friss: nem kell
refresh_stale=true      ← 30 nappal visszaállítva: újra kell
refresh_aged=true       ← a visszaállítás sikerült (a mérés előfeltétele)
refresh_future=true     ← jövőbeli időbélyeg: nem csinálunk belőle fényképezőgépet
preview_refresh=ok      (exit code 0)
```

### Hiba a mérésből, javítva

A teszt először a *visszaállítás után* kérdezte meg újra, hogy „friss-e", és a saját korábbi állításával
vetette össze — más kérdésre más válasz. A „friss" mérése most a visszaállítás **előtt** történik
(`fresh_ok`), és a jelző (`refresh_fresh`) is ezt tükrözi.

### Ami még nyitott

- A **tartós napi ≤100** korlát (a számláló továbbra is futás-szintű), és a háttérben készített előnézetek (I1).
- A 7 napos küszöb **nem** hangolható a felületről (szándékos: egyelőre egy helyen van).
## 2026-11-17 (harminchatodik szelet) — az árva előnézetek törlődnek

Az előnézet-kép a profilban él, és a kártya-rekord csak a nevét hordozza (ADR-035) — ha a kártya megszűnik, a
kép örökre ott maradna. Most a takarítási kör részeként fut egy **járőr**: a mag kártyalistájából összegyűjtjük
a hivatkozott neveket, és a `previews/` mappában minden más kép törlődik (`previews.h`, tiszta függvény).

### Bizonyíték (a tényleges kimenet)

```
antikolt-shell --previewclean
preview_removed=orphan.png,orphan.webp   ← két árva, mindkét kiterjesztéssel
preview_kept=ok                          ← a hivatkozott fájl marad
preview_orphan_gone=ok
preview_stranger_kept=ok                 ← a nem-előnézet fájlhoz nincs közünk
preview_missing_dir=ok                   ← hiányzó mappa nem hiba
preview_cleanup=ok      (exit code 0)
```

A döntés a fájlnév és a kiterjesztés alapján születik (a járőr soha nem nyitja meg a fájlt), ezért a teszt
`orphan.webp`-je csak *tartalom nélküli* fájl: WEBP-kódoló nélkül is mérhető a viselkedés.

### Ami még nyitott

- Az előnézet **frissítése** (ma egyszer készül), a tartós napi ≤100 korlát, és a háttérben készített
  előnézetek (I1) továbbra is hátra vannak.
## 2026-11-17 (harmincötödik szelet) — a munkamenet-szabályok állíthatók

A politika eddig csak megjeleníthető volt. Most a panelen beállítható: lejárat órában (1–720), „app-zárnál
törlés" jelölő, Mentés gomb; a huzalon az új `session_rules` op, a magban a `SessionStore::set_rules`, ami
**ellenőriz, mielőtt ír** (1 óra – 30 nap), és az elutasítás saját hiba-változatot kap, hogy a válasz azt
mondja, mi a baj. A döntés az ADR-039-ben.

### Bizonyíték (a tényleges kimenet)

```
antikolt-shell --rulestest <mag>
rules_ttl_ms=7200000        ← 2 óra mentve
rules_lock_wipe=true
rules_refused=refused       ← az 1000 ms-os TTL-t a mag elutasítja ("the TTL must be …")
rules_reread_ttl_ms=7200000 ← és a tárolt szabály maradt az első, nem az elutasított
session_rules=ok      (exit code 0)
```

### Hiba a mérésből, javítva

A `session_rules` először `SessionError::Rejected(LedgerError::UnknownState)`-et adott vissza TTL-hibára —
a válasz „unknown state" lett volna egy elírásra. Külön `SessionError::Rules` változat készült, saját
üzenettel.

### A szerkesztőmezők a mag állapotát mutatják

A panel most **feltölti** a mezőket a `session_list` válaszából (óra, app-zárnál törlés), és a konténer
alapértelmezését (`container_default`) változatlanul küldi vissza — a shellnek nincs véleménye róla, tehát
nem írhatja felül. Így egy elutasított szerkesztés soha nem látszik mentettnek: a panel a *politika nézete*,
nem a saját memóriája.

A `--sessionpanel` ezt méri: `panel_ttl_hours=2`, `panel_lock_wipe=true` (a teszt előbb beállítja a
szabályokat, hogy ne a korábbi futás maradékát olvassa — a megosztott tár itt is csapda volt).

### Ami még nyitott

- **Kártyánkénti** (origin-szintű) felülírás a konténer-szabály mellett.
- Az órás egység miatt nem egész órás lejárat nem adható meg (szándékos egyszerűsítés).
## 2026-11-17 (harmincnegyedik szelet) — görgetés és nagyítás: a szerződés beváltva

A `proto/README.md` régóta hirdette a `scroll` és a `set_zoom` opot, a motor `not_supported`-ot válaszolt.
Mindkettő elkészült, a döntésekkel együtt az ADR-038-ban: a `scroll` a **dokumentumot** görgeti
(`window.scrollBy`, determinisztikus, pointer nélkül), a `set_zoom` pedig **újratördeli** a lapot
(`Emulation.setDeviceMetricsOverride`), a méretarányt a képernyő saját DPR-jével **szorozva**.

### A shell oldala is kész: nagyítás-gomb és emlékezés

A toolbaron `−` / `🔍 100%` / `+` (25 %-os lépések, 25–500 % között), és a választás **emlékszik**: minden
felület-becsatolásnál és betöltésnél újra alkalmazzuk (mint az oldal-témát), mert a nagyítás a böngésző
beállítása, nem egy oldalé. A gombok ugyanazt a publikus `setZoomPercent()`-et hívják, amit a teszt is — egy
kódút, nem kettő.

### Bizonyíték (a tényleges kimenet)

```
antikolt-shell --scrollzoomtest <motor>
scroll_y=600
dpr_at_150=1.875      ← 1,25 (a kijelző saját DPR-je) × 1,5
dpr_at_100=1.25       ← a felülírás törlése a természetes értéket adja vissza
dpr_at_125=1.5625     ← a shell saját útja (toolbar-metódus): 1,25 × 1,25
scroll_zoom=ok      (exit code 0)
```

### Három hiba a mérésből, mindkettő javítva

1. **A megosztott tár megint.** A teszt a `127.0.0.1` előtagra kattintott, és a korábbi futások
   `preview-probe` kártyáját találta el, amelynek URL-je egy már halott portra mutatott
   (`ERR_CONNECTION_REFUSED`) — a napló `load_failed` sora mutatta meg. A két próba-teszt most **saját
   kártya-azonosító szerint** kattint (`scroll-probe`, illetve `preview-probe`).
2. **Az abszolút DPR csapdája.** A nagyítás eredetileg `deviceScaleFactor = zoom`-ot állított, ami HiDPI-n
   (ez a gép 125 %-os) *kicsinyít*: 150 % helyett 1,5 DPR-t adott a természetes 1,875 helyett. A motor most
   előbb megméri a lap DPR-jét, és szoroz — a teszt pedig a **hányadost** állítja (1,875/1,25 = 1,5), ami a
   kijelző felbontásától független.
3. Egy `#include` sor a preprocessor-direktíva végére csúszott (`#include <functional>#include <QGuiApplication>`)
   — a C4067 figyelmeztetés fogta meg, `#include` nélkül a build elhalt volna.

### Ami még nyitott

- A `scroll` a dokumentumot görgeti, nem a fókuszban lévő belső konténert (gesztus-alapú görgetés = külön
  szelet).
- A shell még nem használja ezt az op-párt: nincs nagyítás-gomb és nincs scroll-pozíció visszaállítás.
- A nagyítás felülírja a viewportot, tehát a lap `resize`-ot kap; a shell nem írhatja vissza `set_bounds`-szal.
## 2026-11-17 (harmincharmadik szelet) — az oldal-téma: sötét oldal, ahol az oldal tudja

Az F7 (oldal-témák) első, becsületes szelete: a böngésző **jelzi** a lapnak a `prefers-color-scheme`-et,
nem kényszerít színeket. A motor új `set_media` opja `Emulation.setEmulatedMedia`-val állítja (`auto` = nincs
felülírás, `dark`, `light`), a shell toolbar-gombja körbeforgatja (auto → dark → light), és **emlékszik** a
választásra: minden felület-becsatolásnál és betöltésnél újra alkalmazza, mert az emuláció csak az épp
látszó lapra érvényes. A döntés az ADR-037-ben.

### Bizonyíték (a tényleges kimenet)

```
antikolt-shell --mediatest <motor>
media_dark=dark       ← a lap matchMedia-je igazat mond
media_light=light     ← ...és vissza is vált
site_theme=ok      (exit code 0)
```

A tanú a lap saját `matchMedia('(prefers-color-scheme: dark)').matches` értéke — a shell semmit nem
állíthat anélkül, hogy a lap ne látná. Mindkét irányt mérjük, mert egy lap, amely mindig sötétet jelent,
nem követi a böngészőt, hanem egyszerűen sötét.

### Hiba a mérésből, javítva

A teszt először **hozzáférési hibával** (0xC0000005) állt le, mert a `loadFinished` kezelőn belül definiált
`ask` lambdát referenciaként kapták el a később futó időzítők: a kezelő stackje már nem élt, amikor az
időzítő tüzelt. A lambda most a kezelőn **kívül** él, és **érték szerint** másolódik az időzítőkbe (a
`surface` azonosítót paraméterként kapja, nem referenciaként). Tanulság a naplónak: a Qt időzítő + C++ lambda
kombinációja csendben függő referenciát csinál, ha a lambdát egy rövid életű scope-ban deklaráljuk.

### Egy második, lassabban megmutatkozó hiba: a `capture` elakadhat

A teljes `verify.ps1` futásban a `--previewgentest` **néha** elbukott, önállóan viszont mindig átment. A
nyomkövetés megmutatta: a motor **soha nem válaszolt** a `capture`-re, a shell a saját 20 s-os
kérés-timeoutjáig várt, a teszt pedig 10 s után továbblépett — és a végén a `shutdown` után érkező hiba
(`a motorház kilépett (kód 0)`) leplezte el a valódi okot. A naplóban a GPU-folyamat
`ContextResult::kFatalFailure` hurka látszik ugyanabban a másodpercben, tehát a `Page.captureScreenshot`
elakadhat, amíg a GPU-folyamat újraindul.

Két javítás: (1) a shell **egyszer újrapróbálja** az elakadt felvételt (`kPreviewAttempts = 2`, 1,5 s
szünettel) — egy GPU-hiba ne vegyen el egy előnézetet; (2) a teszt most a shell timeoutjánál is **tovább
vár** (60 × 400 ms) és kiírja a várakozás hosszát (`preview_wait_ticks`). Öt egymás utáni futás: mind
`preview_generation=ok`.

### Ami még nyitott

- **Oldalankénti kivétel** („ez az oldal maradjon világos”) — ma a beállítás globális.
- A `prefers-color-scheme`-et nem ismerő oldalak **rákényszerített** sötétítése: szándékosan nincs (a kép,
  a márka és a kontraszt sérülne); ha kell, külön ADR-rel.
- A `scroll` és a `set_zoom` továbbra is `not_supported`.

## 2026-11-17 (harminckettedik szelet) — a memória-szám valódi: a motor méri, a lap látja

A termék ígérete a memóriatakarékos böngésző, de a felület semmit nem mutatott: a lap `system.memory`
hívása a mockból nullát kapott. Most a motor `metrics` opja mér — a lap saját DOM-csomópontjait és
JS-halmát (`Runtime.evaluate`), valamint a saját munkakészletét (`GetProcessMemoryInfo`) —, a shell pedig a
lap **kérésére** ezt válaszolja meg a kétirányú csatornán. A büdzsé szándékosan 0, amíg a profilok nem
hordoznak memória-büdzsét (F6): a lap ilyenkor csak a felhasznált értéket írja ki, nem talál ki büdzsét.
A döntés az ADR-036-ban.

### Bizonyíték (a tényleges kimenet)

```
antikolt-shell --metricstest <motor>
asked=ok
dom_nodes=57
js_heap_bytes=10000000
engine_working_set_bytes=117497856
page_used_bytes=103763968
metrics=ok      (exit code 0)
```

Két tanú: a motor `metrics` válasza (DOM-csomópontok, munkakészlet) és a lap `system.memory` válasza, ami a
hídon jött vissza. Megjegyzés a számokhoz: a **JS-halom kvantált** — a Chromium adatvédelmi okból kerekíti
(`10 000 000` pontosan), tehát indikátor, nem könyvelési adat; a munkakészlet és a lap válasza időben
kicsit eltér (117 MB a motor folyamata, 103 MB a kérés pillanatában), mert a working set mozog.

### Ami még nyitott

- A **büdzsé** (F6) és a kártyánkénti memória-bontás: a `metrics` ma az élő felületet méri, nem kártyánként.
- A `metrics` a **home felületre** is válaszol (57 csomópont) — a kártyák mérése ugyanaz az út, de a
  státuszsor és a napló még nem használja kártyánként.

## 2026-11-17 (harmincegyedik szelet) — az előnézet elkészül: a kártya megnyitása képet ad neki

A harmincadik szelet kiszolgálta az előnézetet; most **elő is állítja** a shell. A kártya rekordja additív
`preview_ref` mezőt kapott (a régi tár továbbra is parse-olható), a rács adaptálja
(`antikolt://blob/<név>`), és amikor a felhasználó megnyit egy kártyát, a shell a **valódi oldalról**
készít képet (`capture`), átmozgatja a profil `previews/` mappájába, és a nevet visszaírja a rekordba.
Egy kártya előnézete egyszer készül el (a meglévőt nem írja felül), és egy futás legfeljebb 40-et gyárt.

### Bizonyíték (a tényleges kimenet)

```
antikolt-shell --previewgentest <motor>
card_written=ok
card_opened=ok
preview_file=ok
preview_ref=preview-probe.png
preview_page=loaded:1560x857
preview_generation=ok      (exit code 0)
```

### Két hiba a mérésből, mindkettő javítva

1. **Elavult mag-bináris.** A shell a `preview_ref`-et írta, a mag `card_upsert`-je viszont a régi
   binárison futott (a `preview_ref`-et ismeretlen mezőként eldobta) — a válasz sikeres volt, a tár nem
   tartalmazta. Ez a **tooling** hibája volt: sem a `build.ps1`, sem a `verify.ps1` nem építette a Rust
   magot. Most a `verify.ps1` `cargo build -p antikolt-core-service`-szel kezdi a magot, és ezt külön
   ellenőrzésként is jelzi.
2. A `Card` három inicializálója (a vetett kártyák) nem ismerte az új mezőt — a `cargo build` ezt azonnal
   megfogta, és a mező most mindháromban `String::new()`.

### Ami még nyitott

- Az előnézet **frissítése**: ma egyszer készül; egy elavult kép nem cserélődik le.
- A **napi ≤100** korlát: a számláló futás-szintű (memóriában), a tartós büdzséhez a magnak kellene
  nyilvántartást vezetnie.
- A preview-fájlok **takarítása** (a 24 órás politika nem érinti őket), és a háttérben (a felhasználó
  kártyája nélkül) készített előnézetek — ez az I1 miatt külön szelet.

## 2026-11-17 (harmincadik szelet) — az előnézet-út: `antikolt://blob/` a motorból

A lap már régóta hivatkozik `antikolt://blob/<ref>`-re, de senki nem szolgálta ki. Most a motor
regisztrálja az `antikolt` sémát (`STANDARD | SECURE | CORS_ENABLED | FETCH_ENABLED`) és a
`blob/<név>` kérést a `<user data>/previews/<név>` fájlból válaszolja meg; a nevet a **könyvtáron belül**
oldja fel (üres név, `..`, elválasztó → elutasítás). A döntés az ADR-035-ben, a huzal-oldali leírás a
`proto/README.md`-ben.

### Bizonyíték (a tényleges kimenet)

```
antikolt-shell --previewtest <motor>
preview_file=probe.png 4x3 png
asked=ok
preview_load=loaded:4x3        ← a lap saját tanúja: naturalWidth×naturalHeight
preview_traversal=refused      ← `antikolt://blob/..%2Fhost.cpp` nem tölthető be
preview_delivery=ok      (exit code 0)
```

Két hiba a fordítás közben, mindkettő javítva: (1) a `cef_stream_resource_handler.h` include-ja a
`namespace antikolt` **belére** került, ezért a linker `antikolt::CefStreamResourceHandler`-t keresett
(`LNK2019`) — az include a fájl elejére került; (2) a `DISALLOW_COPY_AND_ASSIGN` privát másoló
konstruktort deklarál, ami elnyeli az implicit default konstruktort (`C2512`) — explicit
`PreviewHandlerFactory() = default;` kellett.

### Ami még nyitott

- Az **előállítás**: a shell még nem generál előnézetet. Kell hozzá: a kártya háttérben megnyitása (I1!),
  `capture`, mentés a profil `previews/` mappájába, és egy additív `preview_ref` mező a kártya-rekordon,
  hogy a rács hivatkozhasson rá. Ütemező (egy kártya egyszerre, ~8 s, napi ≤100) szintén hátra van.
- A preview-fájlok **takarítása** (a 24 órás politika nem érinti őket).

## 2026-11-17 (huszonkilencedik szelet) — minden kérésre pontosan egy válasz (watchdog)

A protokoll v2 szerződése kimondta, hogy a lap minden kérdésére **pontosan egy** válasz jön — de eddig ezt
csak a kód útjai tartották be, nem volt semmi, ami kikényszerítse. Most a shell a kérés beérkezésekor
felveszi a kérést egy halmazba, és **5 másodperces watchdoggot** indít; a `replyToPage` a halmazból
eltávolítással biztosítja, hogy egy kérésre ne menjen ki két válasz (a watchdog és egy késő út sem
duplázhat). A watchdog maga is ezen az úton válaszol, tehát a „pontosan egy” szabály egy helyen él.

A méréshez egy szándékosan meg nem válaszolt metódus kellett (`test.hang`), ami **csak**
`ANTIKOLT_BRIDGE_TEST=1` mellett érhető el — így egy normál futásban egy lap sem akaszthatja meg a
shellt, és a teszt mégis a valódi watchdog-úton megy keresztül.

### Bizonyíték (a tényleges kimenet)

```
antikolt-shell --timeouttest <motor>
asked=ok
answer_ms=6540            ← a válasz a kb. 5 s-os határidő után érkezett (a mérés 6,5 s-nál olvas)
timeout_answer=rejected
timeout_message=a shell nem válaszolt időben
timeout_test=ok      (exit code 0)
```

## 2026-11-17 (huszonnyolcadik szelet) — a jegyzet a kártyán írható

A harmadik írási út ugyanazon a csatornán: a kártyán a **jegyzet-szerkesztő** a kártya belsejében
nyílik (nem dialógusban), és a „Mentés” a `cards.upsert`-tel a maghoz megy. A szerkesztő **helyben**
nyílik, nem újrarendereléssel — egy re-render eldobná a beírt szöveget. A billentyűzet- és
egér-események `stopPropagation`-nal állnak meg a textarea határán, különben a rács nyílbillentyűs
navigációja és a kártya „kattintásra megnyílik” viselkedése beszűrődne gépelés közben (a kettő közül az
egyik biztos hibát okozott volna).

| Terület | Tartalom |
|---|---|
| Kártya | `.note` gomb + `.note-editor` (textarea, Mentés, Mégse) a shadow rootban; a jegyzet szövege a body-ban |
| Lap | `HomePage.setNote(id, notes)` → `cards.upsert`, majd újrarenderelés **a mag válaszából** |
| Tanú | `window.antikoltNoteState()`: a mentés, a modell és a hiba egy helyen |
| Önellenőrzés | `--notetest`: a szerkesztő kinyílik, a szöveg beíródik, a Mentés megjárja a hidat, a shellt és a magot |

### Bizonyíték (a tényleges kimenet)

```
antikolt-shell --notetest <motor>
note_save=ok
note_id=01SEED000000000000000001
note_page=probe note
note_model=probe note
note_core=probe note
note_error=
note_write=ok      (exit code 0)
```

Három független tanú egyezik: a szerkesztő válasza, a rács modellje és egy **friss** mag-kliens rekordja.

### Ami még nyitott

- A jegyzet **törlése** a Mentéssel üres szövegként megy (nincs külön „törlés” gomb).
- A jegyzet szerkesztése közben a rács nem görgethető billentyűzettel (a fókusz a textareaé) — ez
  szándékos, de a fókusz visszaadása a kártyának még nincs megoldva.

## 2026-11-17 (huszonhetedik szelet) — a rács ír: a pin-jelvény a magig ér

A huszonhatodik szelet átadta a csatornát, de a rács még nem használta. Most a kártya **pin-jelvénye
kattintható**: a jelvény egy `button` a kártya shadow rootjában, `stopPropagation`-nal (különben a
kattintás meg is nyitná a kártyát), és egy `antikolt-pin` composed eseménnyel jut el a laphoz, ami a
`cards.upsert`-tel a maghoz adja. A rács **csak a mag válasza után** renderel újra — így egy elbukott
írás nem hagyja a rácsot olyat állítani, amit a tár nem tartalmaz.

A lap oldalán két buktatót kellett kezelni: (1) a `withNativeOpen` kompozíció eddig csak az `open`-t
fordította át, ezért `cards.upsert` nem létezett (`a híd nem tud írni (nincs motor)`); (2) a modell
nézet, a mag rekordja viszont **más alak** (a modellben `container` objektum, a rekordban string) — ezért
az írás a mag rekordját küldi (`coreRowById`), nem a nézetet.

### Bizonyíték (a tényleges kimenet)

```
antikolt-shell --pintest <motor>
pin_click=ok
pin_id=01SEED000000000000000001 before=pinned
pin_page=unpinned pinned_count=2
pin_error=
pin_core=unpinned
pin_write=ok      (exit code 0)
```

Az állítás szándékosan a **megfordítás**, nem a „pin igaz lesz”: a tár megosztott a többi önteszttel (a
`--bridgetest` is billenti a pint), ezért a teszt az első kártyát kattintja, és azt követeli, hogy a lap
**és** a mag ugyanarra az ellenkező állapotra jusson. Két futtatás egymás után: `pinned → unpinned`, majd
`unpinned → pinned` — mindkettő `pin_write=ok`. Két független tanú egyezik: a lap saját
`window.antikoltPinState()`-je és egy **friss** mag-kliens `cards_list`-je.

## 2026-11-17 (huszonhatodik szelet) — a lap kérdezhet: a híd kétirányú (protokoll v2)

Az ADR-032 egyirányú hídja elég volt a rács feltöltéséhez, de a lap nem tudott **választ** kapni a magtól.
Ez a szelet átadta a kérés-irányt: a `window.antikoltNative.request(method, args)` **perzisztens
`cefQuery`**, amit a motor a kezében tart, `bridge_call` eseményként továbbad egy kérés-számmal, a shell
`bridge_reply`-vel válaszol, és a lap promise-a a válasszal zárul. Ez **protokoll v2** (ADR-034).

| Terület | Tartalom |
|---|---|
| Szerződés | `crates/antikolt-ipc`: `Message::BridgeReply { request, json, error }`, `PROTOCOL_VERSION` 1 → 2, `expects_reply()` hamis rá; roundtrip-teszt |
| Motor | `HostState::pending_bridge` (kérés-szám → callback), `OnQuery` kérés-ág (nincs azonnali `Success`), `bridge_reply` op, `OnQueryCanceled` eldobja a függő kéréseket; `kProtocolVersion = 2` a `protocol.h`-ban, a parser és a `welcome` is ezt használja |
| Shell | `EngineClient::bridgeReply`, `bridgeCall` jelzés + `request` szám, `MainWindow::replyToPage`; `cards.search` a magtól, ismeretlen metódusra **elutasítás** |
| Lap | `kBridgeScript`: `request()` + `cards.search()` helper, `version: '2'` |
| Önellenőrzés | `--bridgetest`: a lap kérdez, a shell a magtól válaszol, a lap saját változója kapja meg |

### Bizonyíték (a tényleges kimenet)

```
antikolt-shell --bridgetest <motor>
asked=ok
search_cards=1
search_first_title=Wikipedia — kezdőlap
unknown_method=rejected
bridge_request=ok      (exit code 0)
```

Két hiba a mérésből, mindkettő javítva: (1) a motor `ParseCommand`-ja **elutasította a v2 borítékot**
(`unsupported protocol version 2`), mert a verzióellenőrzés még `1`-et várt — a verzió most egy helyen
(`protocol.h kProtocolVersion`) él, és a `welcome` ugyanazt küldi; (2) a `C4457` figyelmeztetés
(`request` paraméter árnyékolása) `/WX` mellett hiba volt, ezért a változó `bridge_request_id`.

### A kártya írása is ugyanazon a csatornán (`cards.upsert`)

A kérés-irány csak akkor ér valamit, ha a lap **írni** is tud: a `cards.upsert` a teljes kártya-rekordot
viszi (pin, jegyzet, új kártya egy hívás), a shell a maghoz adja (`card_upsert`), a mag validál és tárol.
A `--bridgetest` ezt is méri: a lap kikeresi a „wiki” kártyát, **megfordítja a pin-jét**, elküldi, majd
**újra kérdez** — és a második válaszban már a megfordított érték jön, mert a válasz mindig a magból
származik, nem a lap saját másolatából.

```
asked=ok
search_cards=1
search_first_title=Wikipedia — kezdőlap
unknown_method=rejected
upsert_pinned=true
upsert_persisted=ok
bridge_request=ok      (exit code 0)
```

### Ami még nyitott

- A **rács saját keresése** továbbra is lokális (a lap a teljes listát kapja) — a csatorna azokra a
  kérdésekre készült, amiket a lap egyedül nem tud megválaszolni. A `cards.upsert` kész; a **rács UI-ja**
  (a pin-jelvény kattintása) még nem használja.
- A kérés-élettartam időkorlátja: ha a shell nem válaszol, a lap promise-a a lap élettartamáig vár; a
  motor-oldali eldobás lap-eltűnéskor kész, a shell-oldali válaszkötelezettség még nem kényszerített.

## 2026-11-17 (huszonötödik szelet) — a politika **végre is** lefut: a panel takarít

A huszonnegyedik szelet óta a motor tud törölni, de senki nem kérte. Most a panelen van
**„Takarítás most”**, és a shell maga hajtja végre a mag ítéletét: lekéri a `session_due`-t, és minden
esedékes originre vagy **azonnal** töröl (ha az a kártya van épp nyitva), vagy **sorba állítja**, és a
kártya következő megnyitásakor törli — mert a shellben egy élő felület van (I1), és egy takarítás nem
lophatja el az oldalt, amit a felhasználó olvas. A törlés a terv tárhely-listájával megy (ADR-033), a
siker után a sor `session_close`-szal eltűnik a ledgernből, és a panel frissül.

### Bizonyíték (a tényleges kimenet)

```
antikolt-shell --cleanuptest <motor>
origin=http://127.0.0.1:50114
session_written=ok
page_wrote=ok            ← a lap maga vall, hogy írt (nem a „semmi nem volt” eset)
storage_after=|          ← a süti és a localStorage is üres
rows_left=1 origin_rows=0   ← a mi sorunk eltűnt (a maradék 1 másik teszt sora)
cleanup=ok      (exit code 0)
```

Két tanulság a mérésből, mindkettő beépítve:

1. **Egy futó kliens a saját, régi másolatát olvassa.** A ledger-ellenőrzést először a futó oracle-lel
   végeztem, és `origin_rows=1`-et kaptam — pedig a sor már nem volt a fájlon. A `SessionStore` induláskor
   olvas; ezért a teszt **friss klienssel** olvassa vissza a fájlt, amit a shell épp írt.
2. A **megosztott adatkönyvtár** miatt a ledger nem üres (a `--sessionpanel` is ír sört), ezért a teszt
   nem a „0 sor” -t, hanem a **saját origin** eltűnését követeli meg.

### Ami még nyitott

- A `session_rules` op (szabályok állítása a huzalon) és a panelen a 24 óra / `wipe_on_app_lock`
  szerkesztése.
- A `LossEstimate` (mit veszít a felhasználó egy wipe-pal) megjelenítése a sorokban.
- A takarítás **média-kivétele**: ha a kártya játszik, a törlés várnia kell (a hibernálás kivétele már
  kész, a takarításé még nem).

## 2026-11-17 (huszonnegyedik szelet) — a wipe végrehajtása (a motor töröl, nem csak dönt)

A huszonkettedik szelet a döntést tette le, a huszonharmadik megmutatta; ez a szelet **végrehajt**.
A motor eddig `not_supported`-ot adott a `clear_origin_data`-ra — most a `Storage.clearDataForOrigin`
DevTools-metódust hívja, ugyanazon a `DevToolsCall`-on, mint a pillanatkép, a capture és az eval, tehát
a takarítás nem egy második, párhuzamos út a Chromium felé (ADR-029).

| Terület | Tartalom |
|---|---|
| Motor | `ClearOriginCall` (`Storage.clearDataForOrigin`, a terv tárhely-neveivel), `not_supported` helyett valódi végrehajtás; ismeretlen CDP-nevet naplóz |
| Név-egyeztetés | a terv `indexed_db`-je → CDP `indexeddb`; a többi átmegy (a Chromium az ismeretleneket figyelmen kívül hagyja, ezért a napló mondja meg) |
| Huzal | `clear_origin_data` válasza `{cleared: true, origin}`; a `Command` most `origin` + `storages[]` mezőt is olvas |
| Shell | `EngineClient::clearOriginData(surface, origin, storages, handler)` |
| Önellenőrzés | `--wipetest`: **valódi `http://` origin** egy helyi socketről (a `file://`-nak nincs, a `data:`-nak opak origója van), süti + localStorage írás, olvasás, törlés, újraolvasás |

### Bizonyíték (a tényleges kimenet)

```
antikolt-shell --wipetest <motor>
origin=http://127.0.0.1:49673
engine_start=ok
surface=1
load_status=200
storage_before=1|probe=1
clear=ok
storage_after=|
wipe=ok      (exit code 0)
```

A bizonyíték a **különbség**, nem a válasz: előbb olvashatónak kell lennie (`1|probe=1`), utána mindkét
tárolónak üresnek (`|` = üres localStorage + üres süti). A helyi HTTP-origin miatt a teszt nem függ a
hálózattól — ehhez a shell most megkapja a `Qt6::Network`-öt (csak a tesztekhez, egy socket a saját
processzben).

### Ami még nyitott

- A **panelről indított takarítás** még nincs: a `clear_origin_data` élő felületet vár (az adott
  partíció böngészőjét), a panel viszont nem nyit kártyát. A végrehajtás a következő szelet: a due sor
  kártyájának megnyitása (háttérben), törlés, majd vissza a rácsra.
- A `session_storage` és a `code_cache` a CDP-ben nem külön típus: a `sessionStorage` a lap eldobásával
  megy el (ezt a hibernálás már megteszi), a `code_cache` pedig a `cache_storage` része. A napló ezt
  kimondja, a kód nem hazudik róla.
- A `scroll`, `set_zoom`, `set_media`, `metrics` opok továbbra is `not_supported`-ot adnak.

## 2026-11-17 (huszonharmadik szelet) — a Munkamenetek panel

A huszonkettedik szelet huzalja kész volt, de semmi nem mutatta meg. Ez a szelet a **látható** felét
adta: a shell toolbarján a „🗂 Munkamenetek” gomb egy jobb oldali dokkot nyit, benne a ledger soraival
(origin, állapot, politika, sütik, **lejárt**), és a sorok a mag válaszából jönnek — a lista
`session_list`-ből, a „lejárt” jelzés a `session_due`-ból, a mag órájával és szabályaival (ADR-033). A
panel a saját szövegét is kiadja (`sessionPanelSummary`), hogy tesztelhető legyen anélkül, hogy a
modellre vagy a státuszsorra hagyatkoznánk.

### Bizonyíték (a tényleges kimenet)

```
antikolt-shell --sessionpanel <motor>
core_start=ok
session_written=ok
panel_grab=ok
panel_grab_size=1500x975
panel_rows=1
panel_row=https://example.org | expired | sweep | 7 | lejárt
session_panel=ok      (exit code 0)
```

A teszt előbb **beír egy sort** a szolgáltatás tárolójába (ugyanazon a huzalon, amit a shell használ),
majd megnyitja az ablak paneljét és a **panel saját szövegét** olvassa vissza: a sornak és a
politika-ítéletnek (`lejárt`) is ott kell lennie. Az ablak PNG-be is kimentve (`panel.png`), mert egy
üres táblázat a modellben helyes, a képen nem.

### Ami még nyitott

- A **végrehajtás**: a `clear_origin_data` motor-op továbbra is `not_supported`, tehát a panel csak
  megmutatja, mi lenne esedékes; a „takarítás most” gomb nem létezik.
- A szabályok (`SessionRules`) szerkesztése a panelen (24 óra, `wipe_on_app_lock`) még nincs.
- A `LossEstimate` (mit veszít a felhasználó egy wipe-pal) még nem jelenik meg a sorokban.

## 2026-11-17 (huszonkettedik szelet) — a munkamenet-ledger a magban (24 órás politika)

A `keep`/`sweep`/`wipe` logika eddig csak egy könyvtár volt (`antikolt-core::session`, 13 teszt),
amelyet semmi nem hívott. Ez a szelet bekötötte a mag-szolgáltatásba — a döntést a **mag** hozza, a
shell csak végrehajt (ADR-033).

### Ami elkészült

| Terület | Tartalom |
|---|---|
| Tár | `crates/antikolt-core-service/src/sessions.rs`: `sessions.json` (a kártyatártól **külön fájl**, atomikus írás), `SessionStore::{open,rules,list,upsert,close,due}`, `DueEntry` (sor + a hozzá tartozó `CleanupPlan`) |
| Ledger-szabály | a `validate()` a **írás előtt** fut: hamis sor nem kerülhet lemezre (`Rejected` → `invalid_argument` a huzalon) |
| Huzal-opok | `session_list`, `session_upsert`, `session_close`, `session_due` (a `now_ms` és az `app_locked` a hívóé) |
| Shell | `CoreClient::{sessionList,sessionUpsert,sessionDue,sessionClose}` |
| Önellenőrzés | `--coretest` a kártya után végigviszi a ledgert: írás → lista → due (2100-as órával) → close → üres lista |

### Bizonyíték (a tényleges kimenet)

```
cargo test -p antikolt-core-service
test sessions::tests::a_fresh_ledger_is_empty_and_survives_a_reopen ... ok
test sessions::tests::the_ledger_refuses_rows_that_would_make_it_lie ... ok
test sessions::tests::a_row_is_due_only_after_its_ttl_and_a_kept_row_is_never_due ... ok
test sessions::tests::closing_a_row_removes_it_once ... ok
test result: ok. 7 passed   (+ 2 integrációs teszt: ok)
cargo clippy -p antikolt-core-service --all-targets -- -D warnings   → tiszta

antikolt-shell --coretest <core>
session_upsert=ok
session_rows=1
session_due_rows=1
session_close=ok
rows_after_close=0
session_ledger=ok      (exit code 0)
```

A `session_due_rows=1` a lényeg: a szolgáltatás **nem** a shellben számol — a 24 órás szabályt a
`antikolt_core::session::plan_cleanup` adja, és a terv (`CleanupPlan`) a sorral együtt jön vissza, hogy
a hívó ne tudjon rossz tárhely-készletet törölni.

### Ami még nyitott

- A `clear_origin_data` motor-op továbbra is `not_supported`, tehát a **terv végrehajtása** (a
  tényleges `wipe`) még nem megy: a döntés kész, a végrehajtás a következő szelet.
- A szabályok (`SessionRules`) még nem állíthatók a huzalon (nincs `session_rules` op); a
  `sessions.json` a könyvtár defaultjaival indul.
- A **Munkamenetek panel** (`10-MUNKAMENET.md` §5) még nincs; a híd kész hozzá.

## 2026-11-17 (huszonegyedik szelet) — a kitűzött kártya mérve marad életben

A huszadik szelet a `pinned` jelzőt bekötötte, de mérés nélkül hagyta. A mérés (`--lifecycletest`
2. fázisa) két dolgot hozott: egy **igazi hibát** a shellben, és egy **tényt a mag adatairól**.

1. **Hiba:** a `live_card_pinned_` a hídhíváskor állt be, de a kártyacsere közben a **rács** saját
   `detach`-ja kitörölte — a jelző elveszett, a kitűzött kártya is hibernálódott (`pinned_closed=4`,
   `pinned_honoured=FAILED`). Javítás: a jelző `pending_pinned_`-ben vár, és a **kártya felületének
   csatolásakor** kerül át (`surfaceAttached`, nem-kezdőlap ág).
2. **Tény:** a mag vetett kártyái között az első („Antikolt projekt”) **kitűzött** — ezért a teszt
   1. fázisa, amely az első rács-kártyára kattintott, a jelző bekapcsolása után elbukott
   (`hibernated=FAILED`). A teszt most **nem kitűzött** kártyát keres a rácsban, a 2. fázis pedig
   kifejezetten kitűzöttet nyit.

### Bizonyíték (a tényleges kimenet)

```
antikolt-shell --lifecycletest <motor>
card_surface=2
card_closed=2                                       ← a nem kitűzött kártya hibernálódik
grid_back=file:///D:/Szerver/Antikolt/home/index.html
hibernated=ok
grid_returned=ok
pinned_open=sent
pinned_surface=4
pinned_honoured=ok                                  ← a kitűzött kártya 4 óraütés után is él
exit code 0
```

A `--gridtest` változatlanul zöld (`grid_matches_core=ok`, `grid_click_open=ok`). A tanulság ugyanaz,
mint a korábbi szeleteknél: **a jelző végigvitele nem bizonyíték** — csak az, hogy a két ág mérve
másképp viselkedik.

## 2026-11-17 (huszadik szelet) — egy parancs minden ellenőrzésre

A hat önteszt eddig hat külön, fejből tudott parancs volt — és egy munkamenet, amely egyet elfelejt,
olyan zöld állapotot jelent, amit nem mért meg. Most van `scripts/verify.ps1`: build + `--gridtest`,
`--lifecycletest`, `--opentest`, `--embedtest`, `--capturetest`, `--coretest` + `check-repo.ps1`, és a
végén egy táblázat meg egyetlen `ALL SELF TESTS PASSED`. Hiányzó binárist **FAIL**-ként jelent, nem
hagyja ki csendben; `-SkipBuild` / `-SkipRepo` a részmunkához.

Mérve:

```
Check         Result Detail
-----         ------ ------
gridtest      ok
lifecycletest ok
opentest      ok
embedtest     ok
capturetest   ok
coretest      ok

ALL SELF TESTS PASSED
```

Ez a szkript nem kapu (a CI kapuja továbbra is `check-repo.ps1`), hanem az a parancs, amit egy
munkamenet a állításai előtt lefuttat.

## 2026-11-17 (tizenkilencedik szelet) — a kártya magától lemezre megy (10 perc tétlenség)

A memória-ígéret lényege eddig nem volt bekötve: a fagyasztás csak kézi gombra működött. Most a shell
**inaktivitás-órát** tart a nyitott kártya fölött: alapból 10 perc (a `02-ARCHITEKTURA.md` §4.2 szerint),
`ANTIKOLT_HIBERNATE_MS`-sel rövidíthető (a teszthez). Aktivitásnak a felhasználó **egér- és
billentyűeseménye** számít (a shell ablakában bárhol), nem a motor forgalma.

Időtúllépéskor a shell előbb megkérdezi a lapot, hogy **játszik-e médiát** (a dokumentált kivétel), és ha
nem, akkor pillanatkép → `destroy_surface` → **a rács visszajön**. A rácsot csak a `destroy_surface`
*nyugtája* után kéri újra: ha előbb kér új felületet, a motor `engine_busy`-vel elutasítja (ezt a mérés
mutatta, lásd lent).

### Bizonyíték (a tényleges kimenet)

```
antikolt-shell --lifecycletest <motor>
card_surface=2
card_closed=2
grid_back=file:///D:/Szerver/Antikolt/home/index.html
hibernated=ok
grid_returned=ok      (exit code 0)
```

A motor naplója ugyanebből a futásból (a shell stderr-jén, időrendben):

```
op=eval        → bridge_call surface=1 method=open_card
op=snapshot    → snapshot written: 17065 bytes MHTML, 411 bytes state
op=destroy_surface surface=1
op=create_surface → surface=2 → navigate https://github.com/antikolt/antikolt
   … 1,5 s tétlenség (a teszt órája) …
op=eval        → '(média?)' → idle
op=snapshot    → snapshot written: 4419063 bytes MHTML, 526 bytes state
op=destroy_surface surface=2
op=create_surface → surface=3 → home (a rács visszajött)
```

### Két megfigyelés, ami a következő szeleteket is érinti

1. **A motor valódi HTTPS-oldalt tölt be** (a github.com betöltődött, 4,4 MB-os MHTML-nel), miközben a
   `curl` és a `cargo` TLS-e ezen a gépen elbukik (`schannel SEC_E_NO_CREDENTIALS`). A CEF a saját
   TLS-stackjét használja, tehát **a böngésző képes a valódi webre** — a hálózatfüggő tesztek (és a
   webfejlesztés) innentől nem kerülendők, csak a crate-letöltés blokkolt.
2. **`create_surface` nem előzheti meg a `destroy_surface`-t**: a rács visszakérését a destroy nyugtája
   után kell kérni, különben `engine_busy`. Ez a korábbi (nem hibernálásos) kártyaváltásra is igaz, ott
   véletlenül nem ütközött.

### Ami még nyitott

- A **kitűzött (pinned) kártya kivétele** még nincs bekötve: a mag tudja (`pinned`), a hibernálás
  egyelőre nem kérdezi meg.
- A **24 órás `keep`/`sweep`/`wipe`** politika és a munkamenet-ledger továbbra is hiányzik; a híd
  `cards.upsert`-ja (új kártya a lapról) szintén.
- A média-kivétel kódja kész, de **nincs tesztje** (médiát játszó kártya a készletben nincs).

## 2026-11-17 (tizennyolcadik szelet) — a shell naplója látszik

A tizennegyedik szelet nyitott tétele volt: „a motor `stderr`-je a shell alatt nem látszik (a `qInfo`
út nem ér el a konzolig)". Ez a szelet bekötötte: a shell `qInstallMessageHandler`-rel a **stderr-re**
írja a Qt-üzeneteket, és a motor `stderr`-jét a `qInfo`-n keresztül továbbítja — így a motor naplója
ugyanabban a terminálban olvasható, ahol a shell fut.

Mérve, egy `--gridtest` futás alatt a shell kimenetén (nem külön próbával):

```
[shell] info: [engine] [engine-host] op=eval id=9 surface=1
[shell] info: [engine] [engine-host] bridge_call surface=1 method=open_card
[shell] info: [engine] [engine-host] op=snapshot id=10 surface=1
[shell] info: [engine] [engine-host] state: 411 bytes written
[shell] info: [engine] [engine-host] snapshot written: 17065 bytes MHTML, 411 bytes state
[shell] info: [engine] [engine-host] op=create_surface id=12 surface=0
```

Ez a láthatóság nem kényelmi kérdés: a tizenhatodik szelet két hibáját (elutasított modul-szkript,
`ShadowRoot.dataset`) **csak** azért lehetett megtalálni, mert a lap `console`-ja a motor naplójába
került — és addig az a napló a shell alatt elveszett.

## 2026-11-17 (tizenhetedik szelet) — a kártyára kattintás tényleg megnyitja a kártyát

A tizenhatodik szelet nyitott kérdésként hagyta: a szintetikus kattintás lefut, de nem ér el a lap
kezelőjéig. Két külön hiba volt, mindkettő mérve.

### 1. A kitűzött sor kártyái halottak voltak

A DOM-lánc (a motor `eval`-jével mérve):

```
chain: ["ANTIKOLT-CARD", "pinned", "pinned-section", "MAIN", "BODY", "HTML"]
insideViewport: false
```

A kattintás-figyelő a `#viewport`-on volt delegálva, a **kitűzött (pinned) sor viszont azon kívül él**
— vagyis a legszembetűnőbb kártyák egyáltalán nem reagáltak kattintásra. Javítás: a delegálás a
`document`-en, a `closest(".card, antikolt-card")` dönti el, mit találtak el. Mindkét helyre egy
figyelő.

### 2. A lap a mintakártyát nyitotta, mert induláskor döntött a hídról

A javított figyelő már lefutott, de a motor naplója ezt mutatta (a lap `console`-ja):

```
console level=2 file:///…/home/app.js:101 [mock] open card 01MOCK… https://mail.google.com/
```

Vagyis a kattintás a **minta** `cards.open`-jét hívta. Az ok: a lap a szkript indulásakor döntött arról,
hogy van-e `window.antikoltNative` — a motor viszont a betöltés **után** fecskendezi be, tehát a lap
mindig a minta-ághoz ragadt. Javítás: a híd **hívás-időben** dől el (`withNativeOpen` →
`nativeBridge()`), és ha nincs natív híd (a fájlt sima böngészőben nyitották meg), a minta-viselkedésre
esik vissza.

### Bizonyíték (a tényleges kimenet)

```
antikolt-shell --gridtest <motor>
core_cards=3
grid_source=core
grid_count=3
grid_card=Antikolt projekt / Hacker News / Wikipedia — kezdőlap
grid_matches_core=ok
grid_click_url=https://github.com/antikolt/antikolt
card_click=clicked
card_surface=2
grid_click_open=ok      (exit code 0)
```

A motor oldaláról ugyanez: `bridge_call surface=1 method=open_card`, kétszer (a kitűzött sor és a rács
kártyájára is) — a próba mindkettőt külön mérte.

### Ami még nyitott

- A kattintás-fázis a `--gridtest`-ben most **elvárás**, nem megfigyelés: ha a híd vagy a delegálás
  elromlik, a teszt bukik.
- A kérés-irány (a lap kérdez: keresés, frissítés, új kártya) továbbra is protokoll v2 (ADR-032).

## 2026-11-17 (tizenhatodik szelet) — a rács a mag kártyáit mutatja (és a lap szkriptje eddig nem futott)

A rács eddig a kezdőlap saját **mintakártyáit** mutatta. Ez a szelet bekötötte a mag kártyatárát — és
közben kiderült, hogy a kezdőlap szkriptje **a motorban egyáltalán nem futott le**.

### Ami elkészült

| Terület | Tartalom |
|---|---|
| Shell → lap tolás | a kezdőlap betöltése után a shell `cards_list`-et kér a magtól, és **adatként** adja át: `window.antikoltAcceptCards(JSON.parse("…"))`, a JSON escape-elt string-literal (ADR-032) |
| Lap | `pageCardFromCore` adapter (mag-rekord → rács-modell), lap-oldali keresés a betöltött listán, `window.antikoltGridState()` — a lap saját tanúja arról, mit mutat |
| Shell | `MainWindow::pushCardsToGrid` + `home_surface_` (csak a kezdőlap kap listát); a `cards.open` a lapon látható listából veszi az URL-t, nem a mockból |
| Önellenőrzés | `antikolt-shell --gridtest <motor>`: a magtól kért lista az oracle, a lap a `antikoltGridState`-en keresztül vall |
| Motor | `OnConsoleMessage` → `stderr`: a lap `console`-ja és a JS-hibák most látszanak (eddig a renderer console-jában maradtak) |

### Bizonyíték (a tényleges kimenet)

```
antikolt-shell --gridtest <motor>
core_executable=D:/Szerver/Antikolt/target/debug/antikolt-core-service.exe
core_start=ok
core_name=antikolt-core
core_cards=3
home_loaded=file:///D:/Szerver/Antikolt/home/index.html
grid_source=core
grid_count=3
grid_card=Antikolt projekt
grid_card=Hacker News
grid_card=Wikipedia — kezdőlap
grid_first=Antikolt projekt
grid_matches_core=ok      (exit code 0)
```

A többi önellenőrzés változatlanul zöld: `--embedtest` `embed/paint/focus/viewport_match=ok`,
`--opentest` `card_opened=ok`, `--capturetest` `capture=ok capture_size=ok`, `--coretest` `cards=3
card_open=ok`.

### A hiba, amit ez a szelet talált (és ami miatt a rács üres volt)

A `home/index.html` a szkriptet **modulként** töltötte be (`<script type="module" src="app.js">`), a
motor viszont `file://`-ról nyitja a lapot — a Chromium pedig **CORS miatt elutasítja a modul-szkriptet
file: originról**. A lap szkriptje tehát a motorban soha nem futott le; a „minta-rács” csak akkor
létezett, ha valaki kézzel nyitotta meg a fájlt egy böngészőben. Mérve, a javítás előtt és után
(ugyanaz a próba, `window`-diagnosztika):

```
előtte:  {"accept":"undefined","state":"undefined","native":"object","cards":0}
utána:   {"accept":"function","state":"function","native":"object","cards":16,
          "grid":{"source":"sample","count":28,"firstTitle":"Google — munka",…}}
```

Javítás: klasszikus, `defer`-elt szkript (`<script src="app.js" defer>`); az `app.js` sosem használt
ESM-szintaxist, tehát a viselkedés csak a betöltés útjában változik. **Korrekció a tizedik szelethez:**
ott a „rács a hídon keresztül kér kártyát” valójában az önellenőrzés `eval`-hívása volt, nem a lap
saját kódja — a lap kódja akkor még nem futott. A második tanulság a diagnosztika: a motor mostantól
naplózza a lap `console`-üzeneteit, mert enélkül egy halott szkript pont úgy néz ki, mint egy üres rács.

### Második hiba: a kártya-render minden kártyán elhasalt

A lap `console`-ja (amit az új `OnConsoleMessage` most már kiír) 33-szor ugyanezt adta:

```
console level=4 file:///D:/Szerver/Antikolt/home/app.js:250
  Uncaught TypeError: Cannot read properties of undefined (reading 'signature')
```

Az `AntikoltCard.render()` a `this.shadowRoot.dataset`-et olvasta — a `ShadowRoot` azonban
`DocumentFragment`, nem `HTMLElement`, tehát **nincs `dataset`-je**: a hiba a render *legelején*
keletkezett, így **egyetlen kártya sem rajzolódott ki** (a rács üres `antikolt-card` elemekből állt).
A jel a capture méretén látszik: a kezdőlap képe **23 KB** volt a javítás előtt és **264 KB** utána
(ugyanaz a 1104×742 kép). A signature most a host elemre kerül (`this.dataset`), ahol lennie kell.

Ez a hiba a korábbi szeletekben is jelen volt: a „rács renderel” állítás csak a DOM-elemek számára
vonatkozott, a látható tartalomra nem. A tanulság ugyanaz, mint a modul-szkriptnél: **a lap
console-üzenete nélkül a hibák láthatatlanok**, ezért a motor mostantól naplózza őket.

### Ami még nyitott

- **A kérés-irány** (a lap kérdez: keresés, frissítés, új kártya) a perzisztens `cefQuery`-vel jön, és
  a `bridge_result` üzenettel **protokoll v2** (ADR-032); addig a tolás marad.
- **A kattintás útja** a tizenhetedik szeletben megoldódott (két külön hiba: a delegált figyelő a
  `#viewport`-on volt, a kitűzött sor viszont azon kívül él; és a lap induláskor döntött a hídról,
  mielőtt a motor befecskendezte). A `--gridtest` kattintás-fázisa ott tért vissza, immár mérve.
- **A lap `cards.open`-je a mintakártyát nyitotta** — ugyanaz a start-up döntés; a részletek és a
  javítás a tizenhetedik szeletben.
- A rács **előnézetei** még nincsenek (a `capture` primitív kész, a 260×150-es WebP-pipeline nem).
- A kártyák szerkesztése/címkézése/pin-jelölése a lapon még nincs: a mag tudja (`cards_upsert`), a híd
  nem vezet rá utat.

## 2026-11-17 (tizenötödik szelet) — a széf alakja kész, a titkosítás blokkolt

Az ADR-027 sorrendje szerint a DevTools-réteg után a széf jön. Ez a szelet a széf **alakját** építette
meg — és mérve megállapította, hogy a **titkosítás** ebben a környezetben nem megépíthető. Nem lett
helyette kézzel írt AES és nem lett néma plaintext-tárolás: a varrat zártan viselkedik (ADR-031).

### Ami elkészült

| Terület | Tartalom |
|---|---|
| `crates/antikolt-core/src/vault.rs` | `Vault` zálog: nyitás/zárás, 5 perces idle auto-lock (`tick` pontosan egyszer jelez), `touch`, `require_open` kapu, `open_for_ms`; `SecretKind` (`password`/`totp`/`passkey`, szigorú parse); `VaultStatus` (soha nem tartalmaz titkot); `VaultError` a huzal-kódokkal (`vault_locked`, `not_supported`, `internal`) |
| Titkosítási varrat | `SecretBox` trait: `seal`/`open` a **rekord AAD-jével** (origin + azonosító, ADR-010) + `available()`. A fában egyetlen implementáció: `UnavailableSecrets`, ami mindent megtagad, és kimondja az okot |
| Tesztek | 10 új egységteszt: 4:59 nyitva / 5:00 zár (határinkluzív), az aktivitás újraindítja az órát, a visszafelé lépő óra nem hosszabbít, a varrat soha nem ad vissza nyílt szöveget, a `status` nem tartalmaz titkot |

### Bizonyíték (a tényleges kimenet)

```
cargo test --offline
  antikolt-core (lib)       77 passed   (a 67-ből; +10 a széf)
  antikolt-core-service      3 passed
  antikolt-ipc              14 passed
  protocol/integráció        2 + 1 passed
```

### A blokkoló, mérve (nem vélemény)

```
$ curl -sS -m 20 -o config.json https://index.crates.io/config.json
curl: (35) schannel: AcquireCredentialsHandle failed: SEC_E_NO_CREDENTIALS (0x8009030e)

$ ls ~/.cargo/registry/src/index.crates.io-*/ | wc -l
14        # serde, serde_json, thiserror és a függőségeik — kriptográfiai crate nincs köztük
aes-gcm, argon2, sha2, zeroize, rand_core, getrandom, rusqlite, windows, winapi   → MISSING
```

A crate `#![deny(unsafe_code)]`, tehát a platform kulcstár (DPAPI) FFI-vel sem érhető el. AES-ot kézzel
írni nem opció. Ezért a széf ma **nem tud titkot tárolni**, és ezt a `VaultStatus.can_store = false` +
`unavailable_reason` ki is mondja, nem elrejti.

### Ami még nyitott (pontos sorrend)

1. **A crate-ek bevendorolása** (`cargo vendor` egy hálózattal rendelkező gépen): `aes-gcm`, `argon2`,
   `zeroize` (+ a `rand_core`/`getrandom` lánc), majd `AesGcmSecrets: SecretBox` implementáció a
   rekord-AAD-vel, és a DEK köré a rendszer kulcstár (DPAPI, `unsafe` engedélyezése egyetlen, izolált
   modulban vagy a `windows` crate-tel).
2. Utána a széf **tárolója**: `credential` tábla (a séma már megvan a `0001_init.sql`-ben), a
   `pending_maintenance` mintájára atomikus írás.
3. Utána a **passkey**: `WebAuthn.enable` / `addVirtualAuthenticator` / `getCredentials` /
   `setCredential` — mind a most kész `DevToolsCall` típuson (ADR-029), a hitelesítő adatok a széfben.
4. Utána a TOTP (RFC 6238) és az autofill (izolált world, `Page.createWorld`).

## 2026-11-17 (tizennegyedik szelet) — `capture`: a motor a saját képét adja

A tizenharmadik szelet kimondta, hogy a festést nem a képernyőről kell mérni. Ez a szelet megépítette a
hozzá szükséges réteget és magát a mérést: a **DevTools-réteg egyetlen típuson** megy át, és a `capture`
protokoll-op a motor kompozítorától kér képet.

### Ami elkészült

| Terület | Tartalom |
|---|---|
| `apps/engine-host/src/devtools.{h,cpp}` | **`DevToolsCall`**: metódus + paraméterek + válaszhívó, egyedi üzenet-azonosítóval; a sorozatokat a hívók láncolják. **`DevToolsReply`**: a válasz egyszer parse-olva (`success`, `error`, `from_protocol`, `result`, `raw`), és **birtokolja a parse-olt JSON-dokumentumot** (ADR-029) |
| Motor | `MhtmlCapture` és `EvalCapture` átállítva a közös típusra (viselkedés-azonos, ~180 sor duplikáció törölve); új `ScreenshotCapture`; a válasz-parse egy helyen él |
| `capture` op | `Page.captureScreenshot` (`png`/`jpeg`/`webp`, `quality` 0..100, `fromSurface:true`, `captureBeyondViewport:false`); a fájl `<user_data>/screenshots/shot-N-M.<fmt>`, a válasz: `{ok, path, bytes, width, height, format, surface}` — a **szélesség/magasság a kép fejlécéből** (PNG/JPEG/WebP), nem a viewportból számolva |
| Shell | `EngineClient::capture`; új önellenőrzés `--capturetest <motor>`; az `--embedtest` festés-mérése **átállt a capture-re** (a képernyőkép `screenshot_colours=… (informational)` sorként megmarad, de nem dönt átmenetről) |

### Bizonyíték (a tényleges kimenet)

`antikolt-shell --capturetest <motor>` — a kártya valódi, beágyazott felületen, három színsávos lapon:

```
engine_start=ok
capture_capability=announced
surface=1
load_status=200
page_css=700x460
dpr=1.25
expected_physical=875x575
capture_path=C:/Users/info/AppData/Roaming/Antikolt Project/Antikolt/capture-selftest\screenshots\shot-1-0.png
capture_bytes=2775
capture_size=875x575
capture_image=875x575
capture_colours=4
capture=ok
capture_size=ok      (exit code 0)
```

Vagyis a kép a **lap saját viewportja × eszközskálája** (700×460 CSS → 875×575 device px), és a három
sáv + a fehér háttér valóban ott van a képen.

`antikolt-shell --embedtest <motor>` — ugyanaz a mérés a beágyazott kártyán, a többi méréssel együtt:

```
page={"w":900,"h":640,"dpr":1.25,"screenW":4096,"screenH":1152,"outerW":0,"outerH":0}
screenshot_colours=20 (informational)
capture_colours=25
capture_size_match=ok
page_has_focus=true
embed=ok  viewport_match=ok  paint=ok  focus=ok      (exit code 0)
```

A `--opentest` (kártyanyitás a hídon át) ismét zöld: `bridge=object`, `open_card=ok`, `card_opened=ok`;
a `--coretest`: `cards=3`, `card_open=ok`. A `scripts/check-repo.ps1`: **ALL CHECKS PASSED** (46 SPDX-os
forrásfájl, link-ellenőrzés, `node --check`, `cargo fmt`/`clippy -D warnings`, 87 Rust-teszt).

A protokoll-próba (kézi stdio-kliens) a formátumokat és a hibautat is mérte ugyanazon a felületen:

```
capture png  -> {"bytes":2495,"format":"png","height":442,"width":779,"ok":true,...}
capture webp -> {"bytes":1704,"format":"webp","height":442,"width":779,"ok":true,...}   (quality=70)
capture gif  -> {"code":"invalid_argument","message":"format must be png, jpeg or webp"}
snapshot     -> {"bytes":1189,"mhtml":true,"state_bytes":390,...}
```

A `state.json` tartalma (a pillanatkép második fele, ami eddig üresen maradt):

```json
{"url":"data:text/html,…","title":"probe","scrollX":0,"scrollY":0,
 "anchor":{"selector":"html > body:nth-of-type(1) > div:nth-of-type(1)","dy":0},
 "forms":[],"session":{},"media":[],"viewport":{"width":623,"height":354,"dpr":1.25}}
```

### Három hiba, amit ez a szelet hozott (mind javítva)

1. **A `DevToolsReply` üres szótárat adott** (`type=''`, `value=''`, `state_bytes=0`), miközben a nyers
   válasz helyes volt: `{"result":{"type":"string","value":"…"}}`. A CEF belső szótárai a parse-olt
   JSON-**dokumentumban** élnek; amikor a dokumentum felszabadult, a szótár *üresen* jött vissza —
   nem hibázott, hanem hazudott. Pontosan ezt írta le a korábbi kód kommentje („a real bug that cost a
   debugging round"), most **másodszor** került elő, amikor a dokumentum élettartama a hívótól ehhez a
   típushoz került. A javítás: a válasz **tagja** a dokumentumnak (`document`), nem lokális változó.
2. **A pillanatkép `state.json`-ja eddig csendben üres maradt** (`state_bytes=0`). Ugyanaz a hiba;
   a javítás után `state_bytes=390`, `state: 390 bytes written`.
3. **A `data:` URL-ben a `#` fragmentumnak számít**: a `--capturetest` első változata
   `background:#06c`-t írt, a lap a `#`-nél elvágódott, a kép egyszínű lett (`capture_colours=1`).
   Percent-kódolva (`%2306c`) 4 szín. Ez a fajta csapda a mérésben magában is hibaforrás.

Mellékesen: a `QScreen::grabWindow` nem `const`, a diagnosztika-helper `QScreen*`-t kap.

### Mérési megfigyelés, ami a következő szeletre kötelez

Az **önálló, takart** motorablak (`parent_window=0`, a konzolablak mögött) `capture`-je **egyszínű**
(`capture_colours=1`) volt, miközben a beágyazott, látható kártyáé 25 szín. A kompozítor tehát a takart
ablakot nem festi. Következmény: az **előnézet-készítésnek látható felületet kell adnia** (vagy a
`fromSurface:false` utat kell használnia), különben a rács néma, üres előnézeteket kapna. Ezt az
ADR-030 rögzíti.

### Ami még nyitott

- Az **előnézet-pipeline** (260×150, WebP q=80, blob-tár + luminance/dhash, napi 100 frissítés) még nem
  épült meg: a `capture` primitív kész, a feldolgozó nem.
- A `capture`-nek nincs `from_surface` kapcsolója, és nincs még `metrics` op (a `capture` képmérete most a
  mérés, a DOM-csomószám/szórás-próbák jönnek később).
- A **széf** a következő szelet (ADR-010, ADR-027): a DevTools-réteg már készen áll rá, a passkey
  hívásai (`WebAuthn.*`) ugyanezen a `DevToolsCall` típuson mennek majd.
- A motor `stderr`-je a shell alatt nem látszik (a `qInfo` út nem ér el a konzolig), ezért a hibát egy
  kézi, PowerShell-alapú stdio-próba tárta fel, ami a nyers kereteket és a motor stderr-jét is
  megmutatta. Ezt a csatornát érdemes bekötni (vagy a `crates/antikolt-probe`-ot használni rá).

## 2026-11-17 (tizenharmadik szelet) — a festés-mérés megbízhatatlan: `capture` kell

A `--embedtest` festés-ellenőrzése a képernyőről készít másolatot (`QScreen::grabWindow`) és
megköveteli, hogy ne legyen egyszínű. Ez **környezetfüggő**: ugyanaz a bináris, ugyanaz a kód az
egyik futásban `screenshot_colours=25` (zöld), a másikban `colours=1` (bukik), miközben a geometria és
a fókusz végig helyes:

```
embed=ok  viewport_match=ok  focus=ok     — a lap 900×640 CSS-pixelt lát, a fókusz átmegy
paint=FAILED                              — a képernyőről készült kép egyszínű
```

Újrapróbálkozó méréssel (6 próba, 200 ms) sem javult, ezért a kísérletet **visszavontam** — egy
„néha zöld” teszt rosszabb, mint egy őszintén bukó.

**Döntés:** a festést nem a képernyőről kell mérni. A motoron belül kell képet kérni
(CDP `Page.captureScreenshot`), ami a kompozitortól független, determinisztikus, és ugyanazt bizonyítja.
Ez lesz a `capture` protokoll-op implementációja — és ugyanez az útvonal adja majd a rács
**előnézeti képeit** is, tehát nem teszt-segédeszköz, hanem termékfunkció.

Amíg ez nincs meg, a `paint` ellenőrzés **ismert bizonytalanság**: a többi mérés (`embed`,
`viewport_match`, `focus`) ettől függetlenül érvényes.

## 2026-11-17 (tizenkettedik szelet) — a shell beszél a maggal

Új `CoreClient` a shellben: elindítja a `antikolt-core-service`-t, kézfog, kártyalista, kártyanyitás —
ugyanazzal a keretezéssel, mint a motor-link. `antikolt-shell --coretest <mag>`:

```
core_start=ok
core_name=antikolt-core   core_version=0.1.0
cards=3
first_id=01SEED000000000000000001
opened_at_ms=1.7949e+12
core_ready=ok  cards_list=ok  card_open=ok      (exit code 0)
```

Egy hiba: a `welcome` saját üzenetfajta, ezért a `hello` **kérés id-jét** visszhangozza `to` mezőben —
enélkül a kliens örökké várt (10 s timeout, majd „a mag válaszára időtúllépés”).

**Ami még nyitott:** a rács még nem ezt a listát mutatja (a híd `cards_list` válasza, perzisztens
`cefQuery`-vel), és a magot a shell még nem indítja el automatikusan a főablak indulásakor.

## 2026-11-17 (tizenegyedik szelet) — a mag saját folyamatban: kártyák valódi tárból

A shell eddig nem beszélt a Rust maggal: a kártyalista a kezdőlap mockja volt. Most elindult a mag
**saját folyamatként** (`antikolt-core-service`), és valódi kártyákat szolgál ki.

| Terület | Tartalom |
|---|---|
| `antikolt-ipc` | `encode_json_frame` / `decode_json_frame`: a hossz-prefixes keretezés most már nem csak az `Envelope`-ra jó, hanem a mag kisebb üzenetkészletére is — egy helyen élnek a szigorú bemeneti szabályok |
| `antikolt-core-service` | `hello/welcome`, `cards_list`, `card_open` (időbélyeg + átrendezés), `card_upsert`, `shutdown`; a tár `cards.json`, atomikus írással (temp fájl → rename), első indításkor 3 mintakártyával |
| Tesztek | 3 tár-unit teszt + 2 integrációs teszt, amely **valódi processzt** indít, valódi stdin/stdout linken kérdez, és újraindítás után is látja az elmentett időbélyeget |

A tár egyelőre JSON, nem SQLite: a `rusqlite` offline környezetben nem elérhető, a migrációs séma
(`antikolt_core::migrations`) viszont már megvan, és a kártya alakja egy helyen dől el — az SQLite-ra
váltás így egy fájl cseréje lesz, nem az API-é.

**Ami még nyitott:** a shell még nem indítja el a magot; a híd `cards.list` válasza (perzisztens
`cefQuery`, a shell → mag → lap útvonal) a következő szelet fő tétele. Addig a rács a mockból dolgozik,
a kártyanyitás viszont már valódi.

## 2026-11-17 (tizedik szelet) — a kezdőlap rákattintásra kártyát nyit

A termék köre most már **végig** működik: a kezdőlap (a rács) a hídon keresztül kér egy kártyát, a
shell hibernálja a rácsot, és megnyitja a kért oldalt élő kártyaként.

### Ami elkészült

| Terület | Tartalom |
|---|---|
| Motor (C++) | CEF `CefMessageRouter` mindkét oldala (render + browser); `install_bridge` parancs; a híd-szkript a motorban él; `bridge_call` esemény; a híd minden fő keret betöltése után újratelepül; engedély nélküli felület minden kérést elutasít |
| Rust szerződés | `EngineEvent::BridgeCall { surface, method, args }` (az `args` szándékosan átlátszatlan) + roundtrip teszt |
| Shell | `EngineClient::installBridge`/`bridgeCall`; `MainWindow::openCard`: pillanatkép → megsemmisítés → **új** felület új partícióval → navigálás; a kezdőlap kapja meg egyedül a hidat |
| Kezdőlap | `withNativeOpen(...)`: ha van `window.antikoltNative`, a kártyanyitás valódi, a lista egyelőre a mockból jön |
| Önellenőrzés | `antikolt-shell --opentest <motor>`: a híd jelenléte, a hívás és az új felület is ellenőrizve |

### Bizonyíték (a tényleges kimenet)

```
load_finished surface=1 status=200 url=file:///D:/Szerver/Antikolt/home/index.html
bridge=object
open_card=sent
load_finished surface=2 status=200 url=data:text/html,<title>card2</title><h1>card two</h1>
first_surface=1 second_surface=2 live=2
home_loaded=ok  bridge=ok  open_card=ok  card_opened=ok      (exit code 0)
```

Vagyis: a rács **1-es** felülete hibernálva lett (pillanatkép a lemezre), és a kért kártya **2-es**
felületként, **új partícióval** nyílt meg — nem ugyanabban a felületben navigálva.

### Két hiba, amit ez a szelet hozott (mind javítva)

1. **A `CefMessageRouter` csak akkor létezik, ha a render folyamat is megkapja** (`CefRenderProcessHandler`
   `OnWebKitInitialized` + `OnContextCreated`). Enélkül a `window.cefQuery` nem függvény, a híd pedig
   pont úgy néz ki, mint egy elromlott kezdőlap.
2. **Leállításkor összeomlott a shell (0xC0000005).** Az ok: a `MainWindow` destruktora után a motor
   kliense még kiváltott egy várakozó válasz-callbacket, ami egy már félig lebontott ablakba írt. Most a
   kliens a destruktorában `destroying_` jelzőt állít, törli a várakozó hívásokat és blokkolja a
   szignálokat — a leállás sorrendje pedig: felület bezárása → `shutdown()` (ez a sorompó) → Qt bontás.

### Ami még nyitott

- A híd szándékosan **egyirányú**: a lap nem vár választ a shelltől (a `cefQuery` azonnal nyugtáz).
  A perzisztens lekérdezés (pl. „sikerült-e megnyitni?”) a fagyasztási szeletre marad.
- A kártyalista még a kezdőlap mockja; a core SQLite kártyatáblája és a `cards.list` híd még nincs
  bekötve — ez a következő szelet fő tétele.
- A kártyaváltás minden esetben **ment** a lemezre (helyes, de lassabb, mint egy memóriabeli váltás);
  a „gyors váltás” optimalizálás később jön.

## 2026-11-17 (kilencedik szelet) — a kártya tényleg a saját ablakunkban él

Eddig a motor a **saját** ablakában mutatta az oldalt, a shell meg egy placeholder-t rajzolt. Ez a
szelet áttöri azt a falat: a motor a shell ablakába **ágyazva** rajzol, külön folyamatban maradva.

### Ami elkészült

| Terület | Tartalom |
|---|---|
| Protokoll | `create_surface` `config.parent_window` (natív ablakkezelő, `0` = saját ablak); új `set_bounds` és `focus` üzenet; a mértékegység **DIP** (a lap CSS-pixele), nem fizikai pixel |
| Rust szerződés | `SurfaceConfig.parent_window` (alapértelmezés `0`, tehát additív, verziót nem lépő változás), `Rect`, `Message::SetBounds`, `Message::Focus`; 2 új teszt (régi `create_surface` elfogadása, `set_bounds` előjeles origója) |
| Motorház (C++) | `CefWindowInfo::SetAsChild(parent)`, DIP→fizikai váltás a monitor DPI-jével, `ApplyPendingEmbeddedBounds` a pump hurokban, a szülő ablakot **soha** nem bontja le |
| Shell (Qt) | `LiveCard` natív widget: `winId()` a motornak, minden átméretezésnél `set_bounds`, fókusz átadás; a főablak ezt használja a placeholder helyett |
| Önellenőrzés | `antikolt-shell --embedtest <motor>`: a beágyazott gyerekablak és a lap viewportja is ellenőrizve |

### Bizonyíték (a tényleges kimenet)

```
card_screen_origin=150,151
set_bounds=ok applied=1
load_status=0
child_windows=2
child=Chrome_WidgetWin_1 window=1125x800 at 150,151 client=1125x800
child=Chrome_RenderWidgetHostHWND window=1125x800 at 150,151 client=1125x800
chromium_child=found
card_logical=900x640
card_physical=1125x800
dpr=1.25
page={"w":900,"h":640,"dpr":1.25,"screenW":4096,"screenH":1152,"outerW":900,"outerH":640}
embed=ok
viewport_match=ok        (exit code 0)
```

Vagyis a Chromium ablaka **pontosan a kártya helyén és méretében** van (1125×800 fizikai = 900×640
DIP 125%-on), és a lap **900×640 CSS-pixelt** lát — nem többet. A kép tehát a kártyába kerül, nem egy
külön ablakba.

A geometria azonban még nem bizonyíték arra, hogy **történik is festés**: egy ablak lehet jó helyen és
jó méretben, és akkor is üres. Ezért az önellenőrzés képernyőképet is készít a kártya területéről
(`QScreen::grabWindow(card->winId())`), és megköveteli, hogy ne egyszínű legyen:

```
screenshot=C:/Users/info/AppData/Local/Temp/antikolt-embed.png
screenshot_size=1125x800
screenshot_colours=25
paint=ok
```

A felvételen a `<h1>embedded</h1>` szöveg olvasható a kártya területén, ablakkeret és böngésző-chrome
nélkül — vagyis a motor valóban a mi ablakunkba fest, nem külön ablakban nyílt meg.

A negyedik láncszem a **fókusz**: egy másik folyamat gyerekablaka nem örökli az aktiválást, ezért a
shell `focus` üzenetet küld, és a lapnak egyet kell értenie:

```
page_has_focus=true
embed=ok  viewport_match=ok  paint=ok  focus=ok        (exit code 0)
```

### Négy hiba, amit ez a szelet hozott (mind javítva, mind mérve)

1. **A beágyazott téglalap mértékegysége DIP, nem fizikai pixel.** Fizikai pixelt küldve a Chromium
   *másodszor* is felskálázta: 900×640-es kártyához 2756×1622-es ablak lett, a lap 1575 px viewportot
   látott, a magasságot pedig a képernyő munkaterülete vághatta le. A határt a napló mondja ki: a
   `SetAsChild` és a `set_bounds` **DIP-ben** értendő, a DIP→fizikai váltás a motor dolga (csak ő tudja
   a monitor skáláját). *Ez a legfontosabb tanulság: a rossz mértékegység nem elszíneződést okoz, hanem
   csendben rossz layoutot.*
2. **A CEF gyerekablakának osztályneve `Chrome_WidgetWin_1`**, nem `_0`. A `_0`-ra szűrt keresés némán
   `nullptr`-t adott, ezért a `set_bounds` soha nem találta a felületet — a méret helyes maradt, az
   ablak viszont a képernyőn kívülre került. Most előtag-egyezés van (`Chrome_WidgetWin_`).
3. **A `set_bounds` gyakran előbb érkezik, mint a Chromium ablaka.** A `create_surface` ugyanis már
   akkor válaszol, amikor a böngésző még csak *kérés* szinten létezik. A téglalap ezért eltárolódik, és
   a pump hurok helyezi el, amint a gyerekablak megvan (`applied: false` = „megjegyezve”, nem hiba).
4. **A tesztablak a képernyőn kívülre került**, mert a pozícióját nem adtam meg: így a gyerekablak
   `-2066,198` koordinátája hibának látszott, pedig a CEF helyesen dolgozott. A mérés most explicit
   pozíciót ad (`window.move(120, 90)`), és a kártya fizikai képernyő-origóját (`ClientToScreen`) a
   gyerekablak `GetWindowRect`-jával hasonlítja össze.
5. **Az `eval` válasza csak szöveget adott vissza.** A `document.hasFocus()` logikai értéke üresen
   érkezett (`page_has_focus=`), mert a motor `GetString("value")`-vel olvasta a DevTools eredményét.
   Most a primitívek (logikai, egész, tört) a JSON-szövegükké alakulnak, a `value` pedig továbbra is
   mindig szöveg a protokollon — így nem kellett és nem is szabadott a válasz alakját megváltoztatni.

### Ami még nyitott

- A beágyazott felület fókusza **mérve** működik (`page_has_focus=true`), de valódi billentyűleütést
  még nem küldtem át a gyerekablakon — az első interaktív teszt a következő szeletben lesz.
- A kártyaváltás (mentés → megsemmisítés → új felület) a shell `❄ Fagyasztás` gombján át működik, de a
  „másik kártya megnyitása” még nem.
- A GPU-t ebben a környezetben továbbra is ki kell kapcsolni (`ANTIKOLT_ENGINE_DISABLE_GPU=1`), különben
  a GPU-folyamat elszáll; ez a méréseket is érinti.

## 2026-11-17 (nyolcadik szelet) — a ciklus bezárva: mentés → visszaállítás offline

A böngésző legfontosabb ígérete most már **végig** bizonyított: egy oldal lemezre mentése és **offline
visszaállítása** ugyanazzal a motorral, hálózat nélkül.

### Ami elkészült

| Terület | Tartalom |
|---|---|
| `eval` parancs (C++) | DevTools `Runtime.evaluate {returnByValue}` → `{type, value}` válasz; erre épül a visszaállítás |
| `antikolt-probe --restore <state.json>` | az MHTML megnyitása `file://` címen (offline), majd az állapot visszaírása: görgetés, űrlapértékek (`input`/`change` eseményekkel), `sessionStorage`, média-pozíció |
| Ellenőrzés | a visszaállítás után egy második `eval` igazolja a címet, a görgetést, a mezők számát és a megjelenő szöveget |

### Bizonyíték (a tényleges kimenet)

```
pre-scroll: scrollY=600
snapshot bytes=1440193 state_bytes=4452 ...
state loaded: title="Webböngésző – Wikipédia" scrollY=600 forms=18
navigate=file:///C:/…/surface-1-0.mhtml
load_finished status=Some(200) url=file:///…/surface-1-0.mhtml
restore applied: forms=18 session=0 media=0 scrollY=600
verified: title="Webböngésző – Wikipédia" url=file:///… scrollY=600 fields=18
          text="Ugrás a tartalomhoz\nFőmenü\nKeresés\nAdományok\n…"
probe=ok        (exit code 0)
```

Vagyis: **a lap szövege megjelenik offline**, a **görgetés (600 px) visszaáll**, a **18 űrlapmező
visszakapja az értékét** — motorral, de hálózat nélkül.

### Két újabb hiba, amit ez a szelet hozott (mind javítva)

1. **A DevTools üzenet-azonosítónak egyedinek kell lennie.** Állandó id-t használtam, ezért a *második*
   `Runtime.evaluate` válasza némán elveszett — pontosan úgy nézett ki, mint egy beragadt motor. Most
   minden DevTools-hívás a `NextDevToolsId()` számlálóból kap id-t, és a választ a saját id-jére illeszti.
2. **PowerShell-foglalt változnevek**: a `$host` után a `$profile` is elhasalt (a szkriptem hibázott, nem
   a program). A mérési szkriptek most tudatosan kerülik ezeket.

### Őszinte korlát

A visszaállított MHTML **statikus dokumentum**: a DOM, a stílusok, a képek és az űrlapértékek megvannak,
de az oldal saját JavaScriptje nem fut újra „élő" SPA-ként. Ezért a visszaállítás **olvasásra és
folytatásra** jó, nem arra, hogy egy összetett webalkalmazás futásidejű állapota pontosan folytatódjon —
ezt a `02-ARCHITEKTURA.md` 5.1. pontja már az elején kimondta.

---

A kártya lemezre mentése most már **két fájlt** ír: a teljes oldalt (MHTML) és a DOM-on túli állapotot
(JSON). Ez a teljes hibernálási csomag.

### Ami elkészült

| Terület | Tartalom |
|---|---|
| `kStateExpression` (JS) | görgetés (x/y), **horgony** (legfelső látható elem szelektora + eltolás), űrlapmezők (`input`/`textarea`/`select`, mezőnként max. 2000 karakter), `sessionStorage`, média-pozíció (`currentTime`, `paused`), viewport/DPR |
| `MhtmlCapture` állapotgép | `Page.enable` → `Page.captureSnapshot` → `Runtime.evaluate {returnByValue:true}` → két fájl, egy válasz |
| Biztonsági szabály | **jelszó- és hidden mezők soha nem kerülnek a pillanatképbe** — a snapshot nem hitelesítőadat-tár |
| Válasz | `path`, `bytes`, `state_path`, `state_bytes`, `url`, `title`, `surface` |

### Mért/ellenőrzött eredmény (magyar Wikipédia-cikk)

```
surface-1-0.mhtml       1 440 191 bájt   (From: <Saved by Blink>, beágyazott kép + CSS)
surface-1-0.state.json      4 386 bájt
title=Webböngésző – Wikipédia
scrollY=0  viewport=1263x754  dpr=1,25
anchor=html > body:nth-of-type(1) > div:nth-of-type(2)
forms=18  session_keys=0  media=0
```

A 18 űrlapmező a Wikipédia keresőmezője és a lap egyéb vezérlői — a jelszómezők kimaradtak, ahogy kell.

### Egy újabb CEF-interop buktató (érdemes megjegyezni)

A **`CefValue` tulajdonolja a belső szótárát**: ha egy segédfüggvény visszaadja a `CefValue`-ból kinyert
`CefDictionaryValue`-t, a `CefValue` megszűnésekor a szótár **kiürül** — a `HasKey()` hamis lesz, a
`GetKeys()` üres. Ez némán „nincs adat" hibát okoz. A megoldás: a parse-olt `CefValue`-t életben kell
tartani addig, amíg olvassuk belőle a szótárat (a segédfüggvényt ezért ki is vettem).

---

A pillanatkép most már nem csak a DOM: a **teljes oldal, beágyazott erőforrásokkal**, egyetlen MHTML
fájlban — ez az, amit a kártya lemezre mentésekor tényleg meg kell őrizni, mert ez olvasható vissza
hálózat és motor nélkül.

### Ami elkészült

| Terület | Tartalom |
|---|---|
| `MhtmlCapture` (C++) | CEF DevTools-csatorna: `Page.enable` → `Page.captureSnapshot {format: mhtml}`; a válasz a `CefDevToolsMessageObserver::OnDevToolsMethodResult`-ban érkezik, base64-detektálással (a CEF a kész MHTML-t adja, más átvitelek base64-et), majd `<user_data>/snapshots/surface-N-M.mhtml` |
| Fallback | ha a DevTools-módszer nem indítható, a régi `GetSource` DOM-mentés fut — soha nincs „nincs pillanatkép" |
| Diagnosztika | minden DevTools-válasz nyers első 240 bájtja a stderr-en (a csatorna tisztán marad) |

### Mért eredmény (2026-11-17, Wikipédia-cikk)

| Fázis | Medián | Megjegyzés |
|---|---|---|
| betöltve | 179,0 MB | |
| pillanatkép közben | 184,6 MB | a csúcs +5,6 MB: az MHTML a memóriában áll össze, mielőtt lemezre kerül |
| lemezen (frozen) | 117,8 MB | |
| **visszaadott RAM** | **61,2 MB** | 179,0 → 117,8 MB |

A pillanatkép-fájl **1 440 191 bájt**, fejléce `From: <Saved by Blink>`, és a mérés ellenőrizte, hogy
**beágyazott képet és CSS-t is tartalmaz** — tehát offline, motor nélkül is megjeleníthető.

### Tanulság, amit érdemes megjegyezni

A DevTools `OnDevToolsMethodResult` a **kész MHTML-t** adja vissza (nem base64-et), és a **result
objektumot** kapja, nem a `{"id":..,"result":{..}}` burkolót. Ez a két apróság némán „nincs adat" hibát
okoz, ezért a kód mindkét formát elfogadja, és a nyers választ naplózza.

---

A termék legfontosabb mechanizmusa most már **működik és mérhető**: a motor kimenti a lap DOM-ját
fájlba, a surface megszűnik, és a mérés mutatja, mennyi RAM szabadul fel.

### Ami elkészült

| Terület | Tartalom |
|---|---|
| `snapshot` parancs (C++) | `CefFrame::GetSource` → szerializált DOM → `<user_data>/snapshots/surface-N-M.html`, majd válasz (`path`, `bytes`, `url`, `title`). A választ a `SnapshotVisitor` küldi, amikor a DOM megérkezik. |
| `capabilities` | a motor most már `snapshot` képességet is jelent |
| `antikolt-probe --freeze` | fázisokra bontott folyamat (`loaded` → `snapshot` → `frozen` → `done`), `phase=<név> ts=<ms>` jelölőkkel |
| `perf/measure-freeze.ps1` | 400 ms-onként mintát vesz a folyamatfáról, a fázisjelölők alapján mediánt számol fázisonként, és megírja a riportot |

### Mért eredmény (2026-11-17)

| Oldal | Betöltve | Lemezen | Visszaadott RAM | Pillanatkép |
|---|---|---|---|---|
| `example.com` | 127,3 MB | 107,7 MB | **19,6 MB** | 2,4 KB |
| Wikipédia-cikk | 182,6 MB | 115,9 MB | **66,7 MB** | 345 KB |

A pillanatkép-fájl tartalmát a mérés ellenőrizte: valódi HTML `<title>Webböngésző – Wikipédia</title>`
címmel. A visszamaradó ~116 MB a **motor alapja** — ezt a T2/ultra-low mód a motorház leállításával adja
vissza, pontosan a terv szerint.

### Nyitott (a következő szeletre)

A pillanatkép egyelőre **csak a DOM**. A teljes lemezhibernáláshoz kell még: MHTML a beágyazott
erőforrásokkal, görgetési pozíció + horgony, űrlap- és `sessionStorage`-állapot, médialejátszó pozíció.
Ezek nélkül a visszaállítás „statikus olvasás" szintű, nem teljes állapot-visszaállítás.

---

## 2026-11-16 (negyedik szelet) — a shell ↔ motorház protokoll **él**

Ez a szelet köti össze a böngésző két felét: a Rust oldal (a leendő mag) és a C++ motorház most már
ugyanazt a protokollt beszéli, amit a `crates/antikolt-ipc` definiál.

### Ami elkészült

| Terület | Tartalom |
|---|---|
| `apps/engine-host/src/protocol.{h,cpp}` | stdio-csatorna (4 bájtos hossz-előtag + JSON), olvasó szál + parancssor, **CEF saját JSON-parserével**; `hello`/`welcome`, `ping`, `create_surface`, `navigate`, `destroy_surface`, `shutdown`; események: `surface_created`, `surface_closed`, `load_finished`, `load_failed`, `title_changed` |
| `crates/antikolt-ipc/src/transport.rs` | `EngineLink`: gyermekfolyamat indítása, keretezés, kézfogás, kérés–válasz párosítás időkorláttal, esemény-várakozás |
| `crates/antikolt-probe` | önálló bináris, ami **végigviszi** a folyamatot: spawn → handshake → create_surface → navigate → load_finished → destroy → shutdown |
| Protokoll-bővítés | `Message::Shutdown`, `EngineEvent::SurfaceCreated/SurfaceClosed` (+ a JSON-formákra illesztett tesztek) |

### Bizonyíték (a tényleges kimenet)

```
$ antikolt-probe antikolt-engine-host.exe https://example.com
spawned pid=32488
engine=cef version=158.0.3+g8c29be2+chromium-158.0.8127.18 chromium_major=Some(158) capabilities=4
surface=1
navigate=https://example.com
load_finished surface=1 status=Some(200) url=https://example.com/
surface_destroyed=1
engine_host_exited
probe=ok
exit code = 0
```

Ez azt jelenti, hogy a **valódi oldalbetöltés a protokollon keresztül** működik: a kliens kér, a
motorház CEF-fel betölt, eseményt küld, a kliens lezárja. A böngésző két fele **beszél egymással**.

### Hat hiba, amit ez a szelet felszínre hozott (mind javítva)

1. **A CEF JSON-író kapcsolója `JSON_WRITER_*`** (nem `JSONWRITER_*`), és a `cef_types.h`-ban van.
2. **Egész számokat egészként kell írni.** A `SetDouble(1.0)` → `1.0` a JSON-ban, amit a Rust `u64`
   nem tud fogadni. Javítás: `SetInt` + `JSON_WRITER_OMIT_DOUBLE_TYPE_PRESERVATION`. Ez a fajta hiba
   némán megállítja a protokollt, ezért most **teszt** őrzi a C++ által küldött JSON-formákat.
3. **`GetKeys()` out-paraméteres** ebben a CEF-verzióban.
4. **A saját kliensem végtelen ciklusba futott**: a `request()` a nem illeszkedő eseményt visszatette a
   várakozási sorba, majd újra kivette — miközben a válasz a csatornában várt. Javítás: az eseményeket
   félreteszi, a válasz megérkezése után **egyszer** teszi vissza őket. (Ez volt a „miért nem jön
   válasz, mikor a napló szerint minden válaszolt" rejtély.)
5. **A PowerShell `$host` foglalt változó** — a saját tesztparancsom rossz argumentumot adott át.
6. **Egy korábbi próba-folyamat életben maradt**, és zárolta az exe-t (`os error 5` a buildnél) — a
   tesztek most mindig leállítják a maradék folyamatokat.

Plusz: az `ANTIKOLT_ENGINE_DISABLE_GPU=1` környezeti kapcsoló, mert ebben a környezetben a
**GPU-folyamat hatszor összeomlik**, és a CEF újraindításai megzavarják a mérést. Ez nem termékbeállítás,
csak a determinisztikus futtatás eszköze (a `18-F0-JELENTES.md` fenntartásai között szerepel).

### Következő szelet

1. A **fagyasztás** prototípusa: `snapshot` parancs a motoron (MHTML), majd a motor megszüntetése és a
   memória visszaesésének mérése — a termék legfontosabb mechanizmusa.
2. A GPU-folyamat leállásának kivizsgálása (működő GPU-s mérés).
3. A Qt-héj bekötése ugyanerre a `EngineLink`-re (első valódi shell ↔ motor hívás).

---

## 2026-11-16 (harmadik szelet) — valódi böngészőablak és az első memóriamérés

### Interaktív mód: a bezárás is működik

A harmadik szelet végére az ablak **valódi oldalt tölt be és rendezetten záródik**:

```
$ antikolt-engine-host --url https://example.com
load_end url=https://example.com/ status=200
(ház felhasználó bezárja az ablakot → WM_CLOSE)
load=ok
url=https://example.com/
status=200
title=Example Domain
exit code = 0, maradék folyamat = 0
```

Ehhez három újabb hibát kellett megoldani:

1. **A `CefQuitMessageLoop` hatástalan volt** a blokkoló `CefRunMessageLoop` mellett (a `WM_CLOSE`
   megérkezett, a ciklus mégsem állt le). Megoldás: **external message pump** — a ház maga pumpálja a
   Windows-üzeneteket és hívja a `CefDoMessageLoopWork`-öt. Ez egyben **a Qt-integrációhoz szükséges
   forma** is, tehát nem kerülőút, hanem a helyes architektúra.
2. **A CEF aszinkron hozza létre a böngészőt**, ezért az „ameddig van böngésző" ciklusfeltétel azonnal
   kilépett (a böngésző még nem létezett). Megoldás: `browser_created` jelző, amit az `OnAfterCreated` állít.
3. **A jelentés hazudott**: „load=ok"-ot írt akkor is, ha nem történt betöltés. Most `load=none`, és a
   mérési mód ilyenkor 1-es kilépési kóddal jelez.

### Ami elkészült

| Terület | Tartalom | Állapot |
|---|---|---|
| `apps/engine-host` | `CefInitialize` + **valódi natív ablak** + egy CEF surface + load/title callbackek + `--selftest` / `--url` / `--size` / `--hidden` | ✅ fut, exit 0 |
| `libcef_dll_wrapper` | a CEF C++ wrapper bekötve (`libcef_dll_wrapper` target, `CEF_STANDARD_LIBS`) | ✅ |
| `perf/measure-engine.ps1` | a folyamatfa privát memóriájának mérése, markdown + JSON riport | ✅ |
| `docs/HU/18-F0-JELENTES.md` | **az F0 döntési kapu anyaga, valós számokkal** | ✅ |

### Az F0 mérés eredménye (ez a lényeg)

| Oldal | Motor mediánja | Folyamatok |
|---|---|---|
| `about:blank` | 119,0 MB | 7 |
| Wikipédia-cikk | 177–185 MB | 7 |
| Google Maps | 142,9 MB | 7 |
| YouTube nyitólap | 386–390 MB | 10 |

Teljes böngésző (motor + Qt-héj + mag) várhatóan **270–510 MB** → az 1 GB-os cél **bőven tartható**.
A részletes elemzés és a fenntartások (GPU-folyamat hiánya a mérésben, commit vs working set):
[`18-F0-JELENTES.md`](18-F0-JELENTES.md).

### Öt hiba, amit a valódi CEF-motor felszínre hozott (mind javítva)

1. **A segédfolyamatokat a saját argumentum-parserünk ölte meg** (`--type=gpu-process`), ezért a GPU
   leállt. Javítás: a `--type=` előtagot a parser előtt elkapjuk → `CefExecuteProcess`.
2. **`CefRunMessageLoop` + gyors betöltés = örökös blokk**: a `data:` URL a ciklus indulása *előtt*
   betöltődött, a quit-jel elveszett. Javítás: determinisztikus pump-ciklus (`CefDoMessageLoopWork`).
3. **Élő böngészővel hívott `CefShutdown` = access violation** (ezt látta a projektgazda a
   hibapárbeszédablakban). Javítás: `CloseBrowser(true)` + pumpálás lezárás előtt.
4. **`WS_POPUP`, soha meg nem jelenített ablak** → GPU-kompozitálási hiba. Javítás: normál ablaktípus,
   `ShowWindow` nélkül.
5. **CEF 158: nincs `settings.user_data_path`** → `cache_path` / `root_cache_path`.

Plusz: `SetErrorMode` kikapcsolja a hiba-párbeszédablakot, hogy a hibás futás kilépési kóddal jelezzen.

### Nyitott pont a következő szeletre

A **GPU-folyamat leáll** ebben a környezetben (`0x80000003`), a Chromium szoftveres kompozitálásra esik
vissza. A mérés ezért GPU-folyamat nélküli fát tükröz — működő GPU-n +50–150 MB várható. Ezt előbb
kivizsgáljuk, mint hogy a mérést „véglegesnek" tekintenénk.

---

## 2026-11-16 (folytatás) — eszközlánc telepítése, első natív build

A projektgazda kérésére az eszközöket **én telepítettem** a `D:\Tools` alá (a gép eszközkönyvtára).

### Telepítve

| Eszköz | Verzió | Hely |
|---|---|---|
| **Qt 6** | 6.8.3 (MSVC 2022 x64; qtbase + qtdeclarative + qtsvg) | `D:\Tools\Qt\6.8.3\msvc2022_64` |
| **CEF** | 158.0.3 (Chromium 158.0.8127.18, beta) | `D:\Tools\cef\cef_binary_158.0.3+…_windows64_beta` |
| MSVC / SDK / CMake / Ninja | 19.44 / 10.0.26100.0 / 3.31.6 / a BuildTools-ból | — |

Részletek, buktatók és a telepítés újrafuttatása: [`17-ESZKOZLANC.md`](17-ESZKOZLANC.md).

### Ami elkészült

| Terület | Tartalom | Állapot |
|---|---|---|
| `CMakeLists.txt` | gyökér build: Qt kötelező, CEF opcionális (`ANTIKOLT_CEF_ROOT`), konfigurációs összefoglaló | ✅ |
| `apps/desktop-shell` | Qt-ablak, vezérlősáv, **négy téma** (világos/szürke/sötét/AMOLED), `--selftest` | ✅ fut |
| `apps/engine-host` | motorház váz: **CEF-hez linkel**, `--selftest` a `libcef.dll` betöltését igazolja | ✅ fut |
| `scripts/build.ps1` | automatikus eszközfelderítés (vswhere + lemez), kézi `PATH`/`INCLUDE`/`LIB`, Ninja, öntesztek | ✅ |
| `scripts/install-cef.ps1` | CEF letöltés + kicsomagolás + verifikáció (numerikus verzióválasztás) | ✅ |

### Ellenőrizve (a tényleges kimenet)

```
=== Self test (shell) ===
antikolt-shell 0.1.0
qt=6.8.3
platform=offscreen
quick=1
theme=light ok / grey ok / dark ok / amoled ok
ok

=== Self test (engine host / CEF) ===
antikolt-engine-host 0.1.0
cef=158.0.3+g8c29be2+chromium-158.0.8127.18
chromium=158.0.8127.18
cef_runtime=ok
ok
```

A `cef_runtime=ok` azt jelenti, hogy a fejlécek fordulnak, az importkönyvtár linkel, a `libcef.dll`
betöltődik a kimeneti mappából, és egy CEF C-függvény (`cef_string_utf16_set`) hívható. Ez a
legkockázatosabb integrációs lépés, és **működik**.

### Négy buktató, amit menet közben találtam (mind kódolva a `build.ps1`-ben)

1. A `vcvars64.bat` itt 39 bájtos továbbító, és nem hoz létre fordító-környezetet → a szkript kézzel
   állítja a `PATH`/`INCLUDE`/`LIB`-et.
2. A „Visual Studio 17 2022" CMake-generátor nem találja a Build Tools-t → Ninja generátor.
3. A CMake cache-be adott backslash-es útvonal szintaktikai hibát okoz (`rc.exe`) → perjeles útvonalak.
4. A CEF indexe nem időrendben van: az első szkriptverzió a 2016-os Chromiumot választotta → numerikus
   rendezés.

### Ami még hiányzik a motorháztól

A **böngészőablak** (natív gyermekablak + egy CEF surface + `CefInitialize`/`CefRunMessageLoop`),
a `libcef_dll_wrapper` bekötése, a **CEF sandbox** bekapcsolása, és a mérés. Ez az F0 spike köre —
a következő szelet.

---

## 2026-11-16 — Első fejlesztési szelet: a Rust mag és a repó alapja

### Amit megépítettem

| Terület | Tartalom | Állapot |
|---|---|---|
| Cargo workspace | `crates/antikolt-ipc`, `crates/antikolt-core`, közös profilok | ✅ fordul |
| **IPC-szerződés** | 20 üzenet, 4 bájtos hossz-előtagos keretezés, verzió-ellenőrzés, hibakódok, képesség-lekérdezés | ✅ 10 teszt |
| **Azonosítók** | ULID-szerű, időrendezett, Crockford base32, validáló (de)serializáció | ✅ 8 teszt |
| **Óra** | injektálható `Clock`, `ManualClock`, telítődő időszámítás | ✅ 4 teszt |
| **Erőforrás-profilok** | 5 profil (Gyors/Kiegyensúlyozott/Takarékos/Ultra/Kézi), memória-nyomás sávok, automatikus választás | ✅ 9 teszt |
| **Életciklus-gép** | `Saved→Warming→Live→Freezing→Saved`, purge/archív ág, kivételek, fagyasztási határidő, I1 invariáns | ✅ 15 teszt |
| **Munkamenet-politika** | `keep`/`sweep`/`wipe`, TTL, zárolásra törlés, ledger-sor, „mit veszítünk" becslés | ✅ 13 teszt |
| **Pillanatkép-manifest** | digest típus, mezők, validáció (URL, vezérlőkarakterek, luminance, verzió) | ✅ 14 teszt |
| **Migrációk** | `0001_init.sql` (14 tábla/index/FTS), beágyazás, validáció | ✅ 8 teszt |
| **Kezdőlap** | `<antikolt-card>`, virtualizált rács, 4 téma, keresés, billentyűzet, mock bridge | ✅ szintaxis-ellenőrzött |
| **Repó** | LICENSE (MPL-2.0), NOTICE, AUTHORS, SECURITY, CONTRIBUTING, CODEOWNERS, PR-sablon, CI, .gitignore | ✅ |

### Hogyan ellenőriztem (nem állítás, hanem parancs)

```
cargo test --offline          ->  67 passed (core) + 10 passed (ipc)  = 77
cargo clippy --all-targets --offline -- -D warnings   ->  clean
cargo fmt --all -- --check    ->  clean
node --check home/app.js      ->  OK
SPDX-fejléc ellenőrzés        ->  16/16 forrásfájl rendben
markdown link check           ->  0 broken links
git commit (root)             ->  57 fájl, 9498 beszúrás
```

**A saját CI-szabályom fogta meg a saját hibámat:** az első commit után a helyi ellenőrzés szerint az
`antikolt-ipc` négy forrásfájljából hiányzott az SPDX-fejléc (a `core` fájljaiban ott volt, ezekben nem).
Ez pontosan az, amire a CI `hygiene` jobja való — ezért a fejlécet pótoltam, és a `.gitattributes`-szel
a sorvégeket is normalizáltam (Windows checkout / Linux CI ugyanazt a bájtot lássa). Tanulság a
folyamatra: **a helyi ellenőrzésnek ugyanazt kell futtatnia, mint a CI-nak**, különben a hiba csak a
push után derül ki.

A 77 teszt a *viselkedést* rögzíti, nem a kód formáját: a 10 perces hibernálás a 9:59-es és a 10:00-es
határon, a kivételek (kitűzés/hang/videó/letöltés/popup), a fagyasztási időtúllépés utáni degradált
mentés, a `sweep` nem jelent kijelentkezést, a `wipe` igen, a manifest elutasítja a `javascript:` URL-t
és a vezérlőkaraktereket, az id generálás 10 000 mintán egyedi.

### Ami blokkol — és miért nem írtam meg vakon

| Akadály | Mérés | Hatás | Megoldás |
|---|---|---|---|
| **Nincs Qt 6** | `C:\Qt`, `D:\Qt`, `C:\Program Files\Qt` egyik sem létezik | a Qt-héj nem fordítható | Qt Online Installer → Qt 6 MSVC 2022 64-bit (csak Core/Gui/Quick, ~2 GB) |
| **Nincs CEF** | `C:\cef`, `D:\cef`, `C:\vcpkg` nem létezik | nincs motor, nincs valós memóriamérés | CEF standard build (Windows x64, ~1–2 GB) a `third_party/cef/` alá |
| **Van viszont C++ eszközlánc** | `cl.exe`: `…\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe`; `cmake.exe`: `…\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` | csak PATH-on kívül vannak | a build a `vcvars64.bat`-tal indul, ahogy a Rust-tesztek is |
| **Nincs crates.io elérés** | `SSL connect error ... SEC_E_NO_CREDENTIALS` | új crate-ek nem telepíthetők | `--offline` a helyi cache-ből (263 crate); **rusqlite / libsqlite3-sys / sha2 nem elérhető** |
| **Nincs SQLite kötés** | `rusqlite` hiányzik a cache-ből | a DB-réteg nem írható meg | a séma és a migráció-validáció kész; az `antikolt-db` crate a következő szelet |
| **Nincs SHA-256 provider** | `sha2` 0.10 lánca (`generic-array`) hiányzik | a `Digest` típus kész, de nincs feltöltő | következő szelet (sha2 vagy FIPS-vektorokkal tesztelt saját implementáció) |
| **A WMI-t a sandbox tiltja** | `Get-CimInstance` → hozzáférés megtagadva | a 32 GB RAM nem olvasható, a felhasználó adata | a `09-CELHARDVER.md` jelöli, hogy felhasználói adat |

### Mit kell telepíteni a shellhez (két dolog, ~1–2 óra)

A C++ eszközlánc **megvan**, tehát a hiányzó rész kettő:

1. **Qt 6** — Qt Online Installer, „Qt 6.x → MSVC 2022 64-bit", ebből elég a *Qt Core / Gui / Quick*
   (a Qt WebEngine **nem** kell: a motort a CEF adja). Telepítés után: `C:\Qt\6.x\msvc2022_64`.
2. **CEF** — a standard Windows 64-bit build kicsomagolva a repó `third_party/cef/` mappájába
   (a bináris nem kerül verziókezelésbe, a `.gitignore` kizárja).

Amint ez a kettő megvan, a következő szelet: `CMakeLists.txt` + a minimális `main.cpp` (üres Qt-ablak),
majd az F0 első spike-ja: egy CEF-nézet beágyazása és a **memória megmérése** a három referenciaoldalon.

Ez a lista nem kifogás, hanem a munkanapló: minden sorhoz tartozik egy konkrét következő lépés.

### Döntések, amelyek a kód írása közben születtek

1. **Az azonosító deszerializálása validál.** A `Id` nem `#[derive(Deserialize)]`, mert az nyers
   stringet engedne be; a saját implementáció `Id::parse`-ot hív, így a lemezről vagy hálózatról jövő
   azonosító mindig kanonikus.
2. **`pending_maintenance` tábla.** A 24 órás törléshez motor kell; ha épp nincs, a feladat sorba
   kerül (`docs/HU/02` §4.3). Ezt a táblát a terv nem nevesítette, a végrehajtás megkövetelte.
3. **A fagyasztási határidő külön esemény.** A `Tick` 2,5 s után `FreezeTimedOut`-ot vált, ami
   degradált mentést és motor-megsemmisítést jelent — így egy lassú oldal nem akasztja meg a böngészőt.
4. **A `Warming` alatti távozás megszakítja a betöltést** (nincs mit menteni), nem fagyaszt.
5. **A kezdőlap egységes M méretű kártyákat virtualizál**, a kitűzött kártyák L méretben, nem
   virtualizált sorban jelennek meg (max. 8). A szabad elrendezés a felhasználó saját kezdőlapjára
   marad — ez tudatos kompromisszum a memória-ígéret javára.

### Ismert hiányosságok a kódban (nem rejtett TODO-k)

- `snapshot::Digest`: a típus és a validáció kész, a SHA-256 feltöltő **nincs** meg.
- `migrations`: a listák és a validáció kész, az **alkalmazó** (`apply`, tranzakció, `user_version`)
  a DB-crate-ben jön.
- A `session`-ledger csak a *logika*; nincs mögötte SQLite-lekérdezés és nincs UI-panel.
- A kezdőlap **mock bridge-dzsel** fut; a valódi `window.antikolt` injektálás a shell feladata.
- A `proto/v1` írott specifikációja megvan (`proto/README.md`), de a C++ oldali implementáció nincs.

### Következő szelet (F1 első fele)

1. `antikolt-db` crate: rusqlite (amint elérhető), WAL pragmák, migráció-futtató, tranzakciós tesztek.
2. SHA-256 provider + blob-tár (tartalom szerint címzett, zstd nélkül első körben).
3. `perf/` mérőpad váza (folyamatfa privát munkakészlet mérése), hogy a büdzsé mérhető legyen.
4. Utána a shell (CMake + Qt 6 + CEF) — ez az első olyan lépés, amihez a VECTOR-on eszközbeszerzés kell.
