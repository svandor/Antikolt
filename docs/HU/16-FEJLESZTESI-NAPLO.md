# 16 — Fejlesztési napló

> Kronológiai napló arról, mi készült el, **hogyan lett ellenőrizve**, és mi blokkol. A terv a
> `00`–`15` dokumentumokban van; ez a napló a valóságot rögzíti.

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
