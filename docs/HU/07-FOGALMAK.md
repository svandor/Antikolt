# 07 — Fogalomtár

A tervben használt kifejezések egységes jelentése. Ha egy dokumentum eltér ettől, ez a mérvadó.
(Zárójelben az angol megfelelő, mert a kód és a commit-üzenetek angolul készülnek — ADR-017.)

| Fogalom | Jelentés | Angolul |
|---|---|---|
| **Kártya** | Egy mentett weboldal, a kezdőlapon megjelenő egység. Nincs fül, nincs háttérben futó oldal. | card |
| **Kezdőlap** | A böngésző induló felülete: egy helyi, szerkeszthető HTML-oldal, benne a kártyák. Alapértelmezett sablonja a kártyarács. | home / start page |
| **Kártyarács** | A kezdőlap beépített alap-elrendezése: virtualizált, szekciózott kártyaháló. | card grid |
| **Kezdőlap-vászon** | A kezdőlap mint szabadon szerkeszthető weboldal (a felhasználó bárhogy átrendezheti). | home canvas |
| **Élő nézet** | Az egyetlen webes renderelő, ami éppen fut (oldal vagy kezdőlap). Egyszerre legfeljebb egy. | live view |
| **Motor** | A webes tartalom megjelenítését végző böngészőmotor (elsőként CEF/Chromium). | engine |
| **Motorház** | A motort futtató külön folyamatfa és annak futtatható fájlja (`antikolt-engine-host`). | engine host |
| **Motor-adapter** | A 7 primitívből álló szerződés (`EnginePort`), amin a mag a motort eléri. | engine adapter |
| **Csatolási mód** | Hogyan kerül a motor képe a saját ablakunkba: natív ablak-beágyazás vagy offscreen (OSR). | attach mode |
| **OSR** | Offscreen rendering: a motor képkockákba renderel, a héj kompozitálja. Wayland-biztos út. | offscreen rendering |
| **T1 / T2** | Motor életmód: T1 = melegen tartott motorház, T2 = teljesen leállított motorház (ultra-low mód). | warm / killed engine host |
| **Fagyasztás** | A kártya állapotának lemezre mentése, majd a motor megszüntetése. | freeze / hibernate |
| **Lemezhibernálás** | A fagyasztás eredménye: minden menthető állapot lemezen, nulla RAM. | disk hibernation |
| **Pillanatkép (snapshot)** | Egy fagyasztás teljes mentése: MHTML + meta + előnézet + kivont szöveg, verziózva. | snapshot |
| **MHTML** | A teljes DOM pillanatképe beágyazott erőforrásokkal; hálózat nélkül is megnyitható. | MHTML |
| **Horgony (anchor)** | A görgetési pozíció mellett mentett legfelső látható elem + eltolás, ami a lusta betöltés miatt kell a pontos visszaállításhoz. | scroll anchor |
| **Visszaolvadás** | A kártya „hazatérése": az élő nézet összezsugorodik a kezdőlapi kártyába (animáció + motor megszűnés). | melt-back |
| **Blob-tár** | Tartalom szerint címzett (`sha256`), tömörített (`zstd`) fájltár a pillanatképekhez. | content-addressed blob store |
| **Konténer** | Identitás-konténer: izolált motorpartíció egy fiókhoz (sütik, tárolók, gyorsítótár). | container / identity container |
| **Partíció** | A motor saját adattárolója konténerenként (`persist:c_<id>`). | profile partition |
| **Origin-wipe** | A 24 órás szabály végrehajtása: az adott origin munkamenet-adatainak törlése a partíción belül. | origin-level wipe |
| **Kitűzés** | A kártya jelölése, hogy soha ne hibernáljon automatikusan (pl. zene, hosszú űrlap). | pin |
| **Munkamenet (session)** | Egy `konténer × origin` pár bejelentkezési állapota: sütik, tárolók, Service Werkerek. | session |
| **Munkamenet-ledger** | A munkamenetek mérhető nyilvántartása: állapot, lemez, RAM, politika, „mit veszítünk". | session ledger |
| **`keep` / `sweep` / `wipe`** | A 24 órás politika három szintje: semmit / melléktermékeket / mindent töröl. | keep / sweep / wipe |
| **Erőforrás-profil** | Konfigurációs csomag (motor-élettartam, hibernálás, előnézet, videó, GPU): Gyors / Kiegyensúlyozott / Takarékos / Ultra / Kézi. | resource profile |
| **OSR-kompozitálás** | A motor memóriába renderel, a héj viszi ki a képernyőre (Wayland-biztos út). | OSR compositing |
| **Szoftveres passkey** | A passkey kulcspár a böngésző széfében (nem TPM-ben), CDP virtuális hitelesítővel. | software passkey |
| **Passkey-only fiók** | Olyan fiók, ahol a jelszót törölték, és **kizárólag** passkey-jel lehet belépni — passkey nélküli böngészővel egyáltalán nem nyitható meg. | passkey-only account |
| **DCO (Signed-off-by)** | Egy soros commit-nyilatkozat: a közreműködő jogosult a kódot beadni; nincs CLA, nincs engedélykérés. | Developer Certificate of Origin |
| **Sigstore / provenance** | Ingyenes, automatizált aláírás + bizonyíték, hogy a bináris a nyilvános forrásból, a nyilvános CI-ben készült. | Sigstore provenance |
| **SignPath Foundation** | Ingyenes kódaláírás OSI-licencű nyílt forrású projekteknek (kiadásonként egy jóváhagyás). | SignPath Foundation |
| **Recept** | Oldalanként tanult/tárolt beállítások: sötétítési technika, zoom, engedélyek, űrlap-tippek, belépési forgatókönyv. | site recipe |
| **Belépési forgatókönyv** | Deklaratív lépéssor egy oldal automatikus bejelentkeztetéséhez, biztonsági korlátokkal. | login runbook |
| **Széf** | A beépített jelszó-/TOTP-/passkey-tár. | vault |
| **DEK / KEK** | Adatkulcs (rekordokat titkosít) / kulcsot titkosító kulcs (rendszer kulcstartó vagy mesterjelszó). | data / key encryption key |
| **Technika-létra** | A sötétítés hat fokozata (T0–T6), luminancia-ellenőrzéssel, oldalanként tanulva. | darkening ladder |
| **Luminancia-ellenőrzés** | A sötétítés eredményének mérése a képernyőkép átlagfényerejéből; ha nem elég sötét, jön a következő technika. | luminance check |
| **Szürke mód** | A három alapséma egyike (világos / szürke / sötét), az oldalakon színtelenítés + enyhe tompítás. | grey mode |
| **Ultra-low mód** | T2 + raszter-kezdőlap: a böngésző ~120–180 MB-on, a rács képként jelenik meg, az első interakcióra él. | ultra-low mode |
| **Raszter-kezdőlap** | A kezdőlap utolsó állapotának képe + interakciós térkép, motor nélkül. | raster home |
| **Büdzsé-vezérlő** | A folyamatfa memóriáját figyelő és sávok szerint beavatkozó komponens. | memory governor |
| **M0/M1/M2/M0-light** | A memória-modell állapotai: rács motorral / egy átlagos oldal / egy nehéz oldal / rács motor nélkül. | memory modes |
| **Bridge API** | A kezdőlap és a mag közötti szűk, allowlistelt üzenet-API. | bridge API |
| **Előnézet-ütemező** | A tétlen időben, ugyanazzal a motorral futó előnézet-frissítő. | preview scheduler |
| **Fázis 0 (F0)** | Az időmérő hét: prototípus + valós memóriaszámok + döntési kapu. | phase 0 |
| **Dogfooding** | A projektgazda a saját napi böngészőjeként használja a terméket (M3-tól). | dogfooding |

## Elnevezési szabályok a kódban

- Crate/fájlnév-előtag: `antikolt-` (pl. `antikolt-core`, `antikolt-engine-host`).
- Belső séma neve: `antikolt://home`.
- Adatbázis-táblák: egyes szám, `snake_case` (`card`, `snapshot`, `site_recipe`).
- Az élő nézet a kódban **singleton** (`LiveView`), mert az I1 invariáns ezt kívánja.
