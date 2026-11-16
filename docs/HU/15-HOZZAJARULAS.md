# 15 — Közreműködés és biztonsági kormányzás

> A felhasználói elvárás: **„bárki szabadon közreműködhessen, mert egy böngészőnél lehetnek biztonsági
> kockázatok."** Ez a dokumentum ezt fordítja le működő szabályokra: hogyan lehet becsatlakozni, mi a
> review-folyamat, és hogyan kezeljük a biztonsági kockázatokat anélkül, hogy bürokráciát építenénk.

## 1. Ki közreműködhet, és hogyan?

**Bárki, bármikor, külön engedély nélkül.** A licenc (MPL-2.0) és a hozzájárulási modell (DCO) ezt
biztosítja:

| Lépés | Mit kell tennie |
|---|---|
| 1. Kód | fork → branch → commit. Új fájloknál `SPDX-License-Identifier: MPL-2.0` fejléc. |
| 2. Hozzájárulás | `git commit -s` (Signed-off-by) — egy sor, ami igazolja, hogy jogosult vagy a kódot beadni. **Nincs CLA, nincs aláírás, nincs regisztráció.** |
| 3. Beküldés | pull request a sablonnal (mi változott, hogyan tesztelted, érint-e biztonsági területet). |
| 4. Review | a maintainer (jelenleg a projektgazda + az asszisztens) átnézi; automatizált kapuk futnak. |
| 5. Beolvasztás | zöld CI + jóváhagyás után. |

**Amit nem kérünk:** nem kérünk szerzői jog átadást (CLA), nem kérünk céges jóváhagyást, nem kérünk
pénzt. A közreműködő megtartja a szerzői jogát, a kód MPL-2.0 alatt marad.

## 2. Biztonsági kormányzás (a „böngésző = kockázat" miatt)

Egy böngészőben a hiba = a felhasználó kockázata. Ezért a folyamat nem a bizalmon, hanem a **kapukon**
alapul:

### 2.1. Védett területek (CODEOWNERS)

Ezekben a mappákban a változtatáshoz **maintainer-jóváhagyás kötelező**, és nem lehet „gyorsítással"
beolvasztani:

```
/crates/antikolt-core/src/vault/     # széf, kulcskezelés, kriptográfia
/crates/antikolt-core/src/ipc/       # motorház-protokoll, üzenet-validáció
/crates/antikolt-core/src/snapshot/  # pillanatkép-író/olvasó (nem megbízható bemenet!)
/crates/antikolt-core/src/db/migrations/
/apps/desktop-shell/src/platform/    # ablakbeágyazás, sandbox-konfiguráció
/proto/                              # IPC-séma
```

### 2.2. Automatikus kapuk (minden PR-en)

| Kapu | Mit fog meg |
|---|---|
| Fordítás + teljes tesztkészlet | alaphibák |
| **Fuzz-teszt** a nem megbízható bemenetekre (MHTML-parser, IPC-üzenetek, snapshot-manifest) | összeomlás, memóriahiba, végtelen ciklus |
| **Mért memória-regresszió** (a `perf/` mérőpad rövid szcenáriója) | a memória-ígéret csendes romlása |
| Statikus elemzés (clippy, clang-tidy, `cargo-audit`, `cargo-deny`) | ismert sebezhető függőség, gyanús minta |
| Titok-szkennelés | véletlenül bekerült kulcs |
| Licenc-fejlécek ellenőrzése (SPDX) | jogi tisztaság |

### 2.3. Ami nem megbízható bemenet (és ezért külön figyelmet kap)

1. **Bármely weboldal** — a motor sandboxa véd, de a mi motor-adapterünk és IPC-nk a határ.
2. **A pillanatkép-fájlok** — lemezről visszaolvasott MHTML/manifest: sérülhet, módosulhat, rosszindulatú
   lehet (pl. ha valaki aláírás nélküli fájlt csempész a blob-tárba). → séma-validáció + integritás-hash +
   fuzz.
3. **A kezdőlap** — a felhasználó saját HTML-je: szigorú CSP, allowlist bridge, sanitizálás.
4. **A felhasználó által importált adatok** (jelszó-CSV, könyvjelző-HTML) — soha nem futtatunk kódot
   belőlük, csak adatként kezeljük.

### 2.4. Sebezhetőség-bejelentés (SECURITY.md)

- **Privát csatorna** a bejelentésre (GitHub Security Advisory, e-mail a honlapon).
- **72 órás visszaigazolás**, 30 napos javítási cél a kritikus hibákra.
- **Koordionált nyilvánosságra hozatal**: a javítás után közös bejelentés, a bejelentő nevével (ha kéri).
- **Nincs jutalom (bounty)** — nincs rá keret; helyette köszönet a kiadási jegyzetekben.
- A kiadási jegyzet minden biztonsági javítást külön felsorol.

### 2.5. Kiadási biztonság

- A kiadásokat **Ed25519/minisign** aláírt manifest védi (a frissítő fail-closed ellenőrzi) — lásd
  `14-KODALAS.md`.
- A csomagok **SHA-256** hash-e a honlapon és a GitHub Release-ben.
- **SLSA/Sigstore provenance** a buildről (automatizált, ingyenes) — bizonyítja, hogy a bináris a
  nyilvános forrásból, a nyilvános CI-ben készült.
- Visszahívás (rollback): a frissítő tudjon „előző verzióra visszaállni" és egy sérült kiadást
  visszavonni (kill switch a manifestben).

## 3. Hogyan kezeljük az AI-közreműködést?

A projekt kódját AI-ágens írja (ADR-021), és a felhasználói kód is érkezhet AI-segítséggel. Ez nem baj,
de **láthatónak kell lennie**:

- Minden fájlban `SPDX-License-Identifier: MPL-2.0`.
- A `NOTICE` kimondja: „a kód egy részét AI-eszközökkel állították elő, a szerző átnézte és tesztelte".
- Az `AUTHORS` megnevezi az emberi közreműködőket és a fejlesztőeszközt; AI-t nem listázunk „szerzőként".
- PR-sablonban egy kérdés: „készült-e AI-segítséggel, és mely része?" — nem tiltás, hanem átláthatóság.
- A fejlécek nem állítanak kizárólagos emberi szerzőséget.

Ez a gyakorlat védi a projektet (proveniencia) és a közreműködőt (nem kell a szerzőségét bizonygatnia).

## 4. Amit szándékosan NEM csinálunk

- **Nincs CLA** — a közreműködés ingyenes és azonnali; ennek ára, hogy a licencet később nem lehet
  egyoldalúan megváltoztatni (lásd `13-LICENC.md`).
- **Nincs kötelező kéttagú review** — egy személyes projektnél ez megbénítja a fejlődést; helyette
  védett területek + automatizált kapuk + a maintainer felelőssége.
- **Nincs „security through obscurity"** — a kód nyílt, a fenyegetésmodell is nyílt (`02` 14. fejezet).
- **Nincs csendes változtatás** a biztonsági viselkedésen — minden ilyen ADR-t kap.

## 5. A következő lépés

A `CONTRIBUTING.md`, `SECURITY.md`, `CODEOWNERS`, PR- és issue-sablonok, valamint a fuzz-célok
a **v0.1 kódbázissal együtt** készülnek el (F1 elején). Ez a dokumentum a szabály; a fájlok a megvalósítás.
