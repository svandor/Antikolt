# 04 — Ütemterv, mérföldkövek, elfogadás

> **Fejlesztési modell:** a kódot az **asszisztens (AI-ágens)** írja, a projektgazda dönt, tesztel és
> dogfoodol. Ez a ciklus gyorsítja az írást, de **nem** a validálást: minden fázis végén mérés, teszt és
> a gazda jóváhagyása dönt. Az időbecslések ezért ciklusokban értendők, ±40% bizonytalansággal.


> **Jelölők állapota (2026-11-19):** a pipák a kódhoz és a tesztekhez igazodnak, nem a szándékhoz. Egy tétel
> akkor pipa, ha van rá **futtatható bizonyíték** (a nevezett önteszt), és a részleges tételeknél a nyitott rész
> ott van a sorban. A teljes, mindig friss kép a `19-FOLYTATAS.md` §12; ez a fájl a mérföldkövek terve.

## Fázisok

| Fázis | Tartalom | Ciklus | Kimenet |
|---|---|---|---|
| **F0** | **Kockázat-mentesítő hét(ek)**: 4 spike (lásd lent) + mérőpad | 2–3 hét | `perf/phase0-report.md`, döntési kapu |
| **F1** | Csontváz: Qt-héj + CEF motorház + 1 kártya + lemezhibernálás + visszaállítás | 6–8 hét | kártya: nyit → fagyaszt → visszaállít, mérve |
| **F2** | Kezdőlap-vászon + kártyarács + előnézet + keresés + munkaterületek | 6–8 hét | napi böngészés kártyákkal |
| **F3** | Konténerek + popup/OAuth + engedélyek + proxy + **munkamenet-ledger és panel** | 4–6 hét | **M3: saját napi böngésző** |
| **F4** | Széf + autofill + TOTP + belépési forgatókönyv | 6–8 hét | „nem kell jelszót gépelni" |
| **F4b** | *(opcionális, a v1.1-ben)* szoftveres passkey a széfben | 4–6 hét | passkey-only fiókok is működnek |
| **F5** | Téma-motor (világos/szürke/sötét/AMOLED + technika-létra) | 3–4 hét | nincs vakító oldal |
| **F6** | Erőforrás-profilok + raszter-kezdőlap + büdzsé-vezérlő + hangolás | 3–4 hét | 120–180 MB-os tétlen állapot |
| **F7** | Asztali böngésző-alapok (letöltés, Ctrl+F, zoom, PDF, PiP, nyomtatás, crash recovery, akadálymentesség, HU/EN) | 4–6 hét | **M7: publikus béta** |
| **F8** | Linux/Nobara + Wayland (OSR) + csomagolás + frissítő + aláírás | 4–6 hét | **M8: 1.0** |
| **F9** | Második motor (WebKitGTK) — opcionális | 6–8 hét | többmotoros ígéret |

**Összesen (F4b és F9 nélkül):** ~37–52 hét. Ezért a fázisok **önállóan használható** állapotokat adnak:
az F3 végén már a saját napi böngésződ lehet.

## F0 — a négy spike (ez a legfontosabb hét)

A cél nem szépség, hanem hogy a **négy legnagyobb kockázat** kiderüljön, mielőtt bármi más épül.

| Spike | Kérdés, amire válaszol | Kész, ha |
|---|---|---|
| **S-A. Qt + CEF + mérés** | mekkora a valós memória egy oldallal? (M0/M1/M2) | fut a mérőpad, megvannak a számok a három referenciaoldalon |
| **S-B. Lemezhibernálás** | működik-e az MHTML készítés **és betöltés**, milyen gyors, mekkora? | egy oldal fagyaszt→visszaállít ciklusa < 2,5 s, a tartalom olvasható |
| **S-C. OSR** | mekkora az OSR CPU-költsége és késleltetése egy oldalon? | 30 fps-es görgetés, kattintás működik, mérve |
| **S-D. Passkey (CDP)** | működik-e a virtuális hitelesítő **termelési** környezetben, exportálható-e a kulcs? | egy `create`+`get` kör lefut egy teszt-RP-n, a kulcs exportálható |

Ha az S-D nem megy, a passkey a v1.1-ből kikerül (dokumentált korlát), és a többi fázis nem áll meg.
Ha az S-C vállalhatatlanul drága, a Linux csúszik, a Windows nem.
Ha az S-B nem működik, az egész terv alapja (lemezhibernálás) változik — **ezért ez az első**.

**Döntési kapu:** ha a valós M0 > 600 MB vagy a nehéz oldal > 1,3 GB, a terv felülvizsgálandó
(natív QML rács, agresszívebb motorleállítás, vagy a cél újradefiniálása).

## Mérföldkövek

### M0 — „Működik a bizonyíték" (F0 vége)
- [ ] Négy spike lefutott, riport a `perf/`-ben.
- [ ] Valós memóriaszámok a három referenciaoldalon (statikus cikk, YouTube 1080p, Gmail-szerű SPA).
- [ ] Döntés: megyünk tovább / módosítunk.

### M1 — „A kártya él" (F1 vége)
- [x] Kártya megnyitása, görgetés, fagyasztás, visszaállítás (R0→R1→R2). — `--opentest`, `--scrollzoomtest`, `--lifecycletest` (2026-11-19)
- [x] MHTML + meta + előnézet atomikusan a lemezen. — `--lifecycletest`, `--capturetest`, `--previewtest`
- [x] 10 perces szabály (hang/videó/kitűzés kivétellel). — `--lifecycletest`; a média-kivétel a takarításban is: `--mediacleantest` (ADR-048)
- [ ] Mérőpad: S3 és S6 zöld.

### M2 — „Használom" (F2 vége)
- [x] Virtualizált rács 500 kártyával. — `--gridscaletest`: 503 kártya, **13** renderelt DOM-elem, 20 ms az első renderig (2026-11-19)
- [x] Szerkeszthető kezdőlap: kártya hozzáadása (`＋` + soros űrlap) és törlése. — `--addcardtest` (ADR-061); nyitott: a cím/webcím átírása a felületen, és a törlés megerősítése
- [ ] `<antikolt-card>` külön csomagként (a mai kártya a lap saját komponense).
- [x] Keresés (cím, URL, jegyzet, címke, oldalszöveg; magyar ékezetek). — `--searchtest` + `--tagtest`: ékezet-független cím/webcím/jegyzet/címke, és az oldalszöveg is kereshető (`search_page_text=ok`, ADR-049/055)
- [x] Munkaterületek, címkék, jegyzet, kitűzés; előnézet-ütemező. — `--workspacetest` + `--tagtest` + `--notetest` + `--pintest`: a fül-sor a mag munkaterület-táblájából jön (ma SQLite-sorok, ADR-070), a kártya `workspace` mezője tárolt adat (a régi `index > 12` kitalálás helyett, ADR-056), a fül-soron van életciklus (`＋`/dupla kattintás/`×`, ADR-057), és a kártya munkaterülete a saját választójával váltható (ADR-058), és a fülre is dobható fogd-és-vidddel (ADR-058 megerősítve, `--workspacedragtest`)

### M3 — „Minden fiókom egyszerre" (F3 vége) → **a valódi termék**
- [x] Konténerek színkóddal, kártya-konténer kötés, konténer-váltás. — `--containertest` (ADR-050…054): színes pont, választó a kártyán, chip-legend + szűrő, tábla a magban, `＋`/dupla kattintás/`×`, és használt konténer törlése áthelyezéssel (mind mérve); nyitott: konténerenkénti engedély/proxy
- [ ] OAuth-popup a helyes partícióban (max. 60 s, dokumentált I1-kivétel).
- [x] **Munkamenetek panel + ledger**: látszik, mi van életben, mennyi RAM/lemez, mit veszítenénk. — `--sessionpanel`, `--scrollbook`
- [x] `keep`/`sweep`/`wipe` politika + origin-szintű végrehajtás. — `--wipetest`, `--cleanuptest`, `--mediacleantest`
- [ ] Engedélykezelés és proxy konténerenként.
- [x] Profil-költöztetés: egy fájl, verziózott köteg. — `--profiletest` (ADR-059): `profile_export`/`profile_import`; nyitott: felületi gomb és a köteg titkosítása

### M4 — „Nem kell jelszót gépelnem" (F4 vége)
- [ ] Széf (DEK/KEK, opcionális mesterjelszó), auto-zárolás, KDBX export.
- [ ] Kitöltés izolált világban, origin-kötéssel, natív setterrel.
- [ ] TOTP + vágólap-tisztítás.
- [ ] Belépési forgatókönyv + kézi átadás CAPTCHA esetén.

### M4b — passkey (F4b, v1.1)
- [ ] Szoftveres hitelesítő a széfben, `create`/`get` kör, teszt-RP-vel.
- [ ] Őszinte tájékoztatás: nincs hardveres védelem; a széf zárolása a jelenlét-ellenőrzés.

### M5 — „Nem vakít el este" (F5 vége)
- [x] Négy felületi téma (világos/szürke/sötét/AMOLED), kontraszt-teszt CI-ban. — `--themetest`: pixelmérés + WCAG AA minden témára (a legkisebb 5.6:1)
- [ ] Technika-létra + luminancia-ellenőrzés + oldalankénti tanulás.
- [ ] Szürke mód és olvasómód.

### M6 — „Elfér 150 MB-ban" (F6 vége)
- [ ] Öt erőforrás-profil, automatikus váltással.
- [ ] Büdzsé-vezérlő sávokkal, médiaminőség-korláttal.
- [ ] Raszter-kezdőlap (motor leállítva).
- [ ] Hangolási riport (mely kapcsolók segítettek, mérve).

### M7 — „Béta" (F7 vége)
- [ ] Letöltéskezelő, Ctrl+F, PDF, PiP, nyomtatás. — a **zoom** kész (`--scrollzoomtest`), a többi nyitott
- [ ] Összeomlás utáni helyreállítás.
- [ ] Billentyűzetes navigáció, képernyőolvasó-támogatás.
- [ ] HU/EN felület; jelszó- és könyvjelző-import.

### M8 — „1.0" (F8 vége)
- [ ] Linux (Nobara/KDE/Wayland) OSR-rel, RPM csomag.
- [ ] SignPath-aláírás + Ed25519 frissítés-aláírás, telepítő + frissítő.
- [ ] Licenc/NOTICE/AUTHORS/THIRD-PARTY fájlok, HU/EN dokumentáció, CONTRIBUTING (DCO).

## Kockázatok és „stop" kritériumok

| Kockázat | Ha bekövetkezik | Teendő |
|---|---|---|
| A CEF + Qt integráció nem megy a tervezett idő alatt | F1 csúszik | Egyszerűsítés: natív ablak + minimális QML, a kezdőlap később |
| A memória nem jön ki | a projekt elveszti az értelmét | F0 kapujában döntés: raszter-kezdőlap, motor nélküli mód, vagy a cél újradefiniálása |
| Az MHTML visszatöltés nem működik | a hibernálás alapja sérül | erőforrás-térkép + saját kérés-elfogó (lásd `02` 5.2. tartalék) |
| Az OSR drága vagy törékeny | Linux csúszik | Windows-first kiadás, Linux később |
| Passkey nem megoldható CEF-fel | a passkey-only fiókok kizártak | dokumentált korlát + hardveres út kutatása |
| A dark mode/autofill folyton törik | karbantartási teher | technika-létra + 20 oldalas regressziós készlet automatizálva |
| Egy ember + egy ágens kevés | elvérzés | M3-nál megállás: „saját böngészőként" használat, a többi fázis opcionális |

## Amit előre rögzítek (hogy később ne csússzon)

- **Nincs bővítményrendszer v1-ben.**
- **Egy ablak, egy kártya-renderelő** (I1) — a bejelentkezési popup a dokumentált kivétel.
- **Nincs felhő, nincs telemetria.**
- **A kezdőlap nem kap natív hozzáférést** — csak a bridge API-t.
- **A széf formátuma verziózott és exportálható**, hogy 5 év múlva is megnyitható legyen.
- **Minden állítás mérhető**: minden fázis végén frissül a `perf/` riport és a `03-MEMORIA.md` büdzséje.
