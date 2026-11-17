# 16 — Fejlesztési napló

> Kronológiai napló arról, mi készült el, **hogyan lett ellenőrizve**, és mi blokkol. A terv a
> `00`–`15` dokumentumokban van; ez a napló a valóságot rögzíti.

## 2026-11-17 (ötödik szelet) — a fagyasztás működik, mérve

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
