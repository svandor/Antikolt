# 13 — Licenc és jogi keret (Q10 részletesen)

> Ez a dokumentum **nem jogi tanácsadás**, hanem mérnöki döntés-előkészítés nyilvános források alapján.
> A végleges licencfájl és a szerzői nyilatkozatok a kód első commitjával kerülnek a repóba.

Rövid válasz: **MPL-2.0**, dinamikusan linkelt Qt-val, BSD-3-as CEF-fel, DCO-alapú hozzájárulással,
SPDX-fejlécekkel és egy őszinte `NOTICE`-szal az AI-segítségről.

## 1. Licencválasztás: MPL-2.0 vs Apache-2.0 vs GPL-3.0

| | **MPL-2.0** *(javasolt)* | **Apache-2.0** | **GPL-3.0** |
|---|---|---|---|
| Mit tehet a felhasználó | használhat, módosíthat, terjeszthet | ugyanaz + kifejezett szabadalmi engedély | ugyanaz, de erős copyleft |
| Mit kell megosztani | a **módosított fájlokat** (fájlszintű copyleft); az új fájlok szabadon licencelhetők | semmit | **az egész származtatott művet** |
| Későbbi zárt bővítmény | ✅ lehetséges (a bővítmény nem „fertőződik") | ✅ | ❌ gyakorlatilag nem |
| Qt (LGPL-3) | ✅ kompatibilis (MPL 3.3 „Larger Work") | ✅ | ❌ ütközik, a projektet GPL-be kényszeríti |
| CEF (BSD-3) | ✅ | ✅ | ✅ |
| Böngészőknél a norma | **Firefox, Brave, Floorp, Zen** | Chromium (BSD) | nem jellemző |

**Miért az MPL-2.0:**
1. Ez a böngészők bevált licence — egy felhasználó és egy közreműködő számára ismerős.
2. **Fájlszintű** copyleft: a módosított fájlokat meg kell osztani, de a saját új kódod szabadon
   licencelhető marad (pl. későbbi kereskedelmi kiegészítés).
3. Nincs szükség CLA-ra: a hozzájárulás automatikusan MPL-2.0 alá kerül.
4. Kompatibilis a Qt LGPL-3-mal és a CEF BSD-3-mal.

**Miért nem a GPL-3:** a Qt LGPL-3-mal való kombináció és a későbbi zárt bővítmény lehetősége miatt
gyakorlatilag kizárt. **Miért nem az Apache-2.0:** működne, de nem védi meg a projektet attól, hogy
valaki a javításokat zárt formában tartsa — a böngészőknél az MPL a jobb egyensúly.

### 1.1. Mit jelent ez gyakorlatban (a te esetedben)

| Kérdés | Válasz az MPL-2.0 mellett |
|---|---|
| Használhatja-e bárki ingyen, akár cég is? | **Igen.** |
| Módosíthatja-e bárki? | **Igen.** |
| Közreműködhet-e bárki? | **Igen**, külön engedély nélkül (DCO, nem CLA) — lásd `15-HOZZAJARULAS.md`. |
| Mit kell megosztania, aki módosít? | Az **általunk írt fájlok módosított változatát** (MPL-2.0 alatt). Az **új** fájljai lehetnek zártak is. |
| Elveheti-e valaki a projektet és zárhatja-e el? | **Nem** a mi fájljainkat; de készíthet zárt kiegészítést. |
| Kié a szerzői jog? | Mindenkié a saját kódjáé. A projekt nem kér jogátadást. |
| Válthatunk-e később licencet? | Csak **minden** közreműködő beleegyezésével — ez a „visszafordíthatatlanság" ára. |
| Későbbi fizetős/zárt változat? | Lehetséges (a mag nyílt marad, a kiegészítés zárt lehet). |
| Qt/CEF/distrok kompatibilisek? | **Igen** (LGPL-3 „Larger Work", BSD-3). |
| Ismerik-e a közreműködők? | **Igen** — Firefox, Brave, Floorp, Zen ugyanezt használja. |

**A javaslatom tehát: MPL-2.0 + DCO.** A „bárki szabadon közreműködhet" elvárásodat ez **maradéktalanul**
teljesíti (nincs CLA, nincs aláírás, nincs engedélykérés), miközben a projekt kódja nyílt marad. Ha a
későbbi licencváltás szabadsága fontosabb lenne, az csak CLA-val lenne elérhető — és az éppen a szabad
közreműködést nehezítené, ezért **nem javaslom**.

### 1.2. A biztonság nem licenc, hanem folyamat kérdése

Fontos tisztázás: **a licenc nem befolyásolja a biztonságot.** Az Apache-2.0 sem lenne biztonságosabb, az
MPL-2.0 sem kevésbé az. A böngésző kockázatait a **folyamat** kezeli: védett kódrészek, kötelező review,
fuzz-tesztek, memória-regressziós kapuk, sebezhetőség-bejelentési csatorna, aláírt kiadások. Ezt a
`15-HOZZAJARULAS.md` írja le részletesen — ez a válasz a „lehetnek biztonsági kockázatok" felvetésedre.

## 2. Qt 6 (LGPL-3) — kötelezettségek, amelyeket be kell tartani

Dinamikus linkelés mellett ez a lista kötelező, és nem elhanyagolható:

| Kötelezettség | Mit jelent a gyakorlatban |
|---|---|
| **Dinamikus linkelés** | A Qt-t DLL/SO formában szállítjuk, nem beégetve. |
| **Újralinkelhetőség** | A felhasználónak lehetőséget kell adni, hogy a Qt-t lecserélje. Windows-on ez a Qt-DLL-ek cseréjét jelenti (dokumentálni kell), Linuxon természetes. |
| **Forrás / írásos ajánlat** | A Qt forrását elérhetővé kell tenni (hivatkozás is elég, 3 évig érvényes ajánlat). |
| **Licencszövegek** | A „Névjegy / Licencek" ablakban a Qt LGPL-3 szövege és a felhasználás ténye. |
| **Módosított Qt** | Ha a Qt-t módosítanánk, azt közzé kell tenni — nem tiltjuk. |
| **Statikus linkelés** | ❌ kerülendő: csak akkor lenne szabad, ha a felhasználó újralinkelhető objektumfájlokat kap — gyakorlatban ez a teljes mű GPL-esítését jelentené. |

**MPL-2.0 + Qt LGPL-3 együtt:** az MPL 2.0 3.3 szakasza („Larger Work") lehetővé teszi a kombinációt;
a Qt-s részeket LGPL-3 alatt, az MPL-es részeket MPL-2.0 alatt kell terjeszteni. Ez a bevett gyakorlat
(pl. Qt-alapú nyílt forrású alkalmazások).

## 3. CEF / Chromium — licence és védjegy

| Tétel | Kötelezettség |
|---|---|
| CEF licenc | **BSD-3-clause** → a szerzői jogi nyilatkozatot és a licencszöveget meg kell őrizni |
| Chromium harmadik felek | a Chromium több száz komponenst tartalmaz; a `THIRD-PARTY-NOTICES` / CREDITS fájlt mellékelni kell |
| Védjegy | „**Chromium-alapú** böngészőnek" nevezhetjük magunkat; a „Chrome" nevet, logót **nem** használhatjuk engedély nélkül |

A gyakorlat: a telepítő tartalmaz egy `THIRD-PARTY-NOTICES.txt`-t, a Névjegy-ablakban pedig egy
„Nyílt forrású licencek" gomb.

## 4. A név: „Antikolt"

- Nyilvános védjegy-keresés alapján **nincs ütköző szoftver-védjegy** (a szó jelentése „antikolt" =
  régies/antik – magyar szó, szoftverre nem lajstromozott).
- Flathub és winget listázás védjegy-szempontból akadálytalan, ha nem sugall hivatalos kapcsolatot.
- Javaslat: a név marad; a domaint és a GitHub-szervezetet érdemes előre lefoglalni.
- **Megjegyzés:** a név magyar, a kód angol — ez az ADR-017 szerint rendben van, de a nemzetközi
  olvasóknak a README-ben egy soros magyarázat kell („antikolt ≈ antique-ish, régies").

## 5. Közreműködés: DCO, nem CLA

- Az MPL-2.0 **nem igényel CLA-t**: a hozzájárulás a licenccel automatikusan MPL-2.0 alá kerül, a szerző
  megtartja a szerzői jogát.
- Ezért a **DCO** (`git commit -s`, `Signed-off-by`) a helyes mechanizmus: könnyű, és a legtöbb
  közreműködő ismeri.
- **Következmény, amit tudni kell:** az MPL-2.0 alatt már kiadott kód **visszafordíthatatlanul** MPL-2.0
  marad; egy későbbi licencváltáshoz minden közreműködő beleegyezése kellene. Ezért a licencet most kell
  jól megválasztani — és most az MPL-2.0 a döntés.
- A `CONTRIBUTING.md` a v0.1-gyel együtt készül el, benne a DCO és a kódstílus.

## 6. Magyar/EU sajátosságok

| Téma | Gyakorlati megjegyzés |
|---|---|
| Szerzői jog | Magánszemélyként a kód automatikusan szerzői jogi védelem alatt áll (Szjt., 1999. évi LXXVI. tv.); nincs szükség bejegyzésre. |
| Adományok (VAT) | Hobbiprojekt adományai jellemzően nem áfakötelesek, amíg nem jár értékesített termék/szolgáltatás; ellenjuttatás (perk) esetén már nem ilyen egyértelmű → könyvelő. |
| Licencfejlécek | Fájlonként `SPDX-License-Identifier: MPL-2.0` (3 soros fejléc helyett elfogadott, MPL FAQ Q22). |
| Fogyasztóvédelem | Ha a böngészőt fogyasztóknak adod ki, a felelősség és a garancia kérdései felmerülnek — az MPL kizárja a szavatosságot, de ez nem minden esetben érvényesíthető fogyasztóval szemben. |
| **EU CRA (kiberbiztonsági rendelet)** | A digitális elemeket tartalmazó termékekre 2027 végétől kötelezettségek lépnek életbe; a tisztán nem kereskedelmi, nyílt forrású projektek könnyített megítélés alá eshetnek. **Ellenőrizendő** a kiadás előtt — ez nem blokkoló ma, de 2027-ben már lehet. |

## 7. AI-segítség a kódban (ez a projekt esetében kötelező téma)

Mivel a fejlesztés **AI-ágenssel** történik (ezt a felhasználó rögzítette, ADR-021), a repónak ezt
**őszintén és szabályosan** kell kezelnie:

1. **Ne állítsunk kizárólagos emberi szerzőséget** a fejlécekben. Helyette:
   `Portions copyright © 2026 Antikolt Project. Portions generated with AI assistance.`
2. **`NOTICE` fájl**: rövid nyilatkozat arról, hogy a kód egy részét AI-eszközökkel állították elő, és
   hogy ezt a szerző átnézte/tesztelte.
3. **`AUTHORS` fájl**: az emberi közreműködők (jelenleg a projektgazda) és a fejlesztőeszköz megnevezése.
4. **`SPDX-License-Identifier`** minden fájlban — így nem kell a fejlécben szerzőséget állítani.
5. A kód **eredetének nyilvántartása** (melyik fájl milyen forrásból/ML-ből származik) a commit-üzenetekben
   és a `docs/`-ban; ez később jogi vita esetén véd.
6. Nincs univerzális, kötelező szabály (2026), de a Linux-kernel és az SPDX közösség ezt az irányt
   javasolja — a projekt ezt követi.

## 8. Amit a repóba be kell tenni (checklista)

| Fájl | Tartalom |
|---|---|
| `LICENSE` | MPL-2.0 teljes szövege |
| `NOTICE` | CEF/Chromium/Qt harmadik felek + AI-nyilatkozat |
| `AUTHORS` | emberi közreműködők + fejlesztőeszköz |
| `THIRD-PARTY-NOTICES.txt` | a bináris mellé (Chromium komponensek, Qt, zstd, SQLite stb.) |
| `CONTRIBUTING.md` | DCO, kódstílus, hogyan kell AI-segítséget jelezni |
| Névjegy-ablak | „Licencek" gomb → a fenti szövegek |
| Minden forrásfájl fejléce | `SPDX-License-Identifier: MPL-2.0` |

## 9. Döntés (megerősítendő)

- Licenc: **MPL-2.0** ✅
- Qt: **dinamikus linkelés**, újralinkelhetőség dokumentálva ✅
- CEF: BSD-3, `THIRD-PARTY-NOTICES` mellékelve ✅
- Közreműködés: **DCO**, nincs CLA ✅
- AI: **nyilatkozat + SPDX + AUTHORS/NOTICE** ✅
- Név: **Antikolt** marad ✅

Ha bármelyik ponttal nem értesz egyet (pl. inkább Apache-2.0, vagy inkább CLA a későbbi
licencváltás lehetőségéért), most kell szólni — később már nehéz vagy lehetetlen visszafordítani.
