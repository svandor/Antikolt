# 21 — Hol tart a termék (mért állapot)

> Ez a lap **nem terv**: azt rögzíti, ami **mérve** működik, mi van nyitva, és mi az, amihez emberi lépés kell.
> A mért állapot forrása a kapu: `pwsh -File scripts/verify.ps1` → **ALL SELF TESTS PASSED**, és a napló
> (`16-FEJLESZTESI-NAPLO.md`) szeletei, ahol minden szám egy tényleges futás kimenete.

## A kapu, egy pillantással

```
build + selftest  ok        (CMake + MSBuild + Qt, mindkét bináris elkészül)
core service      ok        (a mag válaszol: hello, cards_list)
38 önteszt        ok        (lásd alább, mit bizonyít mindegyik)
check-repo        ok        (SPDX, linkek, JS-parse, cargo fmt, clippy -D warnings, 78+49+8+1+15 Rust-teszt)
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
| Munkaterületek | `--workspacetest` | fül-sor a magban tárolt munkaterület-táblából, a kártya `workspace` mezője tárolt adat, fül-életciklus, kártya-választó |
| Címkék | `--tagtest` | címke a kártyán, mag-normalizálás (trim/dedup/12×32), kereshető |
| Kezdőlap | `--addcardtest` | `＋` + soros űrlap: a kártya a magba kerül (cím/URL/munkaterület ellenőrizve), átnevezés a cím-szerkesztő `Enter`-jével (`wrote_by=gesture`), webcím-átírás (`edit_card_url=ok`), tároló-ellenőrzés (`db_status=ok schema=7 … verified=yes`), kétlépéses törlés (`delete_armed=ok delete_card=ok`, ADR-068); a teszt kitakarít maga után |
| `<antikolt-card>` csomag | `--gridtest`, `--addcardtest`, `--containertest` | a komponens önálló `home/antikolt-card.js`-ben (ADR-075), a rács és egy felhasználói lap ugyanezt tölti; `home/user-home.example.html` minimális példa; a konténer/munkaterület `<select>` a `setOptions`-ból; a kapu minden kártya-interakciós tesztje zöld |
| Szerkeszthető kezdőlap (tárolás+kiszolgálás) | `--customhometest` | a mag adatkönyvtárában lévő `home.html` felülírja a beépített rácsot (`ANTIKOLT_HOME` → `<coreData>/home.html` → `home/index.html`); a felhasználói lap a profilból töltődik (`custom_home_title=sajat kezdolap`) |
| Profil-költöztetés | `--profiletest` | export → import friss profilba → visszaolvasás **fájlból**; ismeretlen séma elutasítva |
| Munkamenetek | `--sessionpanel`, `--scrollbook`, `--wipetest`, `--cleanuptest` | látszik, mi van életben, mennyi RAM/lemez; `keep`/`sweep`/`wipe` végrehajtás; a „Politika" oszlop legördülővel **kártyánként (origin-szinten)** felülírja a konténer-alapértelmezést (`session_panel=ok`) |
| Bejelentkezési popup, profil-gomb, fogd-és-vidd | `--partitiontest`, `--profiletest`, `--workspacetest` | popup (ADR-073): gesztus → valódi ablak a kártya partíciójában, 60 s, a hibernálás felfüggesztve (a gesztus-út `not-measurable`); profil-gomb: natív `QFileDialog` a `profile_export`/`import` fölött (a dialógus `not-measurable`, az út `--profiletest`-tel mérve); fogd-és-vidd: kártya → munkaterület-fül a `<select>` mellett (ADR-058) |
| Előnézet | `--preview*` | a háttér-worker szabályai (ADR-041) és az ütemező mérve |
| Memória | `--metricstest`, `--memorytest`, `--prefstest`, `--memoryreport` | az engine working set-je mérve (`engine_working_set_bytes`), a büdzsé a profil `memory_budget_mb`-je (`budgetBytes`); a rács used/budget sávot rajzol; a kártya `memory_bytes`-e a fagyasztáskori working set (séma 0008); a `system.memory` `overBudget` jele a médiaminőség-korlát alapja; a `--memoryreport` a teljes vezérlő-állapotot (profil + büdzsé + working set + kártyánkénti memória) egyben írja |
| Tálca, napló, téma | `--traytest`, `--logtest`, `--themetest` | a napló nem ír feleslegesen, a témák a helyes színt adják |

## Ami nyitva van (tudatosan, nem elfelejtve)

1. **A profil-köteg titkosítása** — a köteg munkamenet-sorokat is tartalmaz; ez valódi döntés, nem kényelmi
   kérdés.
2. **A bejelentkezési popup gesztus-útja** — a motor összekötve (ADR-073: gesztus → valódi ablak a kártya
   partíciójában, 60 s, a hibernálás felfüggesztve), de a gesztus-út a harnessben nem mérhető (a szintetikus
   `window.open` nem gesztus): a `popup_requested`/`popup_closed` útja `not-measurable`.
3. **A teljes szerkeszthető kezdőlap** — a `<antikolt-card>` csomag (ADR-075) és a felhasználói HTML
   tárolása+kiszolgálása kész (106. szelet); a **szerkesztő-felület** (a HTML írása a termékből) és a felhasználói
   HTML **sandboxolása** marad hátra.
4. **Az F6 maradéka** — a büdzsé, a beállítás-felülete, a kártyánkénti memória-bontás, a profil-preset, a
   médiaminőség-korlát **jele**, a profil **előnézet-frissítése**, a **hangolási riport** és a **motor-élettartam
   (T1/T2) bekötése** kész (ADR-074/020, 102–110. szelet); a profil **többi dimenziójának** tényleges
   vezérlő-működése (videófelbontás, `warn`/`hard` sávok) és a **raszter-kezdőlap** (a motor tényleges leállítása +
   raszter + hideg újraindítás) marad hátra (az ütemterv 3–4 hetes tétele).

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

