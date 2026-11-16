# 01 — Kérdések és döntések

A tervezést eldöntő kérdések. **A fejlesztést az asszisztens (AI-ágens) végzi**, ezért a technikai
kérdéseket én döntöm el (indoklással), és csak azok a kérdések maradnak nyitva, amelyek **a te
preferenciádat** igénylik. Minden döntés a `05-DONTESEK.md`-ben ADR-ként is rögzítve van.

## 1. Megválaszolt kérdések (határozat)

| # | Kérdés | Döntés | Hol van kifejtve |
|---|---|---|---|
| Q1 | Ki fejleszt, milyen nyelven? | **Az asszisztens**; a fő szempont a **stabilitás és a kis erőforrásigény**. Nyelv: C++20 + Qt 6 héj, Rust mag, CEF motor | `02`, ADR-002, ADR-021 |
| Q2 | Milyen gépen kell futnia? | **VECTOR** (Ultra 9 275HX, 32 GB, RTX 5080 16 GB + Arc iGPU, 2 kijelző, 1 TB) a referencia; a termék célja 8 GB-os gépen is használhatónak lenni | `09-CELHARDVER.md` |
| Q4 | Meleg motor vagy leállított? | **Profilok** (Gyors / Kiegyensúlyozott / Takarékos / Ultra / Kézi), automatikus váltással; a mérés dönt | `09` 3. fejezet, ADR-020 |
| Q5 | Natív ablak vagy OSR? | **Windows: natív ablak; Linux/Wayland: OSR** — ugyanazon absztrakció mögött | `11-OSR.md`, ADR-022 |
| Q8 | 24 órás törlés | **`keep` / `sweep` / `wipe` politika**; a bejelentkezés alapból megmarad, mert az nem RAM-ban él | `10-MUNKAMENET.md`, ADR-009 |
| Q9 | Passkey kell-e? | **v1.0: nincs; v1.1: szoftveres passkey a széfben** (CDP-út), hardveres csak később — a CEF-nek nincs beépített WebAuthn-je | `12-PASSKEY.md`, ADR-023 |
| Q10 | Licenc | **MPL-2.0**, dinamikusan linkelt Qt, BSD-3 CEF, DCO, AI-nyilatkozat | `13-LICENC.md`, ADR-014 |
| Q14 | Kódaláírás | **SignPath Foundation** (ingyenes, OSI-licenchez) + **Ed25519/minisign** frissítés-aláírás; EV nem (2024 óta nincs előnye) | `14-KODALAS.md`, ADR-024 |

A korábbi öt kérdésre adott válaszaid (motor: CEF/Qt, lemezhibernálás, 10 perc, kezdőlap, témák) szintén
rögzítve vannak — lásd `00-JAVASLATOK.md` 0. fejezet.

## 2. Döntések (a válaszaid alapján)

| # | Kérdés | Döntés | Következmény |
|---|---|---|---|
| N1 | A 24 órás politika alapértéke | **`sweep`** marad a konténer-alap (a bejelentkezés megmarad), a `keep`/`wipe` kártyánként választható | a `02` 4.3. és ADR-009 szerint; a gyakorlatban majd finomhangoljuk |
| N2 | Szoftveres passkey elfogadása | **igen** | v1.1 passkey a széfben (B út); a hátrányok a `12-PASSKEY.md` 3.1. pontjában dokumentálva |
| N3 | Van-e passkey-only fiók? | **ellenőrizendő** — a `12-PASSKEY.md` 2.5. pontja leírja, mit jelent és hogyan nézd meg 2 perc alatt | ha van ilyen fiók: a passkey-kutatás az F0-ba kerül (a v1.0 elé) |
| N4 | Licenc | **MPL-2.0 + DCO** (a javaslatom, lásd `13-LICENC.md` 1.1.) | bárki szabadon közreműködhet, nincs CLA; a licenc később nem változtatható egyoldalúan |
| N5 | Lemezkvóta | **50 GB alap**, gépenként felülbírálható | a VECTOR-on 100 GB-ra állítható, ha kell |
| N6 | Kódaláírás kezdése | **ingyenes tanúsítvány** (SignPath Foundation) + **automatizált** frissítés (Ed25519 + Sigstore); fizetős tanúsítvány nincs a tervben | kiadásonként 1 kattintás a SignPath-jóváhagyáshoz; ha ez sem kell, az Authenticode kimarad |
| N7 | Név | **Antikolt** | domain + GitHub-szervezet lefoglalása javasolt |

### 2.1. Egyetlen teendő neked: a passkey-only ellenőrzés

A `12-PASSKEY.md` 2.5. pontja szerint nézd meg a Google- és Microsoft-fiókodban, hogy **törölted-e a
jelszót** (azaz csak passkey-jel lehet belépni). Ha igen, a passkey kutatás a v1.0 **elé** kerül; ha nem,
marad az A → B sorrend, és a v1.0 a legtöbb fiókhoz elég.

### 2.2. Amit a licencről tudni kell (a „mit jelent?" kérdésedre)

Röviden: az **MPL-2.0** mellett bárki használhatja, módosíthatja és terjesztheti a böngészőt (cég is),
**bárki közreműködhet külön engedély nélkül** (DCO-val, CLA nélkül), a módosított *fájljainkat* viszont
ugyanilyen nyíltan kell megosztania. A saját új fájljai lehetnek zártak is. A részletes, közérthető
táblázat a `13-LICENC.md` 1.1. pontjában van; a közreműködés és a biztonsági szabályok pedig a
`15-HOZZAJARULAS.md`-ben.

## 3. Amit én döntöttem el helyetted (indoklással)

Ezek technikai döntések; ha bármelyikkel nem értesz egyet, szólj, és átdolgozom.

| Téma | Döntés | Miért |
|---|---|---|
| Platform-sorrend (Q3) | Windows 11 a v1.0; Linux (Nobara/KDE/Wayland) a v0.3-tól | a Wayland-csatolás külön kockázat; a Windows-on hamarabb lesz használható termék |
| Motor-beágyazás (Q5) | Windows natív ablak, Linux OSR | a Wayland nem engedi megbízhatóan az idegen ablak beágyazását (`11-OSR.md`) |
| Kezdőlap | helyi HTML (`antikolt://home`), `<antikolt-card>` komponenssel | „bárhogy kinézhet" csak HTML/CSS/JS-sel valósítható meg |
| Széf formátum (Q7) | saját SQLite + DEK/KEK, **KeePass `.kdbx` export/import** mellé | a konténer-kötés és a TOTP saját sémaöt igényel; az export megnyitja más eszközök felé |
| Import (Q15) | jelszavak CSV-ből, könyvjelzők HTML-ből; **süti-import nem** | a Chrome 127+ sütititkosítása miatt harmadik fél nem tudja visszafejteni |
| Telemetria (Q13) | **nulla**; helyi összeomlás-napló, kézi küldéssel | adattudatos identitás, GDPR-egyszerűség |
| Nyelvi politika (Q12) | kód/commit angolul; dokumentáció HU (mérvadó) + EN | nemzetközi hozzájárulás, magyar felhasználóknak magyar terv |
| AI-funkciók (Q16) | nincs beépített felhő-AI; csak helyi modell vagy saját kulcs, kikapcsolva | a böngésző adattudatossági ígéretét nem lehet felhő-AI-val aláásni |
| v1 felhasználó (Q17) | először te (dogfooding), publikus kiadás csak utána | ez a legjobb minőségbiztosítás |
| Böngésző-alapfunkciók | kötelezők a v1-ben (letöltés, Ctrl+F, zoom, PDF, PiP, nyomtatás, crash recovery, akadálymentesség) | enélkül nem lehet napi böngésző |
| Többmotoros támogatás | adapter az első naptól, a második motor csak a CEF stabilizálása után | két instabil integrációt nem lehet egyszerre vinni |
| Bővítményrendszer | nincs v1-ben | minden bővítmény saját folyamat/memória, és az I1-et sérti |
| Kártyaszám-felső korlát | nincs (a rács virtualizált) | a lemezterület és a keresés skálázódik, a RAM nem nő |
