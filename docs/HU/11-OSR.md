# 11 — OSR vagy natív ablak? (Q5 részletesen)

A kérdés: hogyan kerül a webmotor képe a böngészőablakba. Két út van, és a különbség nem elméleti —
ez dönti el, hogy a Linux (Wayland) egyáltalán működik-e, és mennyi CPU-t eszik a görgetés.

## 1. A lényeg egy bekezdésben

**Natív ablak:** a motor a **saját kis ablakába** fest, mi ezt az ablakot „beillesztjük" a saját
ablakunkba (mintha egy képet tennénk egy keretbe). Gyors, hardveres, de platformonként külön trükk kell,
és Wayland alatt nem megbízható.

**OSR (offscreen rendering):** a motor **memóriába fest** (egy textúrába), és mi visszük ki a képernyőre
(mint egy videólejátszó a filmkockákat). Kicsit drágább, de mindenhol működik, és a motor akkor is tud
renderelni, ha nincs saját ablaka.

## 2. Natív ablak-beágyazás

**Hogyan:** a motor ablaka a mi ablakunk **gyermeke** lesz.

- Windows: `SetParent(motorHWND, aMiHWND)`, majd a méret/pozíció követése (`MoveWindow`), fókusz és
  bevitel átadása.
- Linux X11: `XReparentWindow` — ugyanez az elv.
- Linux Wayland: **nincs megbízható megfelelője.** A `xdg-foreign` protokollal lehetne idegen surface-t
  beágyazni, de kompozitoronként eltérően támogatott, és a bevitel/fókusz kezelése törékeny.

| Előny | Hátrány |
|---|---|
| Hardveres gyorsítás „ingyen" (a motor közvetlenül a képernyőre fest) | Platformonként külön kód (Windows/X11) |
| Nincs képkocka-másolás → alacsony CPU, jó akkumulátor | Wayland alatt nem járható út |
| Videó/WebGL/DRM a megszokott módon működik | A két ablak szinkronban tartása (átméretezés, görgetés, DPI-váltás, monitorok közti áthúzás) törékeny |
| Kevesebb saját kód (nincs kompozitálás) | A „motor külön folyamatban" csak akkor megy, ha az ablak-átadás is megy (Windows/X11: igen) |

## 3. OSR (offscreen rendering)

**Hogyan:** a motor egy puffert/textúrát kap, abba renderel, és **jelzi, hogy kész** („paint" esemény).
A héj ezt a textúrát rajzolja ki a saját felületére, és a felhasználói bemenetet (egér, billentyű, IME,
görgetés) **visszafelé** továbbítja a motornak.

- Gyorsított változat: megosztott GPU-textúra (Windows: D3D11 shared handle, Linux: dmabuf) — ekkor a
  költség kicsi.
- Szoftveres változat: CPU-s másolás — egyszerű, de drága (1080p @60 fps ≈ 500 MB/s memóriamozgatás).

| Előny | Hátrány |
|---|---|
| **Platformfüggetlen** (Windows, X11, Wayland egyaránt) | Képkockánkénti kompozitálás: CPU/GPU többletköltség |
| A motor **láthatatlanul** is tud renderelni → előnézet, háttér-pillanatkép, „motor nélküli ablak" is könnyű | Bevitel, IME, kurzor, drag&drop, kijelölés kézi továbbítása — sok hibalehetőség |
| Egységes a többmotoros csatolás (minden motor „képkockákat ad") | **Akadálymentesség**: a motor saját akadálymentességi fája nem kapcsolódik automatikusan a Qt-hoz — hidat kell építeni (képernyőolvasó!) |
| A motor folyamata bármikor leállítható, a kép marad (raszter-kezdőlap!) | Videó/DRM/GPU-gyorsítás néha külön utat igényel |

## 4. Miért lesz Windows-on natív, Linuxon OSR

Nem elvi meggyőződés, hanem a platform adottsága:

| Platform | Elsődleges út | Miért |
|---|---|---|
| Windows 11 | natív ablak | gyors, stabil, hardveres; a Wayland-probléma nem létezik |
| Linux X11 | natív ablak | ugyanaz, mint Windows-on |
| Linux Wayland | **OSR** (vagy natív + `xdg-foreign`, ha a kompozitor támogatja) | a beágyazás nem megbízható; az OSR a biztos út |

A kettő **ugyanazon absztrakció** (`EngineSurface`) mögött él, így a felső rétegek nem tudják, melyik
megy. A motor-adapter szerződése nem változik.

## 5. Mit jelent ez neked, felhasználóként?

- **Semmi nem látszik belőle.** Ugyanaz a felület, ugyanazok a kártyák.
- Windows-on a görgetés és a videó a megszokott módon gyors.
- Linuxon (Wayland) kicsit több CPU-t eszik a megjelenítés — ezt a Takarékos profil és a
  médiaminőség-korlát ellensúlyozza.
- Az **előnézeti képek és a raszter-kezdőlap** OSR nélkül is működnek (a motor a saját oldalán tud
  képernyőképet adni), tehát a memória-ígéret nem függ ettől a döntéstől.

## 6. Kockázatok (amit a Fázis 0-ban tesztelni kell)

1. **DPI-váltás / monitorok közti áthúzás** natív beágyazásnál (a leggyakoribb hibaforrás).
2. **Fókusz és IME** (magyar ékezetes bevitel, összetett karakterek) mindkét útban.
3. **Kurzor és kijelölés** OSR-nél (a kurzort a motornak kell kérnie, nem a Qt-nak).
4. **Drag & drop** fájlok behúzása az oldalra.
5. **Akadálymentesség** OSR-nél: képernyőolvasó-híd (ez külön mérföldkő, nem lehet kihagyni).
6. **Videó és DRM** (Netflix-szerű tartalom) viselkedése OSR-ben.
7. **Késleltetés** (input latency) OSR-nél: a kompozitálás plusz 1–3 képkockát adhat — mérni kell, hogy
   érezhető-e.

## 7. Döntés (ADR-006 pontosítása)

- Windows 11 (v1): **natív ablak-beágyazás**.
- Linux (v0.3): **OSR** Waylanden, natív X11-en.
- A kód **az első naptól** úgy épül, hogy a csatolási mód cserélhető legyen; az OSR-ág a Fázis 0-ban
  kap egy minimális prototípust (egy oldal, görgetés, kattintás), hogy a kockázat korán kiderüljön.
- Ha az OSR a Fázis 0-ban vállalhatatlanul drága, a Linux-út csúszik, de a Windows-os termék nem.

**Egy mondatban:** a natív beágyazás a gyorsabb és egyszerűbb, az OSR a hordozhatóbb — ezért
Windows-on az elsőt, Linuxon a másodikat használjuk, ugyanazon felület mögött.
