# Antikolt

**Modern, memóriatakarékos, kártya-alapú böngésző.** Nyitott forrás, magyar és angol nyelven.

> Állapot: **v0.8 — a hibernálási ciklus bezárva**. Mentés: MHTML (DOM + beágyazott erőforrások) **és**
> állapot (görgetés, horgony, űrlapok, `sessionStorage`, média-pozíció, jelszavak nélkül); a motor
> **61 MB RAM-ot ad vissza**. Visszaállítás: a mentett MHTML **offline** (`file://`) betöltve, az állapot
> visszaírva — ellenőrizve: `scrollY=600`, `fields=18`, a cikk szövege megjelenik, hálózat nélkül.
> A Rust mag 79 tesztje zöld.
> Terv: `docs/HU/` (magyar) · angol összefoglaló: `docs/EN/` · napló:
> [`docs/HU/16-FEJLESZTESI-NAPLO.md`](docs/HU/16-FEJLESZTESI-NAPLO.md) · F0 mérés:
> [`docs/HU/18-F0-JELENTES.md`](docs/HU/18-F0-JELENTES.md) · eszközlánc:
> [`docs/HU/17-ESZKOZLANC.md`](docs/HU/17-ESZKOZLANC.md).

---

## Mit tud ma

| Réteg | Állapot |
|---|---|
| `crates/antikolt-ipc` | 20 üzenet, hossz-előtagos keretezés, verzió- és hibakezelés, képesség-lekérdezés — [specifikáció](proto/README.md) |
| `crates/antikolt-core` | azonosítók, óra, öt erőforrás-profil, kártya-életciklus-gép, `keep`/`sweep`/`wipe` munkamenet-politika, pillanatkép-manifest, migrációk |
| `apps/desktop-shell` | Qt 6 ablak, vezérlősáv, négy téma, `--selftest` — **lefordul és fut** |
| `apps/engine-host` | **protokoll-szerver** a CEF felett: `hello`/`welcome`, `create_surface`, `navigate`, `destroy_surface`, `snapshot`, `capture`, `clear_origin_data`, `metrics`, `set_media`, `scroll`, `set_zoom`, `bridge_reply`, `shutdown` |
| `crates/antikolt-ipc` + `antikolt-probe` | stdio-kliens (`EngineLink`) és end-to-end próba — `--freeze` móddal a teljes fagyasztási ciklus |
| `perf/measure-freeze.ps1` | fagyasztás-mérés: **mennyi RAM-ot ad vissza** a kártya lemezre mentése |
| `perf/measure-engine.ps1` | memóriamérő: a motor folyamatfájának privát memóriája, markdown + JSON riport |
| `home/` | a kezdőlap: `<antikolt-card>`, virtualizált rács, négy téma, keresés, billentyűzet — böngészőben önállóan is megnyitható |
| Shell ↔ motor | **kész és mérve**: IPC, fagyasztás/hibernálás, előnézet, memória-mérés, oldal-téma, nagyítás, görgetés — `scripts/verify.ps1` 27 öntesztje |

## Kipróbálás (Windows, ma)

```powershell
pwsh -File scripts/build.ps1                     # shell + motorház
cargo build -p antikolt-core-service            # a mag (az előnézethez és a panelhez kell)
.\build\apps\desktop-shell\antikolt-shell.exe    # a böngésző

# vagy: hordozható mappa + asztali parancsikon (nem telepítő)
pwsh -File scripts/dist.ps1
```

Indítás után **magától elindul a motor** (a „Motor indítása" gomb csak akkor kell, ha leállt), megjelenik a kártyarács, és egy kártyára kattintva
megnyílik az oldal. Ami kipróbálható:

- **kártyák**: nyitás, bezárás (haza), pin, jegyzet (a kártyán, helyben szerkeszthető), előnézet-kép;
- **vezérlősáv**: négy téma, oldal-téma (auto/sötét/világos), nagyítás (`−` / `🔍 100%` / `+`), munkamenet-panel;
- **munkamenet-panel**: a napló sorai, „mennyi veszne el" (`3 süti · 5.0 kB`), a 24 órás szabály állítása
  (óra + app-zárnál törlés) és a **Takarítás most** gomb;
- **olvasóhely**: a kártyát elhagyva és újranyitva ugyanott folytatja (`scroll.json`).

**Őszinte korlátok:** egyszerre **egy** élő kártya van (I1, ez a memória-ígéret); a fejlesztői build
`--no-sandbox`-szal fut; az előnézet best-effort (egy elakadt felvétel kimaradhat); Linux/Wayland (F7), a
széf/passkey és a csomagolás (F8) még nincs.
## Fejlesztés

Előfeltételek: **Rust 1.80+**, **Qt 6** és **CEF** a `D:\Tools` alatt (telepítés:
[`docs/HU/17-ESZKOZLANC.md`](docs/HU/17-ESZKOZLANC.md)), MSVC Build Tools, Node 22 a kezdőlaphoz.

```powershell
# natív build (shell + motorház) + öntesztek
pwsh -File scripts/build.ps1
pwsh -File scripts/build.ps1 -CheckOnly      # csak az eszközlánc

# Rust mag
cargo test --all          # 77 teszt
cargo clippy --all-targets -- -D warnings
cargo fmt --all -- --check

# kezdőlap
node --check home/app.js

# minden, amit a CI is futtat (SPDX, linkek, higiénia, JS, Rust)
pwsh -File scripts/check-repo.ps1
```

Ha a gép nem ér el crates.io-t (a projekt első gépe így volt), a helyi registry-cache-ből
`cargo test --offline` működik. A hozzájárulás szabályai: [`CONTRIBUTING.md`](CONTRIBUTING.md);
sebezhetőség-bejelentés: [`SECURITY.md`](SECURITY.md).

## Mi ez?

Antikolt nem fül-alapú böngésző. A megnyitott oldalak nem élnek a háttérben: minden mentett oldal egy
**kártya** a kezdőlapon, ami maga is egy szerkeszthető, weboldal formátumú felület (nem könyvjelző-sáv).
Egyszerre **legfeljebb egy** webes motor fut, a többi kártya állapota **lemezen** pihen, nem RAM-ban.

Három pillér:

1. **Munkamenet-gazdálkodás** — identitás-konténerek (fiókonként izolált sütik/tárolók), és a fontos
   bejelentkezések (Google, Facebook, saját oldal) **életben tartása 0 RAM-mal**: a süti lemezen van, nem
   memóriában. Mérhető nyilvántartás mutatja, mi van életben, mennyi helyet foglal, és mit veszítenénk a
   törlésével. Beépített jelszószéf + TOTP + automatikus kitöltés.
2. **Cserélhető böngészőmotorok** — a motor egy külön folyamatfa, adapteren keresztül csatlakozik;
   az első a Chromium (CEF), később WebKitGTK (Linux) / WebView2 (Windows).
3. **Lemezhibernálás** — a kártya elhagyásakor a teljes oldalállapot (DOM, űrlapok, görgetés, tárolók)
   lemezre íródik, a motor megszűnik. Visszatéréskor azonnali, offline is olvasható visszaállítás.

## Célok

| Mérőszám | Cél |
|---|---|
| Teljes böngésző, 1 élő oldallal | **≤ 1 GB** (nehéz oldalnál is) |
| Kártyarács nézet, motor nélkül (ultra-low mód) | **≤ 180 MB** |
| Kártyaváltás (hideg kártya megnyitása) | ≤ 600 ms érzékelt |
| Lemezhasználat | korlátlanul tervezett, kvótával (több tíz GB is rendben) |

Részletek: [`docs/HU/03-MEMORIA.md`](docs/HU/03-MEMORIA.md).

## Dokumentumok

| Fájl | Tartalom |
|---|---|
| [`docs/HU/00-JAVASLATOK.md`](docs/HU/00-JAVASLATOK.md) | A Gemini-terv bírálata, javaslatok, kockázatok |
| [`docs/HU/01-KERDESEK.md`](docs/HU/01-KERDESEK.md) | Nyitott kérdések (javasolt válasszal) |
| [`docs/HU/02-ARCHITEKTURA.md`](docs/HU/02-ARCHITEKTURA.md) | A terv: folyamatok, életciklus, adatmodell, széf, témák |
| [`docs/HU/03-MEMORIA.md`](docs/HU/03-MEMORIA.md) | Memória-büdzsé, mérési módszer, CI-kapuk |
| [`docs/HU/04-UTEMTERV.md`](docs/HU/04-UTEMTERV.md) | Fázisok, elfogadási kritériumok, erőforrás |
| [`docs/HU/05-DONTESEK.md`](docs/HU/05-DONTESEK.md) | Döntési napló (ADR) |
| [`docs/HU/06-OTLETEK.md`](docs/HU/06-OTLETEK.md) | Ötletlista a követelményeken túl |
| [`docs/HU/07-FOGALMAK.md`](docs/HU/07-FOGALMAK.md) | Fogalomtár (magyar–angol) |
| [`docs/HU/08-DIZAJN.md`](docs/HU/08-DIZAJN.md) | Design tokenek, négy téma, kártya-anatómia |
| [`docs/HU/09-CELHARDVER.md`](docs/HU/09-CELHARDVER.md) | Célgép (VECTOR) mérései, erőforrás-profilok |
| [`docs/HU/10-MUNKAMENET.md`](docs/HU/10-MUNKAMENET.md) | Bejelentkezések életben tartása, költség-nyilvántartás, „mit veszítünk" |
| [`docs/HU/11-OSR.md`](docs/HU/11-OSR.md) | Natív ablak vs offscreen renderelés (miért melyik) |
| [`docs/HU/12-PASSKEY.md`](docs/HU/12-PASSKEY.md) | Passkey/WebAuthn: miért nehéz CEF-fel, mit teszünk |
| [`docs/HU/13-LICENC.md`](docs/HU/13-LICENC.md) | Licenc, Qt LGPL, CEF BSD, DCO, AI-szerzőség |
| [`docs/HU/14-KODALAS.md`](docs/HU/14-KODALAS.md) | Kódaláírás: mi ingyenes, mi nem, SmartScreen |
| [`docs/HU/15-HOZZAJARULAS.md`](docs/HU/15-HOZZAJARULAS.md) | Közreműködés (DCO) és biztonsági kormányzás |
| [`docs/HU/16-FEJLESZTESI-NAPLO.md`](docs/HU/16-FEJLESZTESI-NAPLO.md) | Fejlesztési napló: mi készült el, hogyan lett ellenőrizve, mi blokkol |
| [`docs/HU/17-ESZKOZLANC.md`](docs/HU/17-ESZKOZLANC.md) | Eszközlánc: Qt 6, CEF, MSVC, build és a négy buktató |
| [`docs/HU/18-F0-JELENTES.md`](docs/HU/18-F0-JELENTES.md) | **F0 mérés**: mit mérünk, mennyi, mit jelent, milyen fenntartásokkal |
| [`proto/README.md`](proto/README.md) | A motorház–mag protokoll v1 írott specifikációja |
| [`perf/README.md`](perf/README.md) | A mérési mérőpad terve és elfogadási küszöbei |
| [`docs/forras/`](docs/forras/) | A kiinduló Gemini-terv (ODT + szövegkivonat) és a kutatási jegyzetek |

## Repó szerkezet

```
crates/antikolt-ipc/     a motorház–mag protokoll (üzenetek + keretezés)
crates/antikolt-core/    domain logika (életciklus, munkamenet, manifest, migrációk)
crates/antikolt-core/migrations/0001_init.sql
home/                    a kezdőlap (a böngésző arca)
proto/                   az IPC írott specifikációja
perf/                    mérőpad (terv + küszöbök)
docs/{HU,EN,forras}      terv, angol összefoglaló, források
```

## Licenc

**MPL-2.0** — a teljes szöveg: [`LICENSE`](LICENSE); a harmadik felek és az AI-segítség nyilatkozata:
[`NOTICE`](NOTICE). Döntés: [`docs/HU/05-DONTESEK.md`](docs/HU/05-DONTESEK.md) (ADR-014), részletek:
[`docs/HU/13-LICENC.md`](docs/HU/13-LICENC.md).

## Hozzájárulás

**Bárki közreműködhet, CLA nélkül** — egy `Signed-off-by` sor (DCO) kell a commithoz. A biztonsági
szempontból védett kódrészek és a folyamat: [`CONTRIBUTING.md`](CONTRIBUTING.md),
[`SECURITY.md`](SECURITY.md), [`docs/HU/15-HOZZAJARULAS.md`](docs/HU/15-HOZZAJARULAS.md).
