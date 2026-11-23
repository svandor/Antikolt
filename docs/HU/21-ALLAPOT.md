# 21 — Hol tart a termék (mért állapot)

> Ez a lap **nem terv**: azt rögzíti, ami **mérve** működik, mi van nyitva, és mi az, amihez emberi lépés kell.
> A mért állapot forrása a kapu: `pwsh -File scripts/verify.ps1` → **ALL SELF TESTS PASSED**, és a napló
> (`16-FEJLESZTESI-NAPLO.md`) szeletei, ahol minden szám egy tényleges futás kimenete.

## A kapu, egy pillantással

```
build + selftest  ok        (CMake + MSBuild + Qt, mindkét bináris elkészül)
core service      ok        (a mag válaszol: hello, cards_list)
74 ellenőrzés     ok        (71 önteszt + build + core service + check-repo; lásd alább, mit bizonyít mindegyik)
check-repo        ok        (SPDX, linkek, JS-parse, cargo fmt, clippy -D warnings, 93+67+11+1+15 Rust-teszt)
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
| Tároló (négy store + felhasználói beállítások) | Rust-tesztek, `--addcardtest` | mind a négy store **SQLite-sorok** (`user_version=7`, WAL): kártyák, konténerek, munkaterületek, munkamenetek, `user_prefs` egyetlen sortal (`CHECK (id = 1)`); a négy `*.json` származtatott export; `db_status=ok schema=7 … verified=yes` mind a négyet soronként méri; a kapcsoló előtti profil store-onként egyszer átvételre kerül (ADR-070, napló 97) |
| Felhasználói beállítások | Rust-tesztek (`sqlite_store::tests`, `service.rs`), `--prefstest` | `UserPrefs` default (`max_live_cards=5`, `idle_hibernate_ms=600_000`, `cold_card_ms=86_400_000`, `memory_budget_mb=512`, `resource_profile=Balanced`), validáció; IPC `user_prefs`/`user_prefs_set`, `profile_export`/`profile_import`; a shell ⚙ Beállítások dokkja szerkeszti (profil-lenővel); a profil az ADR-020 `ProfileKind`-je (séma 0009–0010); a `user_prefs` válasz a levezetett `profile`-t is viszi, a shell `preview_refresh`-t és `engine_warm_idle_shutdown_ms`-t (T1/T2) cache-el |
| Keresés | `--searchtest`, `--tagtest` | ékezet-független cím/webcím/jegyzet/címke, és az **oldalszöveg** is (`search_page_text=ok`) |
| Konténerek | `--containertest` | színes pont, választó a kártyán, chip-legend + szűrő, tábla a magban, `＋`/dupla kattintás/`×`, használt konténer törlése **áthelyezéssel**, proxy/engedély-szerkesztő a rácson (`container_policy=ok`), partíció-takarítás törléskor (`container_partition_cleanup=ok`) |
| Munkaterületek | `--workspacetest`, `--workspacedragtest` | fül-sor a magban tárolt munkaterület-táblából, a kártya `workspace` mezője tárolt adat, fül-életciklus, kártya-választó; fogd-és-vidd a fülre (`draggable` kártya → `drop` a fülre → `setWorkspace`, ADR-058; a `<select>` marad a billentyűzetes út) |
| Címkék | `--tagtest` | címke a kártyán, mag-normalizálás (trim/dedup/12×32), kereshető |
| Kezdőlap | `--addcardtest` | `＋` + soros űrlap: a kártya a magba kerül (cím/URL/munkaterület ellenőrizve), átnevezés a cím-szerkesztő `Enter`-jével (`wrote_by=gesture`), webcím-átírás (`edit_card_url=ok`), tároló-ellenőrzés (`db_status=ok schema=7 … verified=yes`), kétlépéses törlés (`delete_armed=ok delete_card=ok`, ADR-068); a teszt kitakarít maga után |
| `<antikolt-card>` csomag | `--gridtest`, `--addcardtest`, `--containertest` | a komponens önálló `home/antikolt-card.js`-ben (ADR-075), a rács és egy felhasználói lap ugyanezt tölti; `home/user-home.example.html` minimális példa; a konténer/munkaterület `<select>` a `setOptions`-ból; a kapu minden kártya-interakciós tesztje zöld |
| Szerkeszthető kezdőlap (tárolás+kiszolgálás) | `--customhometest`, `--resethometest` | a mag adatkönyvtárában lévő `home.html` felülírja a beépített rácsot (`ANTIKOLT_HOME` → `<coreData>/home.html` → `home/index.html`); a beállítás-dokk „Egyéni kezdőlap visszaállítása" gombja törli és visszatölti a rácsot (`reset_grid_returned=true`) |
| Profil-költöztetés | `--profiletest` | export → import friss profilba → visszaolvasás **fájlból**; ismeretlen séma elutasítva |
| Munkamenetek | `--sessionpanel`, `--scrollbook`, `--wipetest`, `--cleanuptest` | látszik, mi van életben, mennyi RAM/lemez; `keep`/`sweep`/`wipe` végrehajtás; a „Politika" oszlop legördülővel **kártyánként (origin-szinten)** felülírja a konténer-alapértelmezést (`session_panel=ok`) |
| Bejelentkezési popup, profil-gomb, fogd-és-vidd | `--partitiontest`, `--profiletest`, `--workspacetest` | popup (ADR-073): gesztus → valódi ablak a kártya partíciójában, 60 s, a hibernálás felfüggesztve (a gesztus-út `not-measurable`); profil-gomb: natív `QFileDialog` a `profile_export`/`import` fölött (a dialógus `not-measurable`, az út `--profiletest`-tel mérve); fogd-és-vidd: kártya → munkaterület-fül a `<select>` mellett (ADR-058) |
| Előnézet | `--preview*`, `--previewhashtest` | a háttér-worker szabályai (ADR-041) és az ütemező mérve; a preview-név a tartalom hash-e (`previewContentName`, ADR-077 — Qt `QCryptographicHash::Sha256` a shellben), a dedup a nevezésből adódik (`preview_content_addressed=ok` / `worker_content_addressed=ok`) |
| Memória | `--metricstest`, `--memorytest`, `--prefstest`, `--memorypressuretest`, `--memorypressuree2etest`, `--memoryhibernatetest`, `--memorywarnpausetest`, `--videocaptest`, `--memoryreport` | az engine working set-je mérve (`engine_working_set_bytes`), a büdzsé a profil `memory_budget_mb`-je (`budgetBytes`); a rács used/budget sávot rajzol, és `mem-over`/`mem-warn`/`mem-hard` jelzéssel mutatja a nyomást (a híd átadja a `overBudget`/`pressure`-t, 126. szelet); a kártya `memory_bytes`-e a fagyasztáskori working set (séma 0008); a `system.memory` `overBudget` jele a médiaminőség-korlát alapja, és a profil `warn`/`hard` sávjaiból `pressure` (normal/warn/hard) is számítódik (ADR-020, 124. szelet), és a `hard` sáv elérése hibernálja az élő kártyát, a `warn` sáv pedig szünetelteti a preview-munkást (ADR-020, 127–128. szelet); a profil `video_max_height`-je **megjelenítési sapkaként** a top-level `video`-ra kerül (`set_video_cap`, ADR-076, `--videocaptest`: Fast 2160px → Saver 720px → Manual eltávolítva), a dekódolási felbontás `not-measurable`; a `--memoryreport` a teljes vezérlő-állapotot (profil + büdzsé + working set + kártyánkénti memória + nyomás) egyben írja |
| Tálca, napló, téma, nyelv | `--traytest`, `--logtest`, `--themetest`, `--languagetest`, `--gridlangtest` | a napló nem ír feleslegesen, a témák a helyes színt adják; a nyelv futásidőben váltható (ADR-078/079): a shell chrome-ja (`ui/language`, `setLanguage`), a rács (`data-language`, `pushLanguageToGrid`, `home/app.js` `t()`), a kártya inline címkéi + a statikus címkék (`antikolt-card.js` `t()`, `applyStaticLabels`) ÉS a shell státuszüzenetei (`setStatus` → `t()`) HU/EN átcímkéződnek — a HU/EN felület kész |
| Nyomtatás (mentés PDF-be) | `--printtest` | a motor a saját kompozitorával (`PrintToPDF`) írja a PDF-et; a shell `🖨️ PDF` gombja natív fájldialógust nyit (a dialógus `not-measurable`, az utat a `--printtest` méri — F7 nyitány) |
| Keresés az oldalon (Ctrl+F) | `--findtest` | a motor `Find`/`CefFindHandler` útján keresi a szöveget és a találatszámot `find_result` eseménnyel jelenti; a shell Ctrl+F keresősávot mutat (Enter = következő, Shift+Enter = előző, Esc = bezárás) |
| Letöltés | `--downloadtest` | a motor `StartDownload`/`CefDownloadHandler` útján tölti le a fájlt, és a kész állapotot (útvonal + siker) `download_finished` eseménnyel jelenti |
| Összeomlás utáni helyreállítás | `--crashtest` | a motor `renderer_terminated` eseménnyel jelenti a renderer kilépését; a shell az összeomlott felületet azonnal újratölti (a rács a kezdőlapra, a kártya a saját URL-jére tér vissza) |
| PDF megtekintés | `--pdftest` | a PDF a CEF beépített PDFium-nézőjében nyílik (betöltés, nem letöltés) |
| Akadálymentesség | `--accessibilitytest` | minden interaktív vezérlő bemondható (szöveg vagy accessible name), a Ctrl+F gyorsbillentyű regisztrálva |

## Ami nyitva van (tudatosan, nem elfelejtve)

1. **A profil-köteg titkosítása** — a köteg munkamenet-sorokat is tartalmaz; ez valódi döntés, nem kényelmi
   kérdés.
2. **A bejelentkezési popup gesztus-útja** — a motor összekötve (ADR-073: gesztus → valódi ablak a kártya
   partíciójában, 60 s, a hibernálás felfüggesztve), de a gesztus-út a harnessben nem mérhető (a szintetikus
   `window.open` nem gesztus): a `popup_requested`/`popup_closed` útja `not-measurable`.
3. **A teljes szerkeszthető kezdőlap** — a `<antikolt-card>` csomag (ADR-075), a felhasználói HTML
   tárolása+kiszolgálása, a visszaállítás, a **kiinduló sablon + szerkesztés** és a **sandboxolás** kész (106., 111.,
   117., 118. szelet); a felhasználói HTML a hídon keresztül (szándékosan) éri el a kártya-műveleteket, a közvetlen
   fájl-olvasás (`fetch`/`XHR`) bizonyítottan zárt.
4. **Az F6 maradéka** — a büdzsé, a beállítás-felülete, a kártyánkénti memória-bontás, a profil-preset, a
   médiaminőség-korlát **jele**, a profil **előnézet-frissítése**, a **hangolási riport**, a **motor-élettartam
   (T1/T2) bekötése**, a **motor leállítás/újraindítás ciklusa**, a **rács raszterizálása**, a **raszter-kezdőlap
   mód**, annak **automatikus aktiválása**, a **kattintás-leképezés**, a **raszter megjelenítése + kattintás-horog**
   és az **oldalarány-megőrző illesztés** kész (ADR-074/020, 102–120. szelet), a `warn`/`hard` sávokból pedig már `pressure` jel számítódik (ADR-020, 124. szelet), és a `hard` sáv hibernálja az élő kártyát, a `warn` sáv pedig szünetelteti a preview-munkást (ADR-020, 127–128. szelet); a profil **többi dimenziójának**
   tényleges vezérlő-művelete, a `video_max_height`, **kész megjelenítési sapkaként** (ADR-076, `--videocaptest`:
   a profil értéke `video{max-height:Npx}` szabályként jut a top-level `video`-ra, a `manual` profil eltávolítja);
   a **dekódolási** felbontás-csökkentés `not-measurable` — a CEF nem ad laponkénti videó-dekódolási korlátot;
   az **automatikus profilváltás** kész (ADR-080, `--autoprofiltest`: az `Auto` mód a tápforrás + a mért nyomás
   alapján a mag `resolve_profile`-ján át `Balanced`/`Saver`/`Ultra`-ra oldódik, a shell minden minta után újra-kérdez).

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

