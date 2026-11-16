# 09 — Célhardver és eszközprofilok

A mérések és a büdzsé-vezérlő alapja a tényleges célgép. Ez a dokumentum rögzíti a hardvert, amit
mértünk, és azt, hogy ez milyen tervezési döntéseket hoz magával.

## 1. A célgép (VECTOR) — mért adatok

Mérés: 2026-11, a gépen futó DSH-munkamenetből, registry + folyamatlista alapján.

| Komponens | Érték |
|---|---|
| Gazdagép | `VECTOR` (laptop) |
| CPU | **Intel Core Ultra 9 275HX** — 24 szál (8 P-mag + 16 E-mag), Arrow Lake-HX |
| RAM | **32 GB** (felhasználó által megadott; a sandbox a WMI-t nem engedte, ezért a `TotalPhysicalMemory` nem olvasható) |
| GPU (diszkrét) | **NVIDIA GeForce RTX 5080 Laptop GPU**, 16 GB VRAM, driver 32.0.16.2124 |
| GPU (integrált) | **Intel Graphics** (Arc, ~18 GB megosztott), driver 32.0.101.9150 |
| Kijelző | 2 monitor, az elsődleges effektív felbontása **4096×1152** (skálázott) |
| Tároló | 1 TB SSD, ebből **655 GB szabad** |
| OS | Windows 11 (a build-szám a sandbox miatt nem olvasható) |

**Pillanatnyi terhelés a méréskor** (nem a böngésző, hanem a gépen futó dolgok — ez a valóságos zajszint):

| Folyamat | RAM |
|---|---|
| `node` (ez a fejlesztői környezet) | 1866 MB |
| `Memory Compression` | 969 MB |
| `msedgewebview2` (3 folyamat, ez az asszisztens felülete) | ~1680 MB |
| `dwm` (ablakkezelő) | 696 MB |
| `chrome` | 548 MB |
| `thunderbird` | 398 MB |

Ebből két következtetés: (a) a mérésekhez **zajszűrés kell** (a DSH és a fejlesztői eszközök fussanak
más gépen, vagy zárt mérési környezetben); (b) a felhasználó panasza érthető: a 32 GB-ból máris ~5–6 GB
megy el a háttérre, mielőtt egy böngészőablak megnyílna.

## 2. Mit jelent ez a tervezésre

1. **Az 1 GB nem kényszer, hanem fegyelem.** 32 GB RAM mellett nem azért kell 1 GB, mert nincs több,
   hanem mert a cél a *hordozható, halk, akkumulátor-kímélő* böngésző. A gép ereje miatt a fejlesztés
   gyors lesz (24 szál: a pillanatkép-tömörítés és az előnézet-kódolás párhuzamosítható), de a
   *elfogadási kritériumok* ugyanazok.
2. **Két GPU = két mérési ág.** A böngésző (motor + GPU-folyamat) futhat az Intel iGPU-n (takarékos) vagy
   az RTX 5080-on (gyors). Mindkettőt mérni kell: az OSR-es kompozitálás és a videolejátszás RAM/CPU
   költsége eltér. Alapértelmezés: **integrált GPU** (akkumulátor), a diszkrét GPU csak kérésre
   (pl. WebGL-nehéz oldal).
3. **16 GB VRAM bőven elég** a hardveres OSR-hez (megosztott textúrák), tehát az OSR nem VRAM-korlátos.
4. **Két monitor + skálázás**: a DPI-tesztelés nem opcionális (125/150/175%); a natív ablak-beágyazás
   DPI-váltáskor és monitorok közti áthúzáskor a legtörékenyebb pont (ezt a Fázis 0-ban kell tesztelni).
5. **Akkumulátoros üzem**: külön profil — agresszívebb hibernálás, előnézet-frissítés kikapcsolva,
   médiaminőség-korlát, meleg motorház (T1) kikapcsolása.
6. **655 GB szabad hely**: a „több tíz GB" lemezhasználat bőven belefér. Az **alapkvóta marad 50 GB**
   (a döntés szerint), de gépenként felülbírálható — a VECTOR-on például 100 GB-ra állítható
   (`config/devices/vector.toml`), ha az időutazás és a pillanatképek miatt kell.

## 3. Eszközprofilok (Q4 válasza)

A felhasználói kérés: „legyenek profilok, úgyis az éles teszt dönt". Az erőforrás-viselkedés ezért
**profilokba** van csomagolva, nem egyetlen kapcsolóba. A profil egy csomag: motor-élettartam,
hibernálási idő, előnézet-frissítés, médiaminőség, büdzsé-sávok, GPU-választás.

| Profil | Motor | Hibernálás | Előnézet-frissítés | Videó | Mikor |
|---|---|---|---|---|---|
| **Gyors** | meleg (T1) | 15 perc | igen | teljes | asztali gép, konnektor |
| **Kiegyensúlyozott** *(alap)* | meleg (T1) | 10 perc | igen, tétlenül | 1080p | a mindennapi használat |
| **Takarékos** | hideg (T2) 2 perc után | 5 perc | nem | 720p | laptop akkumulátoron |
| **Ultra** | hideg (T2) azonnal | 2 perc | nem | 480–720p | memória-kritikus helyzet |
| **Kézi** | minden érték külön állítható | — | — | — | haladó felhasználó |

- A profil **automatikusan válthat** (konnektor ↔ akkumulátor, memórianyomás), de a felhasználó
  rögzítheti.
- Gépenként megjegyzett konfiguráció (`config/devices/<hostname>.toml`), hogy a VECTOR-on és egy
  gyengébb gépen más legyen az alap.
- A profilváltás **soha nem szakít meg munkát**: csak a jövőbeli viselkedést állítja.
- Minden profil mögött ugyanaz a három mérőszám áll: RAM, kártyanyitási idő, akkumulátor-fogyás.
  A profil nem marketing, hanem **konfiguráció** — a mérések (`perf/`) mondják meg, melyik a jó.

## 4. Mérési alapvonal a célgépen

A `perf/` mérőpad a VECTOR-on fut, három üzemmódban:

| Üzemmód | Mit rögzít |
|---|---|
| `ac` (konnektor) | teljes büdzsé, T1/T2, előnézet-frissítéssel |
| `battery` | ugyanaz, de a Takarékos profillal, fogyasztásméréssel (ha elérhető) |
| `cold` | üres gyorsítótár, első indítás |

A mérőpad **zajszűrése**: a DSH/Node/Edge-folyamatok bezárása vagy külön mérési fiók használata;
a privát munkakészlet mérése folyamatonként; 5 futás mediánja (lásd `03-MEMORIA.md` 5. fejezet).

## 5. Amit a hardver miatt NEM kell megtennünk

- Nem kell a gyorsítótárat vagy a felbontást agresszíven csökkenteni a memória kedvéért — van RAM.
- Nem kell lemondani a hardveres videolejátszásról vagy a WebGL-ről.
- Nem kell a felhasználót korlátozni a „nehéz oldalak" miatt — a büdzsé-vezérlő jelez, de nem tilt.

A cél továbbra is az, hogy **ugyanaz a böngésző egy 8 GB-os gépen is használható legyen** — ezért a
profilok és a büdzsé-vezérlő; a VECTOR csak a fejlesztés és az alapvonal helye.
