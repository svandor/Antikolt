# 03 — Memória-büdzsé és mérés

> A projekt létjogosultsága a memórián áll vagy bukik. Ez a dokumentum kimondja, **mennyi** fér bele,
> **hogyan** mérjük, és **mit** teszünk, ha túllépjük.

> A célgép (VECTOR) adatai és az eszközprofilok: `09-CELHARDVER.md`. A bejelentkezések memória-költsége:
> `10-MUNKAMENET.md` (röviden: életben tartott bejelentkezés = 0 RAM, amíg a kártya hibernált).
>
> **MÉRT EREDMÉNY (F0, 2026-11-16):** a motor folyamatfája egy átlagos cikken **177–185 MB**, egy
> nehéz médiaoldalon (YouTube) **386–390 MB**, üres oldalon **119 MB**. A teljes böngésző a Qt-héjjal
> együtt várhatóan **270–510 MB** — a lenti büdzsé tartható, sőt bőven alatta marad.
> Részletes mérés, módszer és fenntartások: [`18-F0-JELENTES.md`](18-F0-JELENTES.md).

## 1. Alapigazságok (amit sokan elrontanak)

1. **Nem az RSS-t kell összeadni.** Az RSS a megosztott memórialapokat (közös könyvtárak, Chromium kód)
   minden folyamatnál beszámítja, ezért az „RSS-összeg" 2–3× túlbecsül. A helyes metrika a
   **privát munkakészlet** (private working set / private bytes), illetve Linuxon a **PSS**
   (`/proc/<pid>/smaps_rollup`), ami a megosztott lapokat arányosan osztja el.
2. **A memória nagy része a renderelőben van, nem a böngészőben.** Egy statikus cikk 80–150 MB, egy
   videós SPA (YouTube 1080p) 400–550 MB, egy nagyon nehéz adminfelület 600 MB fölött is lehet. Ezért az
   „egy élő nézet" szabály a legfontosabb memória-döntés.
3. **A Chromium alapja nem tüntethető el.** A motor több folyamatból áll: browser/fő folyamat, GPU,
   hálózati szolgáltatás, storage/utility, renderelő, esetleg audio. Ezek együtt ~180–280 MB-ot tesznek ki,
   akkor is, ha egyetlen oldal sincs betöltve. Ezért **kell** a motor teljes leállítása (T2 mód) az
   igazán alacsony tétlen értékhez.
4. **A saját felületünk is fogyaszt.** A kezdőlap egy webes renderelő, tehát a kártyarács nézet is
   ~60–120 MB plusz. Ezért a rács virtualizált, az előnézetek kicsik, és a kezdőlap bonyolultságának
   felső korlátja van (lásd 6. fejezet).
5. **A lemez ingyen van, a RAM nem.** A döntésed szerint mindent lemezre mentünk (akár több tíz GB), ezért
   a RAM-büdzsé csak az *élő* dolgokra vonatkozik.

## 2. Hová megy a memória — szerkezeti modell

| Komponens | Tétlen | Egy oldallal | Megjegyzés |
|---|---|---|---|
| Qt-héj (QML, Qt könyvtárak) | 60–100 MB | 60–100 MB | a saját UI; nem nő oldalanként |
| Rust mag (DB, széf, index) | 10–20 MB | 15–30 MB | lap-cache a SQLite-nál nő |
| Motor alap (browser+GPU+hálózat+utility) | 180–280 MB | 180–280 MB | fix költség |
| Renderelő — kezdőlap | 60–120 MB | — | egyszerű helyi oldal |
| Renderelő — átlagos oldal | — | 80–200 MB | cikk, blog, dokumentáció |
| Renderelő — nehéz oldal | — | 400–600 MB | videó, nagy SPA, WebGL |
| Előnézet-dekódolás (csúcs) | 5–10 MB | 5–10 MB | plafonnal korlátozva |

Ezekből a **módok** (a módok értékei a komponens-sávok összegéből, kerekítve származnak):

| Mód | Leírás | Becsült privát memória |
|---|---|---|
| **M0 — kártyarács, motor fut (T1)** | alapértelmezett napi állapot | **350–470 MB** |
| **M0-light** | kártyarács, motor leállítva, a kezdőlap raszterképként (T2) | **120–180 MB** |
| **M1 — egy átlagos oldal** | olvasás, keresés | **450–620 MB** |
| **M2 — egy nehéz oldal** | videó, nagy SPA | **800–1000 MB** (itt lép be a büdzsé-vezérlő) |
| **M3 — két oldal (tilos)** | csak rövid átmenet a váltás alatt | 1000–1400 MB |

> A táblázat **mérnöki becslés**, amit a Fázis 0 mérőpadja validál (lásd 7. fejezet). A számok a
> döntésekhez kellenek, nem ígéretek.

**Következtetés:** az „1 GB alatt marad" **a napi használatban tartható** (M0, M1), a nehéz oldalaknál
(M2) a büdzsé-vezérlőnek kell beavatkoznia. A „150 MB tétlen" csak a motor leállításával (M0-light)
érhető el — ezt a felületnek őszintén kommunikálnia kell („Ultra-low mód: a kártyarács első kattintásra
élővé válik").

## 3. Ultra-low mód (T2) és a raszter-kezdőlap

A motor leállításával a memória 350–470 MB-ról 120–180 MB-ra esik, de a kezdőlapot valami még mutatja.
Megoldás: **raszter-kezdőlap**:

1. A kezdőlap utolsó állapotáról készül egy (esetleg 2× felbontású) kép.
2. A motor leáll, a héj ezt a képet jeleníti meg, egy **interakciós térkép** (kártya-téglalapok, gombok)
   fölötte.
3. Az első valódi interakcióra (kattintás, görgetés, gépelés, hover 300 ms után) a motor elindul
   (~400–800 ms), és a kezdőlap élővé válik — a kép addig is látszik, tehát nincs üres képernyő.
4. Görgetés a raszterképen: a kép eltolható és a kártyák „kipattanthatók" képként — ez a tétlen böngészés
   élménye, nulla motorral.

Ez az egyetlen mód, ami a „modern, 100 MB-os böngésző" ígéretet hozza. Kockázat: a raszterkép és az élő
kezdőlap közti váltás észrevehető lehet; ezért csak akkor lép be, ha a felhasználó kéri („Ultra-low")
vagy 2 perc tétlenség után, és a visszaváltás azonnali.

## 4. Büdzsé-vezérlő (memory governor)

A mag folyamatosan (2 s-onként) mintát vesz a folyamatfáról, és sávok szerint cselekszik. A vezérlő a
**profilokkal együtt** működik (`09-CELHARDVER.md` 3. fejezet): a sávok a mindenkori profilhoz képest
értelmeződnek.

| Sáv | Küszöb (privát) | Teendő |
|---|---|---|
| zöld | < 700 MB | nincs teendő |
| sárga | 700–850 MB | előnézet-frissítés szünetel; gyorsítótár-takarítás; jelzés a naplóban |
| narancs | 850–1000 MB | a legrégebbi háttér-erőforrások felszabadítása; médiaminőség-korlát (pl. 720p) bekapcsolása; javaslat a felületnek: „nehéz oldal" |
| piros | > 1000 MB | mélytisztítás: a motor gyorsítótárának ürítése, `jscode` cache eldobása, végső esetben a kezdőlap renderelőjének leállítása (raszter mód) |

A vezérlő **soha nem szakítja meg a felhasználó munkáját** (nem zárja be az aktív oldalt); csak a
nem-látható erőforrásokat szabadítja fel, illetve jelez.

**Médiaminőség-korlát:** videós oldalakon a felbontás csökkentése (720p) a legnagyobb egylépéses nyereség
(M2 → ~600–700 MB). Ezt a felület egy kis jelzéssel mutatja („memória-takarékos videó"), és egy kattintással
felülbírálható.

## 5. Mérési módszer

### 5.1. Mit mérünk

- **Windows:** a teljes folyamatfa **privát munkakészlete** (`Private Working Set`), folyamatonként és
  összesen. Eszköz: `Get-Counter '\Process(*)\Working Set - Private'` szűrve az `antikolt*` folyamatokra,
  illetve a mag saját mérője (`GetProcessMemoryInfo` / `K32GetProcessMemoryInfo`).
- **Linux:** `PSS` a `/proc/<pid>/smaps_rollup`-ból, a folyamatfára összegezve.
- **Kiegészítő:** csúcsérték (peak), renderelő-folyamatok száma (I1 invariáns), motorház állapota (T1/T2),
  lemezen lévő blobok mérete.

### 5.2. Mérési szabályok

- **5 mérés, medián** (a memória zajos; az átlag félrevezet).
- **Meleg indítás** (a második indítás), mert az első futás gyorsítótár nélküli.
- **Rögzített szcenárió**, ugyanazokkal az oldalakkal, ugyanazzal a viewporttal (1440×900, DPR 1.25).
- **Semmi más nem fut** a gépen a mérés alatt (a mérőpad figyelmeztet).
- **Baseline commitolva**: minden mérés eredménye `perf/baseline/<dátum>.json` + riport markdown.

### 5.3. Szcenáriók

| # | Szcenárió | Mit bizonyít |
|---|---|---|
| S0 | üres böngésző, kártyarács, 0 kártya | alapköltség |
| S1 | kártyarács, 50 kártya, 50 előnézet | I3, virtualizáció |
| S2 | kártyarács, 500 kártya | nagy könyvtár |
| S3 | egy statikus cikk megnyitva | M1 |
| S4 | YouTube 1080p lejátszás | M2, büdzsé-vezérlő |
| S5 | Gmail-szerű SPA | nehéz SPA |
| S6 | 10 kártya megnyitása egymás után (váltogatás) | szivárgás-ellenőrzés |
| S7 | 1 óra használat szimulálva (200 váltás) | hosszú távú stabilitás |
| S8 | ultra-low mód (T2) tétlen | M0-light |
| S9 | 25 életben tartott bejelentkezés, mind dormant, hideg motor | a bejelentkezés RAM-költsége ≈ 0 |
| S10 | ugyanaz a szcenárió Gyors / Kiegyensúlyozott / Takarékos / Ultra profillal | a profilok valós különbsége |

### 5.4. Elfogadási kritériumok (a CI-kapu alapja)

| Mérőszám | Cél | Kemény maximum |
|---|---|---|
| S0 privát memória | ≤ 380 MB | 450 MB |
| S1 privát memória | ≤ 450 MB | 550 MB |
| S2 privát memória | ≤ 500 MB | 600 MB |
| S3 privát memória | ≤ 620 MB | 700 MB |
| S4 privát memória | ≤ 1000 MB | 1100 MB |
| S8 (ultra-low) | ≤ 180 MB | 250 MB |
| S9 (25 dormant bejelentkezés, hideg motor) | ≤ 180 MB | 250 MB |
| S10 (profilok: Gyors ↔ Ultra különbség) | dokumentált, mérve | — |
| Renderelő folyamatok száma (I1) | 1 | 1 |
| S7 végén a memória növekedése S1-hez képest | ≤ 10% | 20% |
| Kártyanyitás (S3) érzékelt idő | ≤ 600 ms | 900 ms |
| — ebből az első kép (R0) megjelenése | ≤ 50 ms | 100 ms |
| — T2 (hideg motor) esetén az élő tartalom | ≤ 1200 ms | 1500 ms |
| Fagyasztás (S3) | ≤ 800 ms | 1500 ms |
| Keresés 10 000 kártyán | ≤ 50 ms | 120 ms |

## 6. A kezdőlap memória-fegyelme (kötelező szabályok)

1. **Virtualizált rács:** egyszerre max. ~60 kártya-DOM.
2. **Kis előnézetek:** 260×150 WebP; dekódolt bittérkép-plafon 24 kép (utána `img` újratöltés).
3. **Nincs iframe, nincs élő előnézet** a kezdőlapon (az iframe egy második renderelő = I1 megsértése).
4. **Nincs nagy JS-keretrendszer** a kezdőlapon; a beépített sablon ~50 KB JS alatt marad.
5. **A felhasználó saját kezdőlapja** kap egy „súly" jelzést (hány MB memóriát használ a renderelő), és
   ha túllépi a keretet, a böngésző figyelmeztet (de nem tiltja).
6. **Képek és betűtípusok** a kezdőlapon helyi fájlból (nincs hálózat).

## 7. Fázis 0 — a mérési hét (a terv első munkadarabja)

Mielőtt bármi más épül, **egy hét** alatt össze kell állnia egy időmérő prototípusnak, ami:

1. Qt 6 ablak + CEF egy oldallal, egy partícióval;
2. méri és naplózza a folyamatfa privát memóriáját;
3. le tudja állítani a motorházat (T2), és újraindítani;
4. a három kulcsfontosságú oldalon (statikus cikk, YouTube, Gmail) megadja a valós számokat;
5. összeveti a fenti becslésekkel, és **riportot ír** (`perf/phase0-report.md`).

**Döntési kapu:** ha a valós M0 > 600 MB vagy M2 > 1,3 GB, akkor a terv felülvizsgálandó (pl. a kezdőlap
natív QML-re váltása, vagy a motorház agresszívebb leállítása). Ezt a döntést nem lehet megúszni
találgatással — ezért az első hét ez.

## 8. Amit szándékosan NEM teszünk a memóriáért

| Ötlet | Miért nem |
|---|---|
| Sandbox kikapcsolása | biztonsági öngyilkosság, nem is nyer jelentős memóriát |
| Egyfolyamatos motor (`--single-process`) | instabil, összeomlik, nem termékképes |
| Szoftveres renderelés alapból | CPU-robbanás, videó tönkremegy (csak hibakereséshez) |
| A GPU-folyamat kilövése | a legtöbb oldal lassabb lesz, nem éri meg |
| JS-memória agresszív korlátozása | az oldalak összeomlanak; csak szélső esetben (büdzsé-vezérlő piros sáv) |
| Bővítmények támogatása | minden bővítmény saját folyamat/memória |

## 9. Amit a motorhangolásból érdemes kipróbálni (Fázis 0-ban mérve)

Ezek Chromium/CEF kapcsolók, **mérni kell** őket, mert a hatásuk verziófüggő:

- renderelőnkénti heap-plafon (V8 `--max-old-space-size`) — a szökő oldalak megfékezésére;
- a „spare renderer" (tartalék renderelő előmelegítése) kikapcsolása — egy folyamattal kevesebb;
- háttér-throttling a nem látható nézetekre;
- lemezes gyorsítótár méretkorlátja (kevesebb membe mappelt gyorsítótár);
- a `BackForwardCache` kikapcsolása (a vissza-előre gyorsítótár teljes oldalakat tart életben).

Minden kipróbált kapcsoló eredménye bekerül a `perf/` riportba, és csak akkor marad be, ha **mérhetően**
segít és nem rontja a kompatibilitást.

## 10. Őszinte összefoglaló

- **Elérhető:** napi használat 1 élő oldallal 450–620 MB, kártyarács 350–470 MB, ultra-low mód 120–180 MB,
  nehéz videós oldal ~800–1000 MB (büdzsé-vezérlővel kezelve).
- **Nem elérhető:** 4-5 nyitott, nehéz oldal 1 GB alatt — ezért van az „egy élő nézet" szabály.
- **Nem elérhető:** 100 MB alatti tétlen memória *élő* kezdőlappal (a Chromium alap és a renderelő miatt);
  csak raszter-kezdőlappal, motor nélkül.
- **Amit ez jelent:** a jelenlegi 3-4 böngészőablak / 4-5 GB helyett **egy böngésző, 0,5 GB körül** —
  ez a valós, mérhető nyereség, amit a projekt ígér.
