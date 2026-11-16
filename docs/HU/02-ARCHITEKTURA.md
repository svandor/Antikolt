# 02 — Architektúra terv

> Ez a mérvadó terv. A `00-JAVASLATOK.md` a bírálat, a `03-MEMORIA.md` a mérési terv, a
> `04-UTEMTERV.md` az ütemezés, a `05-DONTESEK.md` a döntési napló.

## 1. Célok, nem célok, invariánsok

**Célok**

1. Egy böngésző, ami napi használatban **1 GB RAM** alatt marad, és **kártyarács nézetben ~150 MB**.
2. **Egy felhasználó, minden fiók egyszerre** — konténerek, széf, TOTP, automatikus kitöltés.
3. **Lemezhibernálás**: a kártya elhagyásakor minden állapot lemezre kerül, a motor megszűnik.
4. **Szerkeszthető, weboldal formátumú kezdőlap**, amibe a kártyák beilleszthetők.
5. **Cserélhető motorok**: az első a Chromium (CEF), a szerkezet készen áll a következőkre.

**Nem célok (v1)**

- Böngésző-bővítmény API (Chrome extensions), felhő-szinkronizáció, mobil, többablakos működés,
  beépített felhő-AI, Bittorrent/torrent-kliens, e-mail-kliens, VPN-szolgáltatás.

**Invariánsok (ezek a tervezés alaptörvényei — ha egy funkció ezeket sérti, akkor nem kerül be)**

| # | Invariáns | Ellenőrzés |
|---|---|---|
| I1 | Egyszerre **legfeljebb egy** kártya-renderelő fut (oldal **vagy** kezdőlap). Egyetlen kivétel: a felhasználó által indított **bejelentkezési popup** (OAuth/SSO), ami legfeljebb 60 s-ig él, és automatikusan bezárul. | mérőpad: kártya-renderelő ≤ 1 (+0–1 átmeneti popup) |
| I2 | Minden kártya állapota **lemezen** van, amikor nem élő. | hibernálás után a kártyához tartozó snapshot létezik |
| I3 | A kártya soha nem fogyaszt RAM-ot, amikor nincs megnyitva. | mérőpad: 50 kártya, 0 nyitott oldal → büdzsé |
| I4 | Egy ablak, egy élő nézet (v1). | kód: `LiveView` singleton |
| I5 | Titok (jelszó, TOTP) soha nem kerül a weboldal fővilágába, naplóba, hibajelentésbe. | teszt: page JS nem látja |
| I6 | Nincs hálózati telemetria. | hálózati napló a tesztben üres |
| I7 | A felhasználó adatai (sütik, snapshotok) az ő gépén maradnak, titkosítva, ahol titok van. | audit |

## 2. Rendszer-topológia

```
                    ┌──────────────────────────────┐
                    │      antikolt-shell (Qt 6)    │  ~60-100 MB
                    │  QML felület: kártyarács,     │
                    │  vezérlősáv, széf-ablak,      │
                    │  beállítások, értesítések     │
                    └───────┬──────────────┬────────┘
             FFI (cxx)      │              │  IPC (Unix socket / named pipe)
                    ┌───────▼───────┐   ┌──▼─────────────────────────┐
                    │ antikolt-core │   │  antikolt-engine-host      │ ~200-500 MB
                    │ (Rust lib)    │   │  (CEF / Chromium, külön     │  (csak ha fut)
                    │ · SQLite      │   │   folyamatfa)               │
                    │ · széf        │   │  · CefBrowser + CDP         │
                    │ · életciklus  │   │  · natív ablak vagy OSR     │
                    │ · snapshot tár│   │  · hálózat, sütik, GPU      │
                    │ · keresés     │   └────────────────────────────┘
                    │ · előnézet    │
                    └───────┬───────┘
                            │
                    ┌───────▼───────────────────────────────┐
                    │  Lemez: SQLite, blob-tár, partíciók   │
                    │  (korlátlan, kvótával: alap 50 GB)    │
                    └───────────────────────────────────────┘
```

**Miért nem egyetlen folyamat?** Mert a memória-ígérethez szükség van arra, hogy a *motort teljesen le
lehessen állítani*. Egy külön folyamatfa (T2 mód) 2 perc tétlenség után leáll, és a böngésző **120–180 MB-ra**
zsugorodik. Emellett ez a többmotoros támogatás egyetlen ésszerű útja is: minden motor a saját
végrehajtható fájlja, ugyanazon IPC-szerződés mögött.

**Miért Rust a mag?** Az életciklus-gép, a széf, a pillanatkép-tár és a keresés memóriabiztonságot és
determinisztikus erőforrás-kezelést igényel; a Rust ezt adja, és jól tesztelhető. A CEF és a Qt C++,
ezért a **héj C++, a mag Rust**, `cxx` FFI-vel. (Ha később a héj is Rust lenne, az FFI-réteg marad.)

## 3. A motorház (engine host) és az adapter

### 3.1. A szerződés (7 primitív + 3 kiegészítő)

Minden motor ezt az interfészt valósítja meg; a mag csak ezt látja. A primitívek szándékosan szegények,
hogy egy másik motor (WebKitGTK, WebView2) is teljesíteni tudja őket.

```
EnginePort
  create(cfg) -> SurfaceId           // partíció, viewport, DPR, engedélyek, proxy
  navigate(id, url, opts) -> ()      // opts: referrer, hard reload, timeout
  eval(id, script, world) -> Json    // world: isolated | main
  scroll(id, x, y, animate) -> ()
  capture(id, fmt, quality) -> Bytes // előnézethez (WebP)
  snapshot(id) -> MhtmlBytes         // teljes DOM, beágyazott erőforrásokkal
  destroy(id) -> ()
  --- kiegészítők ---
  set_media(id, prefers_color_scheme, forced_colors)
  set_zoom(id, factor) / metrics(id) -> {fps, dom_nodes, js_heap, mem}
  download / popup / permission események -> eseménycsatorna
```

**Szándékosan NINCS** az interfészben: bővítménykezelés, függvényhívás a motor belső C++ objektumain,
tetszőleges natív kód injektálása. Ez az, ami a motor cserélhetőségét megőrzi.

### 3.2. Protokoll

- Csatorna: helyi socket (Windows: named pipe, Linux: `AF_UNIX` a felhasználói runtime-könyvtárban),
  hozzáférés-vezérléssel (csak azonos felhasználó, `0600`).
- Kódolás: hossz-előtagolt MessagePack (`rmp-serde`), verziózott (`proto/v1`).
- Minden üzenet: `{id, op, payload}` / `{reply_to, ok, error}`; 5 másodperces alap időkorlát, a
  `snapshot`/`capture` 15 másodperc.
- Képkocka-csatorna (csak OSR módban): dupla pufferelt megosztott memória + sérülés-téglalapok.
- A motorház **soha nem kap fájlrendszer-útvonalat** a magtól, csak partíció-nevet; a partíciókönyvtárat
  a mag hozza létre, a motorház csak a saját `--user-data-dir`-ját ismeri.

### 3.3. Csatolási módok

| Mód | Platform | Előny | Hátrány |
|---|---|---|---|
| **Natív ablak** (`SetParent` / `XReparentWindow`) | Windows, X11 | hardveres gyorsítás, egyszerű, gyors | Wayland alatt nem megbízható; beviteli fókusz-kezelés kézi |
| **OSR** (offscreen, képkocka-megosztás) | mindenhol | platformfüggetlen, Wayland-biztos, egységes | CPU-igényes, összetett (vsync, bevitel-továbbítás) |

**Terv:** v1 Windows = natív ablak; a Linux (v0.3) = OSR. Az IPC és az adapter úgy készül, hogy a csatolási
mód neki átlátszó legyen (`EngineSurface` absztrakció). Részletes magyarázat (miért, milyen költséggel,
milyen kockázatokkal): `11-OSR.md`.

### 3.4. Motor életmódok és erőforrás-profilok

A motor nem „be- vagy kikapcsolt", hanem egy **profil** része. A profil egy csomag: motor-élettartam,
hibernálási idő, előnézet-frissítés, médiaminőség, GPU-választás, büdzsé-sávok.

| Profil | Motor | Hibernálás | Előnézet | Videó | Mikor |
|---|---|---|---|---|---|
| **Gyors** | meleg (T1) | 15 perc | igen | teljes | asztali gép |
| **Kiegyensúlyozott** *(alap)* | meleg (T1) | 10 perc | igen, tétlenül | 1080p | mindennapi használat |
| **Takarékos** | hideg (T2) 2 perc után | 5 perc | nem | 720p | laptop akkumulátoron |
| **Ultra** | hideg (T2) azonnal | 2 perc | nem | 480–720p | memórianyomás |
| **Kézi** | minden érték külön | — | — | — | haladó felhasználó |

- T1 = meleg motorház (kártyanyitás ~200–400 ms), T2 = leállított motorház (~600–1200 ms). T2-ben a
  kártya fölé érő egér **előmelegítést** indít, így a kattintás már meleg motort talál.
- A profil **automatikusan válthat** (konnektor ↔ akkumulátor, memórianyomás), de rögzíthető.
- Gépenként megjegyzett konfiguráció (`config/devices/<hostname>.toml`).
- Részletek és a profilok indoklása: `09-CELHARDVER.md` 3. fejezet.

## 4. Kártya-életciklus

**Nincs „alvó fül" állapot.** Ez a terv egyik legfontosabb különbsége a Chrome „Memory Saver"
viselkedéséhez képest: nálunk nincs félholt, lebecsült prioritású, szétkapcsolt, de memóriát mégis
foglaló oldal. A kártya **vagy él** (és akkor az az egyetlen élő nézet), **vagy lemezen van** (és akkor
nulla RAM-ot foglal). A kettő között csak egy rövid, néhány száz milliszekundumos átmenet van.

```
             ┌──────────────────────────── SAVED (dormant) ────────────────────────────┐
             │  nincs motor, van snapshot + előnézet + szöveg                          │
             └───────▲──────────────────────────────┬──────────────────────────────────┘
     24h lejárat      │                              │ kártya megnyitása
     (purge)          │                              ▼
             ┌────────┴────────┐            ┌────────────────┐
             │ PURGING         │            │ WARMING        │  motor + navigálás + snapshot
             │ (adattörlés)    │            │ (skeleton kép) │  visszaállítási kísérlet
             └────────┬────────┘            └───────┬────────┘
                      │                             ▼
                      │                     ┌───────────────┐
                      │                     │ LIVE          │  az egyetlen élő nézet
                      │                     └───┬───────┬───┘
                      │        kártya elhagyása │       │ 10 perc tétlenség
                      │                         ▼       ▼
                      │                  ┌─────────────────┐
                      └──────────────────│ FREEZING        │  capture + MHTML + scroll + storage
                                         │ (mentés)        │  majd motor destroy
                                         └─────────────────┘
```

### 4.1. Állapotok és átmenetek

| Állapot | Jelentés | Memória |
|---|---|---|
| `saved` | kártya a kezdőlapon, snapshot van | 0 |
| `warming` | motor indul, oldal tölt, helyette előnézet/skeleton látszik | nő |
| `live` | az egyetlen élő nézet | teljes |
| `freezing` | mentés folyik (max. 2,5 s), utána `destroy` | teljes |
| `purging` | 24 órás adattörlés fut | kicsi |
| `archived` | a felhasználó archiválta: nincs előnézet-frissítés, de a snapshot megmarad | 0 |

### 4.2. A 10 perces szabály (motor megszűnik)

- **A tétlenség-időzítő** a kártya megnyitásakor indul, és **minden felhasználói bemenetre nullázódik**
  (billentyű, egér, görgetés, kattintás, fókusz).
- Alapérték **10 perc**, kártyánként 1–120 perc vagy `soha`.
- **Felfüggesztő kivételek** (ilyenkor nem szűnik meg a motor):
  - a kártya **kitűzött** (`pinned`),
  - az oldal **hangot ad** vagy **videót játszik** (`navigator.mediaSession.playbackState`,
    `document.querySelector('video,audio')` állapota, illetve a motor által jelzett „audible" esemény),
  - **fájlletöltés** van folyamatban,
  - **mentetlen, hosszú űrlap** van fókuszban? — *nem* kivétel: a mentés megőrzi, ezért nincs rá szükség,
    de a felület 5 másodperccel előtte diszkrét sávot mutat („hamarosan lemezre mentjük").
- Ha a felhasználó éppen egy kártyán dolgozik, de a motor memóriabüdzsé veszélyesen nő (lásd
  `03-MEMORIA.md` 4. fejezet), a **búdzsé-vezérlő** kérheti az azonnali fagyasztást — de csak jelzéssel.

### 4.3. A 24 órás szabály (háromszintű munkamenet-politika)

A cél nem az, hogy kijelentkeztessünk, hanem hogy **a bejelentkezések megmaradjanak** — a bejelentkezés
ugyanis lemezen van, nem RAM-ban (részletesen: `10-MUNKAMENET.md`). Ezért a 24 óra **nem a sütikre**
vonatkozik, hanem a munkamenet **melléktermékeire**:

| Politika | 24 óra után törlődik | Megmarad | Ajánlott |
|---|---|---|---|
| **`keep`** | semmi (csak gyorsítótár-LRU) | sütik, tárolók, bejelentkezés | fontos szolgáltatások (Google, Facebook, saját oldal) |
| **`sweep`** *(konténer-alap)* | gyorsítótár, `sessionStorage`, Service Worker, code cache | sütik, localStorage, IndexedDB → **bejelentkezve marad** | a legtöbb oldal |
| **`wipe`** | minden az originra (kijelentkezés) | csak a széf rekordja | bank, egészségügy, munka |

- A lejárat a kártya **utolsó megnyitásától** számít, lustán értékelődik (megnyitáskor és tétlen időben).
- A törlés **eredet (origin) szintű** az adott partíción belül: az azonos domain + azonos konténer kártyái
  ugyanazt a munkamenetet használják, tehát a szabály kiszámítható.
- A **snapshot soha nem törlődik** a politika miatt (az offline olvasás és a keresés megmarad).
- Felülírás: kártyánkénti `session_policy`, konténerenkénti alapérték, és `wipe_on_lock` érzékeny
  konténerekre.
- A végrehajtáshoz **kell a motor** (CDP `Storage.clearDataForOrigin`), mert a Chromium nem origin-onként
  külön fájlban tárol. Ezért a lejárat először **jelölést** hoz létre (`cleanup_pending`), és a törlés a
  motor következő elérhető pillanatában fut le: az adott konténer következő kártyanyitásakor, vagy egy
  tétlen karbantartási ablakban (max. 2 s). **A futó motor fájljaihoz soha nem nyúlunk kézzel**, és a
  partíciókönyvtárat csak a konténer teljes törlésekor érintjük.
- A felület minden kártyán mutatja a politikát és a lakat-állapotot (`🔒` bejelentkezve, `⏳` hamarosan
  törlődik, `🔓` kijelentkezve).

### 4.4. A „visszaolvadás" animációja (a kártya hazatérése)

Ez a fogalom a te megfogalmazásodból jön („lepotyognak, bezáródnak, visszaolvadnak a weboldalba"), és
a felület nyelve ezt közvetíti:

1. A kártya elhagyásakor a motor **először** elkészíti a pillanatképet (max. 2,5 s), közben a felület egy
   vékony, mozgó sávot mutat a vezérlősáv alatt („mentés").
2. Amikor a mentés kész, az élő nézet **250 ms alatt összezsugorodik** a kezdőlapon lévő kártya helyére
   (FLIP-animáció: a kép a kezdő téglalapból a kártya téglalapjába transzformálódik), és a kártya
   előnézete frissül.
3. A motor `destroy`, a memória azonnal felszabadul — a felhasználó ezt a folyamatot akkor sem érzékeli,
   ha a rács már látszik.

Ugyanez visszafelé: kártya kattintás → a kártya képe „kinyílik" az élő nézetté (0 ms, mert a kép már a
kezünkben van), miközben az oldal tölt. Így soha nincs üres, villogó, fehér átmenet.

## 5. Lemezhibernálás — specifikáció

Ez a termék legfontosabb saját képessége, ezért pontosan definiált.

### 5.1. Mit mentünk (és mit nem)

| Réteg | Mentjük? | Hogyan | Megjegyzés |
|---|---|---|---|
| DOM (teljes, aktuális állapot) | ✅ | CDP `Page.captureSnapshot` → MHTML | beágyazott CSS/kép is |
| Kitöltött űrlapértékek | ✅ | a DOM része (MHTML) | jelszó-mezők **kizárva**, helyette hivatkozás |
| Görgetés (x, y) + horgony | ✅ | `scrollX/Y` + legfelső látható elem szelektora + offset | horgony a lusta betöltés miatt |
| Viewport, DPR, zoom | ✅ | meta | |
| localStorage, IndexedDB, cache, sütik | ✅ *(nem is kell menteni)* | a partíció lemezen marad | a hibernálás nem töröl tárolót |
| sessionStorage | ✅ | kinyerés + újra-injektálás | ez a rendererrel együtt meghal |
| Médialejátszó pozíció | ✅ | `currentTime`, `paused`, `volume`, `muted` | |
| Sötét mód technika + eredmény | ✅ | meta | |
| Kivont szöveg (kereséshez) | ✅ | MHTML-ből | max. 256 KB/kártya |
| Előnézeti kép | ✅ | WebP 260×150 | |
| **JS futásidejű állapot** | ❌ | — | lehetetlen; dokumentált korlát |
| **Nyitott WebSocket/WebRTC/SSE** | ❌ | — | újracsatlakozás az oldal dolga |
| **Futó animációk, időzítők** | ❌ | — | újraindulnak |
| **Passkey-hitelesítés élő munkamenete** | ❌ | — | újra kell hitelesíteni (v1.1) |

### 5.2. MHTML pillanatkép — hogyan

1. `Page.enable`, majd `Page.captureSnapshot {format: "mhtml"}` a motoron (CEF-en keresztül CDP-n vagy
   a motor saját API-ján).
2. A kapott MHTML a DOM pillanatképe, beágyazott erőforrásokkal — **hálózat nélkül is megnyitható**.
3. Tömörítés: `zstd` (3-as szint). Tipikus méret: 1–10 MB oldalanként (nehéz oldalnál 20–40 MB).
4. Tartalom szerinti címzés: a blob neve a `sha256`-ja, így az azonos tartalmú pillanatképek egyszer
   tárolódnak.

**Kockázat és tartalék (ezt a Fázis 0-ban ki kell próbálni):** az MHTML *előállítása* biztos (CDP), de az
MHTML **betöltése** motortól és buildtől függő lehet — ha a CEF nem tud MHTML-t megnyitni, a visszaállítás
nem működik. Két tartalék:

- **A) Erőforrás-térkép + saját kérés-elfogó (javasolt tartalék):** a pillanatképből kinyerjük a
  mappába ágyazott erőforrásokat, és a saját `GetResourceHandler`ünk szolgálja ki őket az eredeti URL-en,
  offline. Ekkor az oldal szkriptjei is **lefutnak** (lemezről), tehát a hűség jobb az MHTML-nél.
- **B) Önálló HTML:** egyetlen, `data:` URI-kkal feltöltött HTML-fájl generálása. Egyszerű, de a
  szkriptek nem futnak, és nagy oldalaknál a fájl mérete megnő.

Az MHTML tehát az **archív/alap** formátum, a visszaállítás útja a Fázis 0 mérésétől függ.

### 5.3. Lemez-elrendezés

```
<userdata>/
  antikolt.db                      # SQLite (WAL)
  blobs/ab/cd/<sha256>.zst         # tartalom szerint címzett blobok (mhtml, text, webp, storage-json)
  containers/<container_id>/       # a motor partíciója (user-data-dir) — a motor kezeli
  home/                            # a kezdőlap jelenlegi verziója + verziótörténet
  logs/                            # helyi napló, rotálva, titok nélkül
  cache/                           # eldobható (előnézet-dekódolás, index)
```

**Kvóta és szemétgyűjtés:** alapértelmezett globális kvóta **50 GB**, kártyánként **100 MB** (a
legutóbbi 2 pillanatkép marad). A gyűjtő tétlen időben fut: LRU sorrend, `archived` kártyáknál agresszívebb.
A felhasználó látja a foglalt helyet kártyánként és összesen, és kézzel is törölhet.

### 5.4. Tartósság és hibatűrés

- Írás: blob → `fsync` az ideiglenes fájlra → **atomikus átnevezés** → SQLite rekord.
- A folyamat összeomlása nem hagyhat fél kártyát: a rekord csak a teljes blob után íródik, a manifest
  integritás-hash-t tartalmaz.
- Ha a mentés 2,5 másodperc alatt nem fejeződik be (nagyon nehéz oldal), akkor **részleges mentés**
  készül: előnézet + görgetés + URL; a felület jelzi („gyorsított mentés"). A felhasználót nem blokkoljuk.
- A `freezing` közben a kártya nem nyitható meg újra (a felület letiltja, amíg a mentés fut).

## 6. Visszaállítás

Három szinten, hogy a kártya megnyitása azonnali legyen:

1. **R0 — azonnali (0 ms):** az előnézeti kép megjelenik felnagyítva, „betöltés" jelzéssel.
2. **R1 — offline (100–400 ms):** az MHTML betöltése lemezről. Az oldal **statikusan, hálózat nélkül**
   olvasható, görgetéssel a mentett pozícióban; fejléc-sáv: „Offline pillanatkép · Élő betöltés".
   Ez egyben **offline olvasómód** — repülőn, lassú hálón is működik.
3. **R2 — élő (300–1200 ms):** navigálás a valódi URL-re, majd:
   - `sessionStorage` visszaírása `Page.addScriptToEvaluateOnNewDocument`-nel (az oldal saját szkriptjei
     előtt fut le);
   - `history.scrollRestoration = 'manual'`;
   - görgetés-visszaállítás **horgonnyal**: addig ismételjük (`requestAnimationFrame`, max. 5 s), amíg a
     `scrollHeight` stabil nem lesz, majd a horgony-elemre állunk — így a végtelen görgetésű oldalakon is
     a helyes pozícióba érünk;
   - nem titkos űrlapértékek visszaírása izolált világból (a titkosakat a széf kínálja fel);
   - média: `currentTime` beállítás + `pause`.

**Melyik szint aktiválódik?** Beállítás: `mindig élő` (alapértelmezett, gyors hálón), `először offline,
kattintásra élő`, `csak offline` (a kártya „pillanatkép-olvasóvá" válik). A kezdőlapról nyitott kártya
alapból R0 → R2; a „pillanatkép megnyitása" gomb mindig R1.

## 7. Adatmodell (SQLite)

`WAL`, `synchronous=NORMAL`, `foreign_keys=ON`, `busy_timeout=5000`. Minden idő `INTEGER` (Unix ms, UTC).
Azonosítók: ULID-szerű szövegek (időrendezhető). Sémaverzió: `PRAGMA user_version`, migrációk
`migrations/0001_init.sql` stb.

```sql
-- Munkaterületek (a kezdőlap fülei/szekciói)
CREATE TABLE workspace (
  id TEXT PRIMARY KEY, name TEXT NOT NULL, icon TEXT,
  position INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL, deleted_at INTEGER
);

-- Identitás-konténerek
CREATE TABLE container (
  id TEXT PRIMARY KEY, name TEXT NOT NULL,
  color TEXT NOT NULL,                    -- #RRGGBB
  icon TEXT,
  partition_key TEXT NOT NULL UNIQUE,     -- 'persist:c_<id>'
  proxy_json TEXT,                        -- {mode:'none'|'http'|'socks5', host, port, user}
  session_ttl_seconds INTEGER NOT NULL DEFAULT 86400,
  session_policy TEXT NOT NULL DEFAULT 'sweep',  -- keep|sweep|wipe
  wipe_on_app_lock INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL, deleted_at INTEGER
);

-- Kártyák (mentett oldalak)
CREATE TABLE card (
  id TEXT PRIMARY KEY,
  workspace_id TEXT REFERENCES workspace(id) ON DELETE CASCADE,
  container_id TEXT REFERENCES container(id) ON DELETE SET NULL,
  url TEXT NOT NULL,
  title TEXT NOT NULL DEFAULT '',
  favicon_ref TEXT,                        -- blob sha256
  notes TEXT,
  position INTEGER NOT NULL DEFAULT 0,
  layout_json TEXT,                        -- {x,y,w,h,size:'s'|'m'|'l'} a kezdőlapon
  theme_mode TEXT NOT NULL DEFAULT 'auto', -- auto|light|grey|dark|amoled
  theme_technique TEXT,                    -- kézi felülírás: T0..T6
  zoom REAL NOT NULL DEFAULT 1.0,
  state TEXT NOT NULL DEFAULT 'saved',     -- saved|warming|live|freezing|purging|archived
  pinned INTEGER NOT NULL DEFAULT 0,       -- soha ne hibernáljon
  session_policy TEXT,                     -- NULL = a konténer politikája; keep|sweep|wipe
  login_required INTEGER NOT NULL DEFAULT 0,
  last_opened_at INTEGER, open_count INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL, deleted_at INTEGER
);
CREATE INDEX card_ws ON card(workspace_id, position);
CREATE INDEX card_recent ON card(last_opened_at DESC);

-- Pillanatképek (verziózott, tartalom szerint címzett blobokra mutat)
CREATE TABLE snapshot (
  id TEXT PRIMARY KEY,
  card_id TEXT NOT NULL REFERENCES card(id) ON DELETE CASCADE,
  created_at INTEGER NOT NULL,
  reason TEXT NOT NULL,                    -- switch|idle|close|crash|manual
  url TEXT NOT NULL, final_url TEXT NOT NULL, title TEXT,
  scroll_x INTEGER NOT NULL DEFAULT 0, scroll_y INTEGER NOT NULL DEFAULT 0,
  anchor_json TEXT,                        -- {selector, dx, dy}
  viewport_json TEXT NOT NULL,             -- {w,h,dpr,zoom}
  manifest_ref TEXT NOT NULL,              -- blob
  preview_ref TEXT,                        -- blob (webp)
  text_ref TEXT,                           -- blob (kivont szöveg)
  luminance REAL,                          -- 0..1 (sötét mód ellenőrzéshez)
  bytes INTEGER NOT NULL DEFAULT 0,
  engine TEXT NOT NULL,                    -- 'cef'
  integrity TEXT NOT NULL                  -- sha256 a manifestre
);
CREATE INDEX snapshot_card ON snapshot(card_id, created_at DESC);

-- Hitelesítő adatok (a titkok *_ct oszlopai AES-256-GCM-mel titkosítottak)
CREATE TABLE credential (
  id TEXT PRIMARY KEY,
  container_id TEXT REFERENCES container(id) ON DELETE SET NULL,
  origin TEXT NOT NULL,
  kind TEXT NOT NULL DEFAULT 'password',   -- password|totp|passkey
  label TEXT,
  username_ct BLOB, password_ct BLOB,
  totp_secret_ct BLOB, totp_params_json TEXT,  -- {digits,period,algorithm}
  passkey_ct BLOB,                          -- v1.1
  last_used_at INTEGER, created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL
);
CREATE INDEX credential_origin ON credential(origin, container_id);

-- Munkamenet-ledger: eredet (origin) szintű nyilvántartás, hogy mérhető legyen,
-- mit tartunk életben és mi a költsége (lásd 10-MUNKAMENET.md)
CREATE TABLE session (
  id TEXT PRIMARY KEY,
  container_id TEXT NOT NULL REFERENCES container(id) ON DELETE CASCADE,
  origin TEXT NOT NULL,
  state TEXT NOT NULL DEFAULT 'unknown',   -- logged_in|logged_out|expired|unknown
  policy TEXT NOT NULL DEFAULT 'sweep',    -- keep|sweep|wipe (örökölt a konténerből)
  cookie_count INTEGER NOT NULL DEFAULT 0,
  cookie_bytes INTEGER NOT NULL DEFAULT 0,
  storage_bytes INTEGER NOT NULL DEFAULT 0, -- localStorage + IDB + Cache Storage
  sw_count INTEGER NOT NULL DEFAULT 0,
  last_seen_at INTEGER,
  last_verified_at INTEGER,               -- utolsó igazolt bejelentkezés
  last_sweep_at INTEGER,
  live_ram_bytes INTEGER,                  -- mért érték, ha épp fut; NULL ha dormant
  keep_alive INTEGER NOT NULL DEFAULT 0,   -- opcionális éltetés (1 kérés / 15-60 perc)
  updated_at INTEGER NOT NULL
);
CREATE UNIQUE INDEX session_origin ON session(container_id, origin);

-- Oldalankénti recept (tanult beállítások)
CREATE TABLE site_recipe (
  origin TEXT PRIMARY KEY,
  theme_mode TEXT, theme_technique TEXT, zoom REAL,
  permissions_json TEXT,                   -- {camera:'deny', notifications:'ask', ...}
  form_hints_json TEXT,                    -- {selector: 'username'|'password'|'otp'}
  login_runbook_json TEXT,                 -- lépéssor (lásd 9.5)
  blocked_runbooks INTEGER NOT NULL DEFAULT 0,
  updated_at INTEGER NOT NULL
);

CREATE TABLE tag (id TEXT PRIMARY KEY, name TEXT NOT NULL UNIQUE, color TEXT);
CREATE TABLE card_tag (
  card_id TEXT NOT NULL REFERENCES card(id) ON DELETE CASCADE,
  tag_id  TEXT NOT NULL REFERENCES tag(id)  ON DELETE CASCADE,
  PRIMARY KEY (card_id, tag_id)
);

-- Helyi napló (titok nélkül), rotálva. Szándékosan nincs FK: a napló a kártya törlése után is marad.
CREATE TABLE event (
  id INTEGER PRIMARY KEY AUTOINCREMENT, at INTEGER NOT NULL,
  kind TEXT NOT NULL, card_id TEXT, detail_json TEXT
);

-- Teljes szöveges keresés: szóalapú ÉS részstringes (magyar ékezetek kezelésével)
CREATE VIRTUAL TABLE card_fts USING fts5(
  card_id UNINDEXED, title, url, notes, content,
  tokenize = "unicode61 remove_diacritics 2"
);
CREATE VIRTUAL TABLE card_fts_trigram USING fts5(
  card_id UNINDEXED, content, tokenize = "trigram"
);
```

**Miért két FTS-index?** A `unicode61 remove_diacritics 2` a magyar ékezeteket (á, é, ő, ű) is kezeli
szókeresésnél; a `trigram` a részstringes, gépelés közbeni kereséshez kell. Mindkettő helyi, nincs
hálózat.

**Keresési szintaxis:** `alma kenyér` (és), `"pontos kifejezés"`, `tag:munka`, `container:munka`,
`workspace:otthon`, `site:youtube.com`, `before:2026-01-01`, `is:pinned`.

## 8. Konténerek

- Minden konténer = egy motor-partíció (`persist:c_<id>`), saját süti-, tároló- és gyorsítótár-könyvtárral.
- **Izoláció:** két konténer között nincs átjárás; ugyanaz a domain két konténerben két külön fiók.
- **Törlés/origin-wipe:** a 24 órás szabály és a „munkamenet törlése" gomb origin-szintű.
- **Proxy:** konténerenként beállítható (a motor indítási paramétere, illetve CDP
  `Network.setProxyOverride` — motoronként ellenőrizni kell). Ez az egyik legerősebb megkülönböztető
  funkció: „munka konténer = céges VPN, magán konténer = otthoni hálózat".
- **Konténer-szín:** a vezérlősávban mindig látszik, melyik identitásban vagy (színkód + név), és a
  kezdőlapon a kártyák szegélye jelzi. Ez véd a „rossz fiókba posztolás" ellen.
- **Konténer-törlés:** a teljes partíciókönyvtár törlése, megerősítéssel, a hozzá tartozó credential-ökkel
  együtt (opcionálisan megőrizve).

### 8.5. Popupok, OAuth/SSO és bejelentkezési ablakok

A „mindenhol automatikusan bejelentkeztetés" legtöbbször **popupon** keresztül történik („Jelentkezzen be
Google-fiókkal"), ezért ez nem részlet, hanem alapszolgáltatás:

- A motor popup-kérését (CEF `OnBeforePopup`) a mag **valódi, ideiglenes ablakként** nyitja meg,
  **ugyanabban a partícióban** — különben a folyamat nem örökli a sütiket, és a bejelentkezés elakad.
- A popup **nem kártya**: bezárásakor megszűnik (nincs snapshot, nincs előnézet). A felhasználó
  „kártyaként mentheti", ha mégis meg akarja tartani.
- `window.opener` és a `redirect_uri` visszairányítás működik; az SSO-teljes lánc (`accounts.google.com`
  → vissza a szolgáltatásra) egyetlen konténeren belül marad.
- A popup az élő nézet életciklusát követi: ha a motorház leáll, a popup is megszűnik. Ezért ha
  hitelesítés van folyamatban, a böngésző **nem állítja le** a motort, illetve figyelmeztet.
- Ez az **I1 invariáns egyetlen, dokumentált kivétele**: rövid ideig 2 renderelő fut (kártya + popup),
  legfeljebb 60 s-ig; a mérőpad ezt külön kezeli.

## 9. Széf, automatikus kitöltés, TOTP

### 9.1. Kulcshierarchia

```
DEK (32 bájt, véletlen)  ──AES-256-GCM──▶  minden titok (credential, TOTP, passkey)
   ▲
   ├─ wrap #1: rendszer kulcstartó (Windows DPAPI / Linux Secret Service), alkalmazás-specifikus entrópiával
   └─ wrap #2: opcionális mesterjelszó → Argon2id (m=64 MiB, t=3, p=4) → AES-256-GCM kulcs
```

- Két wrap lehetővé teszi: gyors nyitás a rendszerkulccsal, **hordozhatóság** mesterjelszóval (export/import,
  másik gép), és kulcsrotációt.
- A DEK csak a Rust mag memóriájában él, feloldás után; zároláskor nullázódik. Auto-zárolás:
  5 perc tétlenség vagy a képernyő zárolása (Windows `WM_WTSSESSION_CHANGE`, Linux logind signal).
- Ha a rendszerkulcs nem elérhető (pl. Linuxon nincs Secret Service), a felület **figyelmeztet** és
  kéri a mesterjelszót — soha nem esik vissza csendben plaintextre.

### 9.2. Kitöltés (autofill)

- A szkript **izolált világban** fut (CDP `Page.createIsolatedWorld`), így az oldal JS-e nem látja a
  változóit, csak a DOM-változásokat.
- A kitöltés **natív setterrel** történik (`Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,
  'value').set.call(el, v)`) + `input`/`change`/`blur` események — különben a React/Vue-alapú oldalak nem
  veszik észre.
- A választó felület **zárt Shadow DOM**-ban jelenik meg (az oldal nem tudja átstilizálni, és a zárt
  shadow tartalmát az oldal JS-e nem olvassa).
- **Origin-kötés:** kitöltés csak egyező originre (IDN/punycode normalizálás után), és csak top-level
  vagy azonos-origin iframe-be; cross-origin iframe-be soha. Gyanús esetben (homográf domain) figyelmeztet.
- **Mentés:** űrlap elküldésekor / navigációkor felajánlja a mentést; a jelszó csak titkosítva tárolódik;
  a „jelszó megváltozott" észlelésekor frissítést kér.

### 9.3. TOTP

RFC 6238 szerint (SHA-1/6 jegy/30 s az alap; SHA-256/8 jegy/60 s is támogatott). A kódot a mag számolja,
a felület visszaszámláló gyűrűvel mutatja, és **vágólapra másolás után 30 s-mal törli a vágólapot**
(ha a vágólap még ugyanazt tartalmazza). Óraeltérés-kezelés: a szerveridőhöz igazítás lehetősége.

### 9.4. Amit a széf soha nem tesz

- Nem küld semmit hálózatra (nincs felhő-szinkron v1-ben).
- Nem naplóz titkot, nem tesz titkot a kezdőlap bridge-ére, nem ad titkot a fővilágnak.
- Nem tölt ki jelszót `http://` oldalra (csak `https://` és `localhost`).
- Nem tölt ki felhasználói mozdulat (kattintás/billentyű) nélkül.

### 9.5. Bejelentkezési forgatókönyv (login runbook)

Deklaratív lépéssor kártyánként (a `site_recipe.login_runbook_json`):

```json
{
  "version": 1,
  "steps": [
    {"op": "goto", "url": "https://example.com/login"},
    {"op": "wait_for", "selector": "input[type=email]", "timeout_ms": 8000},
    {"op": "fill", "selector": "input[type=email]", "from": "credential.username"},
    {"op": "click", "selector": "button[type=submit]"},
    {"op": "wait_for", "selector": "input[type=password]", "timeout_ms": 8000},
    {"op": "fill", "selector": "input[type=password]", "from": "credential.password"},
    {"op": "click", "selector": "button[type=submit]"},
    {"op": "wait_for", "selector": "input[autocomplete=one-time-code]|iframe[title*=captcha]", "timeout_ms": 15000},
    {"op": "fill_totp", "selector": "input[autocomplete=one-time-code]", "from": "credential.totp"}
  ],
  "on_failure": "hand_over_to_user"
}
```

A végrehajtó **max. 20 másodpercig** fut, nem kattint semmire, ami fizetés/törlés/posztolás (blokkolt
művelet-lista), és bármikor megszakítható. Ha CAPTCHA-t lát, átadja a felhasználónak. Ez az a
kompromisszum, ami a „mindenhol bejelentkeztet" ígéretből **biztonságosan** megvalósítható.

### 9.6. Munkamenetek és a bejelentkezés költsége

A széf mellett külön alrendszer a **munkamenet-ledger**: minden `konténer × origin` párról nyilvántartja,
hogy be van-e jelentkezve, mekkora a lemezes lábnyoma, van-e Service Workere, milyen politikát követ, és
mennyi RAM-ot használ, ha éppen fut. A felület **Munkamenetek** panelje ezt mutatja, és kimondja, hogy
egy bejelentkezés életben tartása **0 RAM-ba** kerül, amíg a kártya hibernált — a valódi költség a
meleg motorház, a háttérben életben tartott kapcsolat, illetve a Service Workerek.

A teljes elemzés, a „mit veszítünk" tábla és az éltetés (keep-alive) szabályai: `10-MUNKAMENET.md`.

## 10. Téma-motor: világos / szürke / sötét / AMOLED + sötétítési technika-létra

### 10.1. A felület (saját UI)

Négy téma, design-tokenekkel (CSS-változók + QML paletta): `light`, `grey` (semleges, közepes fényerejű —
ez az alapértelmezett, mert nappal és este is jó), `dark`, `amoled` (tiszta fekete, OLED-hez).
Kártyánként felülírható; a világos/szürke/sötét váltás a vezérlősávból egy kattintás.

### 10.2. Oldalak sötétítése — a létra

| Szint | Technika | Mikor nyer |
|---|---|---|
| **T0** | `prefers-color-scheme: dark` emulálása | az oldal natívan támogatja a sötétet (a legjobb eredmény) |
| **T1** | Chromium beépített automatikus sötétítése | gyors, közepes minőség oldalakon |
| **T2** | Generált stíluslap (Dark Reader-szerű motor, MIT licencű alapból adaptálva) | a legtöbb híroldal, blog, dokumentáció |
| **T3** | Invertálás + kép/videó visszainvertálás | régi, statikus oldalak |
| **T4** | Kényszerített kontraszt (`forced-colors`) | akadálymentesített/egyszerű oldalak |
| **T5** | Tompító/dimmelő réteg (fényerő, kontraszt) | bármikor, utolsó fogásként |
| **T6** | Olvasómód (saját, témázott cikk-nézet) | cikkek, szürke módhoz ideális |

A **szürke mód** az oldalakon: színtelenítés + enyhe tompítás (`grayscale(0.9) brightness(0.92)`),
opcionálisan T6-tal kombinálva — ez a „figyelemelterelés nélküli" olvasás.

### 10.3. Önellenőrzés (a lényeg, ami a Gemini-tervből hiányzik)

Minden sötétítés után a motor **képernyőképet** ad, amiből kiszámítjuk az **átlagos relatív fényerőt**
(32×32-re kicsinyítve). Ha a kívánt mód (sötét) után a fényerő > 0,45, a böngésző **fellép a következő
technikára**. Az eredményt `site_recipe`-ben tároljuk, így a következő megnyitáskor már a nyertes technika
indul. Ez mérhető, tesztelhető és oldalanként tanul — nem hit kérdése.

Emellett rögzítjük a „tört" eseteket (pl. a technika után eltűnt a tartalom: a DOM-elemek száma drasztikusan
csökkent) → automatikus visszalépés az előző technikára + jelzés a felületnek („ez az oldal nem szereti a
sötét módot").

## 11. Kezdőlap: a kártyarács mint szerkeszthető weboldal

### 11.1. Modell

- A kezdőlap egy **helyi HTML-dokumentum** (`antikolt://home`), amit a böngésző saját maga szolgál ki
  fájlrendszerből. Alapértelmezésben a beépített **kártyarács-sablon**.
- **A „szerkeszthetőség" a kezdőlapra (a könyvtárra) vonatkozik, nem idegen weboldalak tartalmára.** Egy
  kártya tartalma a weboldal sajátja (illetve a mentett pillanatképe); a felhasználó a *könyvtárat*, a
  kártyák elrendezését, megjelenését, jegyzeteit szerkeszti — az idegen oldal DOM-ját nem. (Kivétel: a
  felhasználói szkriptek kártyánként, későbbi fázisban.)
- A kártyák **web-komponensként** jelennek meg: `<antikolt-card card-id="01J...">`, amit a felhasználó
  bárhova beilleszthet (rácsba, szekcióba, két oszlopba, saját dizájnba).
- A felhasználó a kezdőlapot **belső szerkesztővel** módosíthatja (HTML/CSS/JS, előnézettel, visszavonással,
  verziótörténettel), vagy **külsőleg** megírhatja és behúzhatja a saját fájlját.
- A kezdőlap-változatok verziózottak (`home/versions/`), visszaállíthatók, exportálhatók egyetlen
  `.antikolt-home` fájlba (HTML + elrendezés JSON + előnézetek nélkül).

### 11.2. Bridge API (a kezdőlap és a mag között)

A kezdőlap **nem kap** fájlrendszer- vagy hálózati hozzáférést; csak ezt a szűk API-t:

```
antikolt.cards.list({workspace, filter, sort}) -> CardMeta[]
antikolt.cards.get(cardId) -> CardMeta
antikolt.cards.open(cardId, {mode})           // élő | offline pillanatkép
antikolt.cards.setLayout(cardId, layout)
antikolt.cards.create({url, title, ...})
antikolt.search(query) -> CardMeta[]
antikolt.workspaces.list() / .switch(id)
antikolt.containers.list()
antikolt.events.on('open'|'close'|'preview-updated', cb)
```

Ez `postMessage`-en megy, a mag oldalán **allowlist**-tel és séma-ellenőrzéssel. A kezdőlap nem tud
tetszőleges natív műveletet kérni.

### 11.3. Biztonság

- CSP: `default-src 'none'; img-src data: antikolt:; style-src 'unsafe-inline'` — **távoli szkript nem
  tölthető be** a kezdőlapra (kivéve, ha a felhasználó kifejezetten engedélyezi egy adott originre).
- Beillesztett HTML sanitizálása (nem megbízható forrásból származó tartalom).
- A kezdőlap a motorban fut, de **más partícióban** (dedikált `persist:c_home`), hogy a sütik ne
  keveredjenek; hálózat felé alapból offline.
- A kezdőlap összeomlása nem viheti magával a böngészőt; ha a kezdőlap hibás, a böngésző a beépített
  sablonra esik vissza és jelzi a hibát.

## 12. Előnézet-pipeline

- **Mikor készül:** kártya fagyasztásakor, kártya megnyitásakor (frissítés), és az ütemező szerint.
- **Hogyan:** `capture` → 260×150 → WebP (q=80) → blob + `luminance` + `dhash` (azonos képek kiszűrése).
- **Ütemező (a „nincs háttérmotor" elv megtartásával):** csak ha (a) a felhasználó a kezdőlapon van,
  (b) nincs gépelés/egérmozgás 30 s-ig, (c) nem fut más motorfeladat, (d) a kártya előnézete > 7 napos,
  (e) opcionálisan csak töltőn. Egyszerre **egy** kártya, 8 s időkorlát, utána azonnal fagyasztás.
- **Korlát:** napi max. 100 frissítés, és a felhasználó kikapcsolhatja.
- **Kezdőlap-oldali követelmény:** virtualizált rács, csak a látható kártyák DOM-ban; dekódolt
  bittérkép-plafon (pl. 24 kép); előnézet nélküli kártya helyőrzővel.

## 13. Keresés

- Indexelt mezők: cím, URL, jegyzet, címkék, kivont oldalszöveg (max. 256 KB/kártya).
- Két FTS5 index (szó + trigram), lásd 7. fejezet.
- Cél: 10 000 kártya fölött is **50 ms** alatti válasz, memóriában tartott index nélkül (SQLite lap-cache).
- A keresés a kezdőlap bridge-én keresztül érhető el, és a `Ctrl+K` gyorskereső is ezt használja.

## 14. Biztonsági modell

**Fenyegetések:** rosszindulatú weboldal; rosszindulatú letöltött fájl; a kezdőlapra beillesztett idegen
HTML; más helyi folyamat, ami a böngésző adatait olvasná; hálózati támadó; ellopott laptop.

**Ellenintézkedések:**

| Terület | Intézkedés |
|---|---|
| Oldalak | motor saját sandboxa + site isolation; nincs natív hozzáférés; engedélyek origin+container szerint |
| Kitöltő | izolált világ, zárt Shadow DOM, origin-kötés, felhasználói mozdulat |
| Titkok | AES-256-GCM, DEK/KEK, Argon2id opció, auto-zárolás, vágólap-tisztítás |
| Kezdőlap | szigorú CSP, sanitizálás, allowlist bridge, külön partíció |
| Letöltések | külön mappa, „futtatás" tiltása, Windows Defender opcionális vizsgálat, megerősítés |
| Tanúsítványok | hiba = kemény leállás, felülírás csak kártyánként, naplózva |
| IPC | helyi socket, `0600`, üzenetséma-validáció, nem megbízható bemenet kezelése |
| Adatvédelem | nincs telemetria, nincs felhő, helyi napló rotálva, titok nélkül |
| Kriptográfia | rendszerkönyvtár (OS CSPRNG, DPAPI, Secret Service), nem saját implementáció |

## 15. Platform-réteg

| Terület | Windows 11 (v1) | Nobara/Fedora + KDE (v0.3) |
|---|---|---|
| Ablak | Qt 6 + natív CEF-ablak (`SetParent`) | Qt 6 + OSR (Wayland) / natív (X11) |
| Skálázás | DPI-tudatos, 125–200% tesztelve | Qt high-DPI + frakcionális skálázás |
| Kulcstartó | DPAPI | Secret Service (KWallet/gnome-keyring) |
| Értesítés | Windows toast | `org.freedesktop.Notifications` |
| Csomagolás | MSI/NSIS telepítő | RPM, később Flatpak |
| Frissítés | saját, aláírt frissítő | RPM repo / Flatpak |

**Wayland-kockázat:** a natív ablak-beágyazás az `xdg-foreign` protokolltól függ, ami kompozitoronként
eltérő; ezért Linuxon az OSR az elsődleges út, a natív beágyazás csak X11 alatt.

## 16. Motor-adapter és a jövőbeli motorok

- `EnginePort` (3.1.) + `EngineSurface` (csatolás) + eseménycsatorna. A mag csak ezt ismeri.
- **Motor #1: CEF** (Chromium). Előny: teljes kontroll, CDP, stabil, BSD licenc.
- **Motor #2 jelölt: WebKitGTK** (Linux) — kisebb memórialáb, jó „light mód"; hátrány: eltérő
  kompatibilitás, OSR-központú integráció.
- **Motor #3 jelölt: WebView2** (Windows) — rendszerrel száll, kicsi telepítő; hátrány: kevés kontroll
  (DevTools, popup, profil), ezért csak „kompatibilitási motor" szerepre.
- **Kizárva:** Gecko (nincs beágyazási API), Servo és Ladybird (2026-ban nem termékképesek beágyazásra).
- A motor kiválasztása kártyánként/konténerenként lehetséges (pl. „ezt az oldalt WebKitGTK-val nyisd"),
  de az alapértelmezett a CEF. A motorváltás **új snapshotot** készít (a két motor DOM-ja eltér).

## 17. Nem-funkcionális követelmények

| Követelmény | Cél | Mérés |
|---|---|---|
| Indulás a kezdőlapra | ≤ 1,5 s hideg, ≤ 0,6 s meleg | mérőpad |
| Kártyanyitás érzékelt ideje | ≤ 600 ms (R0 azonnal) | mérőpad |
| Fagyasztás (mentés) | ≤ 2,5 s, NAGY oldalnál is | mérőpad |
| Keresés | ≤ 50 ms / 10 000 kártya | benchmark |
| Memória | lásd `03-MEMORIA.md` | CI-kapu |
| Lemez (alap kvóta) | 50 GB, kártyánként 100 MB | admin felület |
| Adatbázis méret | 10 000 kártyánál < 200 MB (a blobok nélkül) | benchmark |
| Akadálymentesség | billentyűzetes teljes navigáció, képernyőolvasó-támogatás | kézi teszt |
| Nyelvek | HU + EN (v1) | i18n teszt |

### 17.1. Nyitott függőségek (amit a terv feltételez, de még nem bizonyított)

Ezek a pontok **kockázatok, nem döntések** — a Fázis 0-nak ezeket kell igazolnia vagy cáfolnia:

1. **MHTML betölthetősége** a motorban (a készítése biztos) — lásd 5.2. tartalékok.
2. **Popupanatómia CEF-fel**: `OnBeforePopup` + azonos partíciójú ideiglenes ablak, `window.opener`
   működéssel, osszági bejelentkezési láncokkal.
3. **Izolált világ** elérése a motoron (CDP `Page.createIsolatedWorld` vagy a motor saját API-ja) — ezen
   áll a kitöltő biztonsága.
4. **Originenkénti adattörlés** a partícióban (CDP `Storage.clearDataForOrigin` pontosan azt törli-e, amit
   hiszünk, és a futó oldal mit lát belőle).
5. **A belépési forgatókönyv** DOM-függőségei: a szelektorok oldalváltozásra törnek — ezért kell a
   „kézi átadás" és a `blocked_runbooks` számláló.
6. **A luminancia-mérés** valós költsége (kép → 32×32 → átlag): a terv szerint elhanyagolható, de mérni kell.
7. **A kezdőlap (HTML) súlya**: a felhasználó saját kezdőlapja hány MB-ot foglal a renderelőben.
8. **Az I1 invariáns** tartása a kezdőlap és a kártya között (a kezdőlap renderelőjét a kártya nyitásakor
   meg kell szüntetni, és vissza kell állítani záráskor — ez a „raszter-kezdőlap" miatt olcsó).
9. **Natív ablak-beágyazás** stabilitása (fókusz, IME, DPI-váltás, alt-tab) Windows 11-en.
10. **Argon2id** választott paraméterei (64 MiB / 3 / 4) a gyorsulva öregedő hardveren — 2 évente felülvizsgálandó.

## 18. Repó-szerkezet és technológia
```
antikolt/
  apps/desktop-shell/          # C++20 + Qt 6 (QML), CEF-integráció, platform réteg
  crates/antikolt-core/        # Rust: db, vault, lifecycle, snapshot, search, preview
  crates/antikolt-ipc/         # üzenetséma (serde + rmp), verziózás, tesztek
  crates/antikolt-engine-host/ # a motorház futtatható (CEF), később más motorok
  home/                        # beépített kezdőlap (HTML/CSS/JS) + sablonok
  proto/                       # IPC-séma leírás + generált kód
  migrations/                  # SQL migrációk
  perf/                        # mérőpad, baseline-ok, riportok
  tests/e2e/                   # Playwright/Qt-teszt szcenáriók
  docs/{HU,EN,forras}
```

| Réteg | Választás | Indok |
|---|---|---|
| Mag | Rust (stable), `rusqlite` vagy `sqlx` (SQLite), `serde`, `zstd`, `image`, `argon2`, `aes-gcm` | biztonság, determinizmus |
| Héj | C++20, Qt 6 (QML), CMake | CEF C++-hoz illeszkedik; QML gyors UI |
| Motor | CEF (Chromium) + CDP | kontroll, snapshot, téma-emuláció |
| FFI | `cxx` | típusbiztos Rust↔C++ |
| Teszt | Rust unit + C++ unit (Catch2) + e2e (Playwright) + mérőpad | rétegenként |
| Csomagolás | CMake + CPack/NSIS (Win), RPM (Linux) | platform-natív |

## 19. Források (a terv alapjául szolgáló dokumentációk)

- CEF — https://github.com/chromiumembedded/cef
- Chrome DevTools Protocol (Page.captureSnapshot, Emulation.setEmulatedMedia, Storage.clearDataForOrigin) —
  https://chromedevtools.github.io/devtools-protocol/
- SQLite FTS5 — https://www.sqlite.org/fts5.html
- DPAPI — https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptprotectdata
- Secret Service API — https://specifications.freedesktop.org/secret-service/latest/
- Argon2 (RFC 9106) — https://www.rfc-editor.org/rfc/rfc9106.html
- TOTP (RFC 6238) — https://www.rfc-editor.org/rfc/rfc6238
- Qt 6 LGPL-kötelezettségek — https://doc.qt.io/qt-6/lgpl.html
- xdg-foreign (Wayland beágyazás) — https://wayland.app/protocols/xdg-foreign-unstable-v2
- MPL-2.0 — https://www.mozilla.org/en-US/MPL/2.0/
