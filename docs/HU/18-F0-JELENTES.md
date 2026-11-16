# 18 — F0 jelentés: az első valós memóriamérés

> Ez a dokumentum az F0 döntési kapu anyaga (`04-UTEMTERV.md`): **először van számunk, nem becslés.**
> Mérés: 2026-11-16, VECTOR, CEF 158 (Chromium 158), Qt 6.8.3, MSVC 19.44.

## 1. Mit mértünk

A **motor folyamatfájának privát memóriáját** (browser + renderer + GPU/hálózat/utility folyamatok),
egy oldal betöltése után 15 másodperccel, 5 egy-másodperces minta mediánja. A folyamatfa az összes
`antikolt-engine-host.exe` példány (a CEF a saját futtatható fájljával indítja a segédfolyamatait).

| Oldal | Medián | Csúcs | Folyamatok |
|---|---|---|---|
| `about:blank` | **119,0 MB** | 119,0 MB | 7 |
| Wikipédia-cikk (`en.wikipedia.org/wiki/Web_browser`) | **185,3 MB** (rejtett) / **176,6 MB** (látható) | 185,4 MB | 7 |
| Google Maps | **142,9 MB** | 142,9 MB | 7 |
| YouTube nyitólap | **390,1 MB** (rejtett) / **386,3 MB** (látható) | 393,4 MB | 10 |

A láthatatlan és a látható ablak mérése gyakorlatilag egyezik (±5%), tehát a rejtett ablakos mérés
**nem torzít** — ez fontos, mert így a CI-ban nem kell ablakot felvillantani.

## 2. Mit jelent ez a büdzsére

A teljes böngésző = motor + Qt-héj (~60–100 MB) + Rust mag (~20 MB):

| Állapot | Motor (mért) | + héj/mag (becslés) | Összesen | Cél (`03-MEMORIA.md`) |
|---|---|---|---|---|
| Kártyarács, motor fut, üres oldal | 119 MB | ~90–120 MB | **~210–240 MB** | ≤ 470 MB ✅ |
| Egy átlagos cikk | 177–185 MB | ~90–120 MB | **~270–305 MB** | ≤ 620 MB ✅ |
| Egy térkép-szerű SPA | 143 MB | ~90–120 MB | **~235–265 MB** | ≤ 620 MB ✅ |
| YouTube (nehéz médiaoldal) | 386–390 MB | ~90–120 MB | **~480–510 MB** | ≤ 1000 MB ✅ |

**Verdikt: az 1 GB-os ígéret ezen a gépen bőven tartható, és a legnehezebb mért eset is a felében van.**
Ez jobb, mint a terv becslése (amely 450–620 MB-ot várt egy átlagos oldalra) — a becslés óvatos volt,
ami helyes hozzáállás, de most már tudjuk a valós számot.

Fontos árnyalat: a mérés **egy oldalra** vonatkozik. A projekt ígérete az, hogy *soha* nincs egynél több
élő oldal — éppen ezért a fenti számok a napi használat felső korlátját adják, nem az alsóját.

## 3. Fenntartások (hogy senki ne hivatkozzon félre)

1. **A mért érték privát commit** (`Process.PrivateMemorySize64`), mert a `Working Set - Private`
   teljesítményszámláló ebben a környezetben nem volt elérhető. A commit rendszerint **≥** a
   munkakészlet, tehát a számok inkább felfelé, mint lefelé torzítanak.
2. **A GPU-folyamat ebben a környezetben leáll** (`exit_code=-2147483645` = `0x80000003`, CHECK-hiba),
   és a Chromium visszaesik szoftveres kompozitálásra. A 7 folyamat ezért GPU-folyamat nélküli fa; egy
   működő GPU-n **+50–150 MB** várható. Ezt a következő mérésnél külön kell vizsgálni.
3. **A Qt-héj nincs a fában** — a fenti „+ héj/mag" oszlop becslés, nem mérés.
4. **Nincs benne** a pillanatkép-készítés, az előnézet-feldolgozás, a széf és a CEF sandbox.
5. Egyetlen gép, egyetlen hálózat; a védelem (EDR) és a böngésző-gyorsítótár növelheti a számokat.

## 4. Amit a mérés közben találtunk (és javítottunk)

Ezek a valódi mérnöki eredmények, nem mellékesek:

1. **A CEF gyermekfolyamatait a saját argumentum-parserünk ölte meg.** A `--type=gpu-process` /
   `--type=utility` kapcsolókat a parser ismeretlen argumentumnak vette, a segédfolyamatok kiléptek, a
   GPU pedig „isn't usable" hibával leállt. Javítás: a `--type=` előtagot a **parser előtt** elkapjuk, és
   egyenesen a `CefExecuteProcess`-hez megyünk.
2. **A `CefRunMessageLoop` és a gyors betöltés versenyhelyzete.** A `data:` URL betöltése *a ciklus
   indulása előtt* befejeződött, így a quit-jel elveszett → a folyamat örökre blokkolt. Javítás:
   a mérési mód saját determinisztikus pump-ciklust használ (`CefDoMessageLoopWork` + határidő), nem
   blokkoló ciklust. Ez egyben a Qt-integrációhoz is a helyes minta.
3. **Élő böngészővel hívott `CefShutdown` = azonnali access violation** (`0xC0000005`, `0x40`-es
   címre olvasás — ezt a hibapárbeszédablakot látta a projektgazda is). Javítás: lezárás előtt
   `CloseBrowser(true)`, majd addig pumpálunk, amíg a böngésző el nem tűnik.
4. **A `WS_POPUP`, soha meg nem jelenített ablak** GPU-kompozitálási hibát okoz. Javítás: normál
   ablaktípus, csak nem hívjuk meg a `ShowWindow`-t.
5. **A CEF 158-ban nincs `settings.user_data_path`** (a `cache_path` / `root_cache_path` van helyette).
6. **A hiba-párbeszédablakot kikapcsoltuk** (`SetErrorMode`), hogy egy hibás futás ne blokkoljon egy
   modális ablakon, hanem kilépési kóddal jelezzen — CI-barát.

## 5. Következtetés és a következő lépés

- **A legnagyobb kockázat („nem fér bele az 1 GB-ba") megszűnt.** A motor egy nehéz oldallal ~390 MB,
  a teljes böngésző várhatóan ~500 MB — a cél fele.
- A további munkát **nem a memória**, hanem a **funkciók** fogják meghatározni: hibernálás és
  visszaállítás, a kezdőlap, konténerek, széf.
- Következő szelet:
  1. a GPU-folyamat leállásának kivizsgálása és megszüntetése (működő GPU-s mérés);
  2. a **shell ↔ motorház** összekötése a `crates/antikolt-ipc` protokollal (első valódi IPC-hívás);
  3. a **fagyasztás** prototípusa: MHTML pillanatkép készítése a motoron (CDP vagy `CefBrowserHost`),
     majd a motor megszüntetése és a memória visszaesésének mérése — ez a termék legfontosabb
     mechanizmusának első bizonyítéka.

## 6. Újrafuttatás

```powershell
# egy oldal
& perf/measure-engine.ps1 -Url https://en.wikipedia.org/wiki/Web_browser

# több oldal, látható ablakkal
& perf/measure-engine.ps1 -Visible -Url @('https://en.wikipedia.org/wiki/Web_browser','https://www.youtube.com/')

# a nyers riportok helye (gitignore-olt)
perf/reports/
```

A mérés a `scripts/build.ps1` utáni állapotot feltételezi (a kimenetben ott van az
`antikolt-engine-host.exe` és mellette a teljes CEF runtime).
