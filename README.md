# Antikolt

**Modern, memóriatakarékos, kártya-alapú böngésző.** Nyitott forrás, magyar és angol nyelven.

> Állapot: **v0.1 alapozás**. A Rust mag (IPC-szerződés, életciklus-gép, munkamenet-politika,
> pillanatkép-manifest, migrációk) és a kezdőlap elkészült és tesztelt — **77 zöld teszt**, tiszta
> clippy. A Qt + CEF shell még nem épült meg (eszközbeszerzés: MSVC/CMake/Qt 6/CEF).
> A terv a `docs/HU/` alatt olvasható, magyarul; angol összefoglaló: `docs/EN/`.
> Fejlesztési napló: [`docs/HU/16-FEJLESZTESI-NAPLO.md`](docs/HU/16-FEJLESZTESI-NAPLO.md).

---

## Mit tud ma

| Réteg | Állapot |
|---|---|
| `crates/antikolt-ipc` | 20 üzenet, hossz-előtagos keretezés, verzió- és hibakezelés, képesség-lekérdezés — [specifikáció](proto/README.md) |
| `crates/antikolt-core` | azonosítók, óra, öt erőforrás-profil, kártya-életciklus-gép, `keep`/`sweep`/`wipe` munkamenet-politika, pillanatkép-manifest, migrációk |
| `home/` | a kezdőlap: `<antikolt-card>`, virtualizált rács, négy téma, keresés, billentyűzet — böngészőben önállóan is megnyitható |
| `apps/desktop-shell` | **még nincs** — a következő mérföldkő (CMake + Qt 6 + CEF kell hozzá) |

## Fejlesztés

Előfeltételek: **Rust 1.80+**, linker (Windows: MSVC Build Tools; Linux: `cc`), és a kezdőlaphoz Node 22
(csak szintaxis-ellenőrzéshez).

```sh
cargo test --all          # 77 teszt
cargo clippy --all-targets -- -D warnings
cargo fmt --all -- --check
node --check home/app.js
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
