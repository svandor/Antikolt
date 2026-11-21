# 21 — Hol tart a termék (mért állapot)

> Ez a lap **nem terv**: azt rögzíti, ami **mérve** működik, mi van nyitva, és mi az, amihez emberi lépés kell.
> A mért állapot forrása a kapu: `pwsh -File scripts/verify.ps1` → **ALL SELF TESTS PASSED**, és a napló
> (`16-FEJLESZTESI-NAPLO.md`) szeletei, ahol minden szám egy tényleges futás kimenete.

## A kapu, egy pillantással

```
build + selftest  ok        (CMake + MSBuild + Qt, mindkét bináris elkészül)
core service      ok        (a mag válaszol: hello, cards_list)
38 önteszt        ok        (lásd alább, mit bizonyít mindegyik)
check-repo        ok        (SPDX, linkek, JS-parse, cargo fmt, clippy -D warnings, 78+47+6+1+15 Rust-teszt)
```

A hordozható példány: `scripts/dist.ps1 -Release` → `dist/Antikolt/` (445.6 MB), asztali és Start-menü
parancsikonnal; a telepített binárison a `--profiletest`, `--workspacetest`, `--containertest`, `--tagtest`,
`--addcardtest` külön is lefutott (`exit=0`), mert a `dist` ugyanaz a kód, de nem ugyanaz a fájl.
**Kivétel, kimondva:** a CEF-GPU blokker **feloldva** — a `--disable-software-rasterizer` kapcsoló kapcsolta ki a
SwiftShader-t a `ANTIKOLT_ENGINE_DISABLE_GPU=1` ágban; javítva és mérve (`--embedtest` → `exit=0` 2,6 s alatt). A
VECTOR hardveres GPU-útja továbbra is elhal (RTX 5080 + Intel, Chromium 158 beta), ezért ezen a gépen a kapu
`$env:ANTIKOLT_ENGINE_DISABLE_GPU='1'`-gyel fut; a hardveres út bisectelve (napló 93): a d3d11 ANGLE-háttér a hibás a
5080-on, a `--use-angle=gl` megy. A motor stdio-hibája is feloldva (`protocol.cpp`: a `stdin`-t egy privát
`DuplicateHandle`-másolatból olvassa, mert a Chromium bezárja az örökölt handle-t), és a keret-írás is binárissá lett
(`_setmode(_fileno(stdout), _O_BINARY)`, mert a szöveges `stdout` a kerethossz `0x0A` bájtját CRLF-fé torzította —
napló 94). A `containertest` Qt6Core-krachja is feloldva (`main.cpp`: az `evaluate_json`/`evaluate_text` segédek a
saját `QEventLoop`-jukat `std::make_shared`-állapotba teszik — a gyökeret AddressSanitizer adta). **A teljes kapu így
zöld: `ALL SELF TESTS PASSED`** (`$env:ANTIKOLT_ENGINE_DISABLE_GPU='1'`), a `--containertest` → `exit=0` 12 s, minden
`container_*` ok. Napló 90–94. szelet.

## A repó nyilvános

`https://github.com/svandor/Antikolt` — **PUBLIC**, GitHub szerint `licenseInfo: mpl-2.0`, a szerző
**Varga Sándor (`@svandor`)**, a leírás kétnyelvű. A távoli fán **149 fájl** van, és nincs rajta build-kimenet,
felhasználói adat vagy titok (`build/`, `dist/`, `target/`, `node_modules/`, `*.exe`, `*.dll`, `*.sqlite3` → egy
találat sem). A CI az első push után **zöld** lett (`completed success`, 2m12s) — a hosted gépek a Rust-teszteket,
a JS-parse-t, az SPDX- és linkellenőrzést futtatják; a Qt/CEF binárisokat és a 38 natív öntesztet **nem** (azok
Qt-t, CEF-et és GPU-t kérnek), ezt a README ki is mondja, és a mért eredmény a naplóban van, nem badge-ben.

## Ami mérve kész (és mit bizonyít a teszt)

| Terület | Teszt | Mit bizonyít (mért kimenet) |
|---|---|---|
| Rács, 500 kártya | `--gridscaletest` | 503 kártya, **13** renderelt DOM-elem, 20 ms az első renderig; a görgetéses ugrás `not-measurable` (nincs layout-magasság a harnessben) |
| Tároló (négy store) | Rust-tesztek, `--addcardtest` | mind a négy store **SQLite-sorok** (`user_version=4`, WAL): kártyák, konténerek, munkaterületek, munkamenetek; a négy `*.json` származtatott export; `db_status=ok schema=4 … verified=yes` mind a négyet soronként méri; a kapcsoló előtti profil store-onként egyszer átvételre kerül (ADR-070, napló 97) |
| Keresés | `--searchtest`, `--tagtest` | ékezet-független cím/webcím/jegyzet/címke, és az **oldalszöveg** is (`search_page_text=ok`) |
| Konténerek | `--containertest` | színes pont, választó a kártyán, chip-legend + szűrő, tábla a magban, `＋`/dupla kattintás/`×`, használt konténer törlése **áthelyezéssel** |
| Munkaterületek | `--workspacetest` | fül-sor a magban tárolt munkaterület-táblából, a kártya `workspace` mezője tárolt adat, fül-életciklus, kártya-választó |
| Címkék | `--tagtest` | címke a kártyán, mag-normalizálás (trim/dedup/12×32), kereshető |
| Kezdőlap | `--addcardtest` | `＋` + soros űrlap: a kártya a magba kerül (cím/URL/munkaterület ellenőrizve), átnevezés a cím-szerkesztő `Enter`-jével (`wrote_by=gesture`), webcím-átírás, tároló-ellenőrzés (`db_status=ok schema=4 … verified=yes`), törlés; a teszt kitakarít maga után |
| Profil-költöztetés | `--profiletest` | export → import friss profilba → visszaolvasás **fájlból**; ismeretlen séma elutasítva |
| Munkamenetek | `--sessionpanel`, `--scrollbook`, `--wipetest`, `--cleanuptest` | látszik, mi van életben, mennyi RAM/lemez; `keep`/`sweep`/`wipe` végrehajtás |
| Előnézet | `--preview*` | a háttér-worker szabályai (ADR-041) és az ütemező mérve |
| Memória | `--metricstest` | az engine working set-je mérve, a költségvetés nem kitalált |
| Tálca, napló, téma | `--traytest`, `--logtest`, `--themetest` | a napló nem ír feleslegesen, a témák a helyes színt adják |

## Ami nyitva van (tudatosan, nem elfelejtve)

1. **A kártya szerkesztése** — létrehozás (`＋`, ADR-061), törlés, **átnevezés** (ADR-062) és **webcím-átírás**
   (ADR-063) működik a felületen, mind mérve (`wrote_by=gesture`, `edit_card_url=ok`). Nyitott: a törlés
   megerősítése, és hogy a webcím-váltás után a rögzített oldalszöveg a régi tartalom marad.
2. **A konténerenkénti engedély/proxy (F3) — a motor fele kész, a felület fele nyitott.** A partíciónkénti
   `CefRequestContext`, a konténer-proxy és az engedély-politika kész és mérve (`--partitiontest`, ADR-071, napló 98:
   külön profilkönyvtár, `fixed`/`direct` proxy a kontextuson, ismeretlen engedély megtagadva). A **rácson nincs
   szerkesztő** a proxyhoz/engedélyekhez: a házirend adatbázis-sor, a hídon (`containers.upsert`) írható; a
   partíció-profilkönyvtárak takarítása is nyitott.
3. **OAuth/SSO popup** — a mechanizmus kész (I1-kivétel, 60 s), a bejelentkezési folyamat nincs összekötve.
4. **A köteg felületi gombja** — a mag, a híd (`profile.export/import`) és a lap-út (`antikoltProfileWrite/Read`)
   kész és mérve (ADR-059/060); a **natív fájlválasztó** hiányzik, mert a harness nem tud dialógust kattintani —
   ez a lépés önmagában `not-measurable`.
5. **A profil-köteg titkosítása** — a köteg munkamenet-sorokat is tartalmaz; ez valódi döntés, nem kényelmi
   kérdés.
6. **Fogd-és-vidd a munkaterületekhez** — a `<select>` mellett, nem helyette (ADR-058).

## Amihez emberi lépés kell (nem kód)

- **Aláírás:** SignPath Foundation (nyílt forrású kódaláírás) és/vagy Store — a kulcs nem a mi gépünkön van
  (`20-ALASAS.md`).
- **Passkey/vault:** `cargo vendor` hálózat nélkül nem megy ebben a környezetben; a CDP-s passkey-kísérlet
  eredménye `12-PASSKEY.md`-ben.
- **Kiadási workflow:** a GitHub Actions kiadás még soha nem futott (nincs aláírt artefaktum).

## Hogyan ellenőrizhető mindez öt perc alatt

```powershell
pwsh -File scripts/verify.ps1                 # 38 önteszt + check-repo
pwsh -File scripts/build.ps1                  # csak a két bináris
pwsh -File scripts/dist.ps1 -Release          # hordozható példány + parancsikon
build/apps/desktop-shell/antikolt-shell.exe --profiletest build/apps/engine-host/antikolt-engine-host.exe
```

Ha bármelyik lépés nem azt mondja, amit ez a lap állít, **ez a lap a hibás** — a napló és a kapu a valóság.

