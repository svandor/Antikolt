# 21 — Hol tart a termék (mért állapot)

> Ez a lap **nem terv**: azt rögzíti, ami **mérve** működik, mi van nyitva, és mi az, amihez emberi lépés kell.
> A mért állapot forrása a kapu: `pwsh -File scripts/verify.ps1` → **ALL SELF TESTS PASSED**, és a napló
> (`16-FEJLESZTESI-NAPLO.md`) szeletei, ahol minden szám egy tényleges futás kimenete.

## A kapu, egy pillantással

```
build + selftest  ok        (CMake + MSBuild + Qt, mindkét bináris elkészül)
core service      ok        (a mag válaszol: hello, cards_list)
40 önteszt        ok        (lásd alább, mit bizonyít mindegyik)
check-repo        ok        (SPDX, linkek, JS-parse, cargo fmt, clippy -D warnings, 78+17+2+1+15 Rust-teszt)
```

A hordozható példány: `scripts/dist.ps1 -Release` → `dist/Antikolt/` (443.9 MB), asztali és Start-menü
parancsikonnal; a telepített binárison a `--profiletest`, `--workspacetest`, `--containertest`, `--tagtest`
külön is lefutott (`exit=0`), mert a `dist` ugyanaz a kód, de nem ugyanaz a fájl.

## Ami mérve kész (és mit bizonyít a teszt)

| Terület | Teszt | Mit bizonyít (mért kimenet) |
|---|---|---|
| Rács, 500 kártya | `--gridscaletest` | 503 kártya, **13** renderelt DOM-elem, 20 ms az első renderig; a görgetéses ugrás `not-measurable` (nincs layout-magasság a harnessben) |
| Keresés | `--searchtest`, `--tagtest` | ékezet-független cím/webcím/jegyzet/címke, és az **oldalszöveg** is (`search_page_text=ok`) |
| Konténerek | `--containertest` | színes pont, választó a kártyán, chip-legend + szűrő, tábla a magban, `＋`/dupla kattintás/`×`, használt konténer törlése **áthelyezéssel** |
| Munkaterületek | `--workspacetest` | fül-sor a mag `workspaces.json`-jából, a kártya `workspace` mezője tárolt adat, fül-életciklus, kártya-választó |
| Címkék | `--tagtest` | címke a kártyán, mag-normalizálás (trim/dedup/12×32), kereshető |
| Kezdőlap | `--addcardtest` | `＋` + soros űrlap: a kártya a magba kerül (cím/URL/munkaterület ellenőrizve), és a `cards.delete`-tel eltűnik; a teszt kitakarít maga után |
| Profil-költöztetés | `--profiletest` | export → import friss profilba → visszaolvasás **fájlból**; ismeretlen séma elutasítva |
| Munkamenetek | `--sessionpanel`, `--scrollbook`, `--wipetest`, `--cleanuptest` | látszik, mi van életben, mennyi RAM/lemez; `keep`/`sweep`/`wipe` végrehajtás |
| Előnézet | `--preview*` | a háttér-worker szabályai (ADR-041) és az ütemező mérve |
| Memória | `--metricstest` | az engine working set-je mérve, a költségvetés nem kitalált |
| Tálca, napló, téma | `--traytest`, `--logtest`, `--themetest` | a napló nem ír feleslegesen, a témák a helyes színt adják |

## Ami nyitva van (tudatosan, nem elfelejtve)

1. **A kártya szerkesztése** — létrehozás (`＋`, ADR-061), törlés, **átnevezés** (ADR-062) és **webcím-átírás**
   (ADR-063) működik a felületen, mind mérve (`wrote_by=gesture`, `edit_card_url=ok`). Nyitott: a törlés
   megerősítése, és hogy a webcím-váltás után a rögzített oldalszöveg a régi tartalom marad.
2. **SQLite-store csere** — a négy store ma JSON; a `0002` migráció (`page_text`, `tags`, `workspace`) még nincs
   lefuttatva, mert a JSON-additív mezők eddig mindent elbírtak.
3. **Konténerenkénti engedély/proxy (F3)** — a konténer ma szűrő és címke, nem izolációs határ.
4. **OAuth/SSO popup** — a mechanizmus kész (I1-kivétel, 60 s), a bejelentkezési folyamat nincs összekötve.
5. **A köteg felületi gombja** — a mag, a híd (`profile.export/import`) és a lap-út (`antikoltProfileWrite/Read`)
   kész és mérve (ADR-059/060); a **natív fájlválasztó** hiányzik, mert a harness nem tud dialógust kattintani —
   ez a lépés önmagában `not-measurable`.
6. **A profil-köteg titkosítása** — a köteg munkamenet-sorokat is tartalmaz; ez valódi döntés, nem kényelmi
   kérdés.
7. **Fogd-és-vidd a munkaterületekhez** — a `<select>` mellett, nem helyette (ADR-058).

## Amihez emberi lépés kell (nem kód)

- **Aláírás:** SignPath Foundation (nyílt forrású kódaláírás) és/vagy Store — a kulcs nem a mi gépünkön van
  (`20-ALASAS.md`).
- **Passkey/vault:** `cargo vendor` hálózat nélkül nem megy ebben a környezetben; a CDP-s passkey-kísérlet
  eredménye `12-PASSKEY.md`-ben.
- **Kiadási workflow:** a GitHub Actions kiadás még soha nem futott (nincs aláírt artefaktum).

## Hogyan ellenőrizhető mindez öt perc alatt

```powershell
pwsh -File scripts/verify.ps1                 # 40 önteszt + check-repo
pwsh -File scripts/build.ps1                  # csak a két bináris
pwsh -File scripts/dist.ps1 -Release          # hordozható példány + parancsikon
build/apps/desktop-shell/antikolt-shell.exe --profiletest build/apps/engine-host/antikolt-engine-host.exe
```

Ha bármelyik lépés nem azt mondja, amit ez a lap állít, **ez a lap a hibás** — a napló és a kapu a valóság.
