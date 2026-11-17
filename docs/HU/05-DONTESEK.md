# 05 — Döntési napló (ADR)

Minden visszafordíthatatlan vagy drága döntés itt van rögzítve: **kontextus → döntés → következmény**.
A döntés megváltoztatása új ADR-t igényel (a régi „Elavult" állapotot kap).

## ADR-032 — A kártyalista a shellből tolódik a lapra, amíg a híd egyirányú (2026-11-17)

**Kontextus.** A rácsnak a mag kártyáit kell mutatnia. A lap viszont ma **nem tud kérdezni**: a híd
(`window.antikoltNative`) egyirányú, a `cefQuery` válaszútja nincs bekötve. Egy valódi kérés-válasz
körhöz a motor-oldalon perzisztens `CefMessageRouterBrowserSide::Callback` kell (a motornak tartania
kell a hívást, amíg a mag válaszol), a huzal-protokollon pedig egy új üzenet (`bridge_result`) — ami a
szerződés szerint **protokoll-verziót emel** (v1 → v2), és végig kell vinni a motoron, a shellen és a
magon.

**Döntés.** A kártyalista egyelőre **shell-kezdeményezett tolás**: a shell a kezdőlap betöltése után
elkéri a magtól a `cards_list`-et, és a lapnak **adatként** adja át
(`window.antikoltAcceptCards(JSON.parse("…"))`, a JSON a shellben escape-elt string-literal, nem
beszúrt kód). A lap `pageCardFromCore` adapterrel alakítja a mag rekordjait a saját modelljévé, a
keresés a lapon fut a betöltött listán, és `window.antikoltGridState()` a lap saját tanúja arról, mit
mutat. Ha nincs mag, a lap a mintakártyáit mutatja, és a státuszsor kimondja, miért.

**Következmény.** (+) a rács ma valódi adatot mutat, nulla protokoll-változással; (+) a lap-oldali
tanú (`antikoltGridState`) miatt a mérés nem a shell könyvelésére vagy képernyőképre hagyatkozik;
(−) a lap nem tud frissíteni/kérdezni (új kártya, átrendezés csak újratöltéskor látszik); a valódi
kétirányú híd a protokoll v2-vel jön, és akkor ez a tolás visszaminősül eseménnyé („a lista
megváltozott"). Részletek: `16-FEJLESZTESI-NAPLO.md`, tizenhatodik szelet.

## ADR-031 — A széf határa zártan viselkedik: nincs titkosítás, nincs titok (2026-11-17)

**Kontextus.** A sorrend (ADR-027) szerint a széf a következő szelet. Az ADR-010 szerinti titkosítás
(rekordonként AES-256-GCM, AAD = origin + rekord-azonosító, DEK a rendszer kulcstárában vagy Argon2id-del)
crate-eket igényel: `aes-gcm`, `argon2`, `zeroize`. Ebben a buildkörnyezetben **nincs crates.io elérés**
(`curl: (35) schannel: AcquireCredentialsHandle failed: SEC_E_NO_CREDENTIALS`), az offline registry
**14 crate**-et tartalmaz, egyetlen kriptográfiait sem; a `windows`/`winapi` crate sem elérhető, a crate
pedig `#![deny(unsafe_code)]`, tehát a platform kulcstár (DPAPI) FFI-vel sem hívható.

**Döntés.** A széf **alakja** most elkészül (zárás/nyitás, 5 perces idle auto-lock, `require_open` kapu,
`SecretKind`, `VaultStatus`), a titkosítás pedig **explicit varrat** mögé kerül (`SecretBox` trait),
amelynek a fában egyetlen implementációja van: `UnavailableSecrets`, ami **minden pecsételést és
feloldást megtagad** (`vault_locked` / `not_supported`). A `VaultStatus` kimondja, hogy ebben a buildben
`can_store: false`, és megadja az okot. **Nincs néma plaintext-fallback** — ez az ADR-010 egyetlen
megszeghetetlen szabálya —, és nincs kézzel írt AES.

**Következmény.** (+) a shell, a munkamenet-ledger és a passkey-folyamat már erre az alakra épülhet;
(+) a valódi kriptográfia egyetlen implementáció cseréje lesz, az API nem változik; (+) a 10 új teszt a
viselkedést rögzíti (4:59 nyitva, 5:00 zár, a visszafelé lépő óra nem hosszabbít, a varrat soha nem ad
vissza nyílt szöveget); (−) a széf **nem tud titkot tárolni**, amíg a crate-ek nincsenek bevendorolva —
ezt a felületnek ki kell mondania, nem elrejtenie. Részletek: `16-FEJLESZTESI-NAPLO.md`, tizenötödik
szelet.

## ADR-029 — Minden DevTools-hívás egy típuson megy át (2026-11-17)

**Kontextus.** A CEF DevTools-átvitele aszimmetrikus: `ExecuteDevToolsMethod` csak **elindít** egy
hívást, a válasz később, `OnDevToolsMethodResult`-ban érkezik, és a hívó csak az üzenet-azonosítóból
tudja, melyik válasz az övé. Az első két fogyasztó (MHTML-pillanatkép, JS-kiértékelés) ezért két külön,
majdnem azonos observer-osztályt kapott. A `capture` és a passkey (ADR-027) ugyanazt a hármat igényli:
egyedi azonosító, paraméter-szótár, válasz-feldolgozó.

**Döntés.** Egyetlen `DevToolsCall` típus (`apps/engine-host/src/devtools.{h,cpp}`): metódusnév +
paraméterek + válaszhívó. A **sorozatokat a válaszhívók láncolják** (`Page.enable` →
`Page.captureSnapshot` → `Runtime.evaluate`), nem egy állapotgép. A válasz **egyszer**, egy helyen
parse-olódik (`DevToolsReply`), és ez a típus **birtokolja a parse-olt JSON-dokumentumot**: a CEF belső
szótárai a dokumentumban élnek, és ha a dokumentum felszabadul, a szótár **üresen** jön vissza — nem
hibázik, hanem hazudik (üres `value`, `state_bytes=0`). Ez a hiba kétszer került elő (egyszer a közvetlen
observernél, egyszer amikor ez a típus elvette a dokumentum élettartamát a hívótól).

**Következmény.** (+) egy helyen dől el a válasz-parse, az azonosító-kiosztás és az observer élettartama;
(+) a `capture` és a WebAuthn-hívások ugyanezt ingyen kapják; (+) az `eval` és a pillanatkép
`state.json`-ja újra működik. (−) a válasz JSON-je a feldolgozó lefutásáig a memóriában marad (néhány KB;
a pillanatképnél a base64 dekódolás után a hívó már nem hivatkozik rá). Részletek:
`16-FEJLESZTESI-NAPLO.md`, tizennegyedik szelet.

## ADR-030 — A festést a motor saját képével mérjük, nem a képernyőről (2026-11-17)

**Kontextus.** A korábbi festés-ellenőrzés a képernyőről készített másolatot (`QScreen::grabWindow`), és
ugyanaz a bináris ugyanazon a kódon hol 25 színt, hol 1 színt mért (tizenharmadik szelet). A képernyőkép a
**desktopot** méri, nem a motort: egy takart, minimalizált vagy más kompozitor-úton rajzolt ablak
egyszínű képet ad, miközben a lap hibátlanul kirajzolódott.

**Döntés.** A festés mértéke a motor saját képe: CDP `Page.captureScreenshot` a `capture` protokoll-opon
keresztül, PNG-ben, a fájl mellé írt valódi képmérettel. A képernyőkép csak **diagnosztika** marad (nem
dönt átmenetről), és a `capture` ugyanaz az út, ami a rács **előnézeteit** is adja — nem teszt-segédeszköz.

**Következmény.** (+) determinisztikus mérés (a kompozitor pixelei, a lap saját viewportja a független
tanú); (+) az előnézet-pipeline ugyanezen a primitíven épül; (−) a takart/önálló motorablak képe üres
lehet, ezért az előnézet-készítésnek látható felületet kell adnia (mért megfigyelés: önálló, takart
ablaknál 1 szín, beágyazott kártyánál 25). A mérés a `--capturetest` és a `--embedtest` önellenőrzésben
él.

## ADR-027 — A passkey a v1.0 része, nem v1.1 (2026-11-17)

**Kontextus.** A felhasználónak vannak regisztrált passkey-ei (az Ügyfélkapu is erre épül), és
kimondta: a passkey fontos. A korábbi sorrend (v1.1 szoftveres passkey) a passkey nélküli v1.0-hoz
vezetett volna, ami a felhasználó számára nem „elég jó böngésző", hanem egy olyan, amibe nem tud
belépni a saját fiókjaiba.

**Döntés.** A **szoftveres passkey v1.0-hatókör**, a `12-PASSKEY.md` „B" útja szerint:
CEF + CDP `WebAuthn.enable` / `WebAuthn.addVirtualAuthenticator` (CTAP2, platform authenticator,
`automaticPresenceSimulation`), a hitelesítő adatok a **széfben** tárolva
(`WebAuthn.getCredentials` / `setCredential`), a széf feloldásához kötve. Hardveres kulcs (FIDO2 USB)
továbbra is későbbi: a CEF-nek nincs beépített WebAuthn-je, azt a CDP-úton kell adni.

**Következmény.** A `capture`/DevTools réteg kap egy új fogyasztót, és a széf (ADR a titkosított
tárról) **nem tolható** a passkey után — a kettő egy szeletben kell. A sorrend tehát:
DevTools-infrastruktúra → széf → passkey → autofill/TOTP.

## ADR-028 — A lemezkvóta állítható, alapértéke 50 GB (2026-11-17)

**Kontextus.** A korábbi válasz: „inkább 50 GB, de felülbírálható". A felhasználó most pontosított:
**legyen állítható**.

**Döntés.** A kvóta **felhasználói beállítás** (eszközönként), alapértéke 50 GB, felső korlát a szabad
lemezterület mínusz biztonsági sáv; a beállítás a magban él (nem a shellben), mert a mag dönt a
hibernálásról. A kvóta túllépése nem hiba, hanem **leszorítási szabály**: a legrégebben megnyitott
kártyák törlődnek először (LRU), a rögzítettek (pinned) utoljára.

**Következmény.** A `03-MEMORIA.md` költségvetése és a `session`/`snapshot` nyilvántartás egy
`quota_bytes` beállítást kap; a shell csak megjeleníti.

| # | Döntés | Állapot |
|---|---|---|
| ADR-001 | Egyetlen élő webes nézet | Elfogadva |
| ADR-002 | Natív Rust mag + Qt 6 héj + CEF motor (nem Electron) | Elfogadva |
| ADR-003 | Lemezhibernálás MHTML + meta (nem screenshot+görgetés) | Elfogadva |
| ADR-004 | Külön motorház folyamat (T1/T2 mód) | Elfogadva |
| ADR-005 | Motor-adapter 7 primitívvel + MessagePack IPC | Elfogadva |
| ADR-006 | Windows: natív ablak; Linux: OSR | Elfogadva |
| ADR-007 | SQLite + WAL + két FTS5 index | Elfogadva |
| ADR-008 | Tartalom szerint címzett blob-tár kvótával | Elfogadva |
| ADR-009 | 10 perc / 24 óra, origin-szintű törléssel, felülírható | Elfogadva |
| ADR-010 | Széf: DEK/KEK + AES-256-GCM + opcionális Argon2id | Elfogadva |
| ADR-011 | HTML kezdőlap szűk bridge API-val | Elfogadva |
| ADR-012 | Téma-létra + luminancia-ellenőrzés + szürke mód | Elfogadva |
| ADR-013 | Előnézet-frissítés ugyanazzal a motorral, ütemezve | Elfogadva |
| ADR-014 | Licenc MPL-2.0, Qt dinamikus linkeléssel | Elfogadva |
| ADR-015 | Nulla telemetria | Elfogadva |
| ADR-016 | Egy ablak, nincs bővítményrendszer (v1) | Elfogadva |
| ADR-017 | Kód és commit angolul, dokumentáció HU + EN | Elfogadva |
| ADR-018 | Asztali böngésző-alapfunkciók a v1 kötelező részei | Elfogadva |
| ADR-019 | Munkamenet-ledger: a bejelentkezések mérhető életben tartása | Elfogadva |
| ADR-020 | Erőforrás-profilok (Gyors/Kiegyensúlyozott/Takarékos/Ultra/Kézi) | Elfogadva |
| ADR-021 | A fejlesztést AI-ágens végzi — minőségbiztosítási szabályokkal | Elfogadva |
| ADR-022 | Csatolási mód: Windows natív, Linux OSR; OSR-spike az F0-ban | Elfogadva |
| ADR-023 | Passkey-stratégia: v1.0 nélküle, v1.1 szoftveres hitelesítő a széfben | Elfogadva |
| ADR-024 | Kódaláírás: SignPath Foundation + Ed25519 frissítés-aláírás | Elfogadva |
| ADR-025 | Lemezkvóta: 50 GB alap, gépenként felülbírálható | Elfogadva |
| ADR-026 | Közreműködés és biztonsági kormányzás (DCO, védett területek, fuzz, SECURITY.md) | Elfogadva |
| ADR-027 | Passkey a v1.0-ben: CDP virtuális hitelesítő, a hitelesítő adatok a széfben | Elfogadva |
| ADR-028 | Lemezkvóta: állítható, alapértéke 50 GB, LRU-leszorítással | Elfogadva |
| ADR-029 | Minden DevTools-hívás egy `DevToolsCall` típuson; a válasz birtokolja a JSON-dokumentumot | Elfogadva |
| ADR-030 | Festés-mérés a motor saját képével (`capture`); a képernyőkép csak diagnosztika | Elfogadva |
| ADR-031 | Széf: a titkosítás varrata zártan viselkedik (nincs crate → nincs titok, nincs plaintext) | Elfogadva |
| ADR-032 | A kártyalista shell-tolással jut a lapra, amíg a híd egyirányú (kétirányú híd = protokoll v2) | Elfogadva |

---

## ADR-001 — Egyetlen élő webes nézet

**Kontextus:** a cél az 1 GB alatti memória; a hagyományos fülmodell minden fülhöz renderelőt tart.
**Döntés:** az alkalmazásban egyszerre legfeljebb egy webes renderelő fut (oldal vagy kezdőlap).
**Következmény:** (+) a legnagyobb memória-nyereség; (+) egyszerű életciklus. (−) nincs háttérben
zajló oldal (zene, értesítés, feltöltés); (−) a kártyaváltás mindig hibernálás+visszaállítás — ezt a
lemezre írás és az azonnali előnézet kompenzálja. **Egyetlen, dokumentált kivétel:** a felhasználó által
indított bejelentkezési popup (OAuth/SSO) ideiglenesen második renderelőt nyit, legfeljebb 60 s-ig
(`02-ARCHITEKTURA.md` 8.5.). **Ellenpélda, amit tudatosan vállalunk:** a háttérzene-lejátszás nem működik;
ha ez később kell, kizárólag egyetlen „kitűzött média" nézet kaphat kivételt (új ADR).

## ADR-002 — Natív Rust mag + Qt 6 héj + CEF motor (nem Electron)

**Kontextus:** az Electron alapköltsége (~250–350 MB üresen, Node + saját renderelő) nem fér bele a
célba; a többmotoros követelmény pedig Electron alatt nem teljesíthető.
**Döntés:** a domain logika Rustban (`antikolt-core`), a héj C++20 + Qt 6 (QML), a motor CEF (Chromium),
a kettő között `cxx` FFI. A motor külön folyamatban, IPC-n keresztül.
**Következmény:** (+) ~150–250 MB-tal kisebb alap, teljes motorkontroll, CDP, többmotoros út nyitva;
(−) jóval több integrációs munka, C++ és CEF ismerete kell; (−) nincs npm-ökoszisztéma.

## ADR-003 — Lemezhibernálás MHTML + meta (nem screenshot + görgetés)

**Kontextus:** a „pillanatkép + görgetés" visszaállítás elveszíti az űrlapokat, az SPA-állapotot és a
betöltött tartalmat; a felhasználói döntés: „őrizzen meg mindent, de ne RAM-ban".
**Döntés:** fagyasztáskor `Page.captureSnapshot` (MHTML) + meta (görgetés, horgony, viewport, média,
sessionStorage) + előnézet + kivont szöveg kerül lemezre, tömörítve, tartalom szerint címkezve.
**Következmény:** (+) a legjobb visszaállítási hűség, ami memória nélkül lehetséges; (+) offline
olvasás és teljes szöveges keresés ingyen; (−) lemezterhelés (1–40 MB/kártya) — kvótával kezelve;
(−) a JS-futásidejű állapot így sem menthető (dokumentált korlát).

## ADR-004 — Külön motorház folyamat (T1/T2)

**Kontextus:** a Chromium alapköltsége akkor is megvan, ha nincs oldal; a többmotoros csatolás is külön
folyamatot kíván.
**Döntés:** a motor `antikolt-engine-host` futtatható, IPC-n keresztül; T1 = melegen tartva, T2 = teljesen
leállítva (ultra-low mód).
**Következmény:** (+) 150 MB-os tétlen állapot elérhető; (+) motorcsere/összeomlás-izoláció ingyen;
(−) hidegindítás 0,6–1,2 s (hover-prewarm mal enyhítve); (−) összetettebb IPC és életciklus.

## ADR-005 — Motor-adapter 7 primitívvel + MessagePack IPC

**Kontextus:** a motorok cserélhetősége csak szűk, jól definiált interfésszel lehetséges.
**Döntés:** `create / navigate / eval / scroll / capture / snapshot / destroy` (+ 3 kiegészítő), helyi
socket, hossz-előtagolt MessagePack, `proto/v1` verziózás.
**Következmény:** (+) bármely motor implementálható; (+) a protokoll tesztelhető a motor nélkül;
(−) a motor-specifikus képességeket flag-ekkel kell jelezni; (−) a leggazdagabb funkciók (pl. bővítmények)
nem férnek bele a szerződésbe — tudatos korlát.

## ADR-006 — Windows: natív ablak; Linux: OSR

**Kontextus:** a Wayland nem teszi lehetővé idegen folyamat ablakának megbízható beágyazását; Windows-on
és X11-en viszont rutin.
**Döntés:** Windowson natív ablak-beágyazás (`SetParent`), Linuxon offscreen renderelés + saját
kompozitálás.
**Következmény:** (+) mindkét platformon jó út; (−) két csatolási út karbantartása; (−) az OSR CPU-igényes,
ezért Linuxon a médiaminőség-korlát hamarabb lép be.

## ADR-007 — SQLite + WAL + két FTS5 index

**Kontextus:** strukturált metaadat + teljes szöveges keresés + helyi, hálózat nélküli működés.
**Döntés:** egy SQLite adatbázis (WAL, `synchronous=NORMAL`), `unicode61 remove_diacritics 2` a szókereséshez,
`trigram` a részstringes kereséshez.
**Következmény:** (+) egy fájl, tranzakciók, érett eszközök; (+) magyar ékezetek kezelve; (−) két index
dupla hely és írási költség; (−) a nagyon nagy szövegkorpuszt korlátozni kell (256 KB/kártya).

## ADR-008 — Tartalom szerint címzett blob-tár kvótával

**Kontextus:** a pillanatképek nagyok és ismétlődhetnek; a döntés: akár több tíz GB lemez.
**Döntés:** blobok `sha256` névvel, `zstd` tömörítéssel; globális kvóta 50 GB, kártyánként 100 MB,
LRU + archívum-alapú szemétgyűjtés; atomikus írás.
**Következmény:** (+) deduplikáció, integritás-ellenőrzés, robusztus mentés; (−) a kvóta és a GC
kódot igényel (és felhasználói felületet).

## ADR-009 — Munkamenet-politika: `keep` / `sweep` / `wipe`

**Kontextus:** a felhasználói döntés eredetileg a „24 óra után teljes törlés = kijelentkezés" volt, de a
követelmény pontosítása szerint **a fontos bejelentkezéseket életben kell tartani** (Google, Facebook, saját
oldal), lehetőleg kis memóriaigénnyel, mérhető költséggel. A bejelentkezés (süti, token) a motor **lemezes**
partíciójában van, tehát életben tartása **0 RAM**-ba kerül, amíg a kártya hibernált.
**Döntés:** a 24 óra nem a bejelentkezést törli, hanem a melléktermékeket; háromszintű politika
(kártyánként/konténerenként):
- **`keep`** — semmit nem töröl; a bejelentkezés megmarad (fontos szolgáltatások);
- **`sweep`** *(konténer-alap)* — gyorsítótár, `sessionStorage`, Service Worker, code cache törlése;
- **`wipe`** — teljes origin-törlés = kijelentkezés (bank, egészségügy, munka).
A törlés origin-szintű, a snapshot soha nem törlődik, és végrehajtásához a motor kell (CDP
`Storage.clearDataForOrigin`), ezért `cleanup_pending` jelöléssel, a motor következő elérhető pillanatában fut.
**Következmény:** (+) a bejelentkezések megmaradnak, a RAM-költség mérhető és ~0; (+) a `wipe` továbbra is
elérhető a kényes fiókokhoz; (−) az „eltűnt a bejelentkezésem" élmény nem lesz alapértelmezett, de a
felhasználónak meg kell értenie a lakat-jelzést; (−) a Service Workerek hibernálása miatt a háttér-push
nem működik (dokumentált veszteség).

## ADR-010 — Széf: DEK/KEK + AES-256-GCM + opcionális Argon2id

**Kontextus:** a DPAPI önmagában a felhasználói profilhoz köt, nem hordozható, nincs kulcsrotáció.
**Döntés:** véletlen DEK titkosítja a rekordokat (AES-256-GCM, rekordonként nonce, AAD = origin+id); a DEK
két csomagolása: rendszer kulcstartó (DPAPI / Secret Service) és/vagy mesterjelszó (Argon2id).
**Következmény:** (+) hordozható, rotálható, erős; (−) a mesterjelszavas mód elvesztése = adatvesztés
(ezért explicit figyelmeztetés és mentési kód); (−) saját kriptográfiai kód (rendszerkönyvtárakra építve).

## ADR-011 — HTML kezdőlap szűk bridge API-val

**Kontextus:** a „bárhogy kinézhet" követelmény csak HTML/CSS/JS-sel teljesíthető; viszont a kezdőlap nem
kaphat natív hozzáférést.
**Döntés:** helyi HTML kezdőlap (`antikolt://home`), kártyák `<antikolt-card>` komponensként, szigorú CSP,
allowlist bridge (`cards.*`, `search`, `workspaces.*`, `events.on`), sanitizálás, verziózott szerkesztő.
**Következmény:** (+) végtelen testreszabhatóság; (+) a felhasználó saját oldalt írhat; (−) a kezdőlap egy
renderelőt foglal (ezért virtualizált, kis súlyú); (−) a felhasználó saját kódja hibás lehet → beépített
sablonra esés.

## ADR-012 — Téma-létra + luminancia-ellenőrzés + szürke mód

**Kontextus:** a sötét mód automatizálása megbízhatatlan; a felhasználói döntés: előre definiált
technikák, és a szürke mód legyen az alap.
**Döntés:** hatfokozatú technika-létra (T0–T6), minden lépés után a képernyőkép átlagfényerő alapján
ellenőrzés, az eredmény oldalanként (`site_recipe`) tárolva; négy felületi téma (light/grey/dark/amoled).
**Következmény:** (+) mérhető, önellenőrző, oldalanként tanul; (−) minden lépéshez kép és extra munka;
(−) a generált stíluslap motor karbantartást igényel (MIT licencű alapból adaptálva).

## ADR-013 — Előnézet-frissítés ugyanazzal a motorral, ütemezve

**Kontextus:** az „aktuális előnézet" élő betöltést kíván, ami ellentmond a „nincs háttérmotor" elvnek.
**Döntés:** az előnézet-frissítés az egyetlen motorházat használja, tétlen időben, egyszerre egy kártyát,
8 s időkorláttal, napi/heti korláttal, kikapcsolhatóan.
**Következmény:** (+) nincs második renderelő; (+) friss rács; (−) néha a motor „felébred" tétlenül
(ezért csak a kezdőlapon, tétlenül, jelzéssel).

## ADR-014 — Licenc MPL-2.0, Qt dinamikus linkeléssel

**Kontextus:** nyílt forrású böngésző; a CEF BSD, a Qt LGPL-3.
**Döntés:** MPL-2.0 (fájlszintű copyleft); a Qt-t dinamikusan linkeljük, a telepítőben cserélhetően.
**Kötelezettség:** a Qt módosított fájljait közzé kell tenni; a felhasználónak lehetőséget kell adni a Qt
lefordítására és cseréjére (Linuxon ez természetes, Windows-on dokumentálni kell).
**Következmény:** (+) megosztható, de nem fertőző; (−) a Qt-csere jogi/telepítési követelményeit be kell
tartani. **Részletek, kötelezettség-lista és az AI-szerzőség kezelése:** `13-LICENC.md`.

## ADR-015 — Nulla telemetria

**Kontextus:** adattudatos identitás, GDPR-egyszerűség, felhasználói bizalom.
**Döntés:** nincs automatikus hálózati jelentés; összeomlás esetén helyi napló, amit a felhasználó
kézzel küldhet el.
**Következmény:** (+) bizalom, egyszerű megfelelés; (−) nehezebb a hibák felderítése — ezért kell jó helyi
diagnosztika és könnyű „hibacsomag export".

## ADR-016 — Egy ablak, nincs bővítményrendszer (v1)

**Kontextus:** minden további renderelő/ablak/bővítmény ellene dolgozik az 1 GB-os célnak.
**Döntés:** v1-ben egy ablak és egy élő nézet; bővítmény-API nincs; helyette felhasználói szkriptek,
szabályok és oldal-receptek.
**Következmény:** (+) a memória- és biztonsági modell egyszerű; (−) a felhasználók egy része hiányolja
majd a bővítményeit — ez tudatos csere.

## ADR-017 — Kód és commit angolul, dokumentáció HU + EN

**Döntés:** a forráskód, azonosítók, commit-üzenetek, API-dokumentáció angolul; a terv és a felhasználói
dokumentáció magyarul (mérvadó) és angolul (fordítás).
**Következmény:** (+) nemzetközi hozzájárulás lehetséges; (−) a dokumentációt két nyelven kell karbantartani
(a fordítás mindig egy lépéssel lemarad — ez elfogadott).

## ADR-018 — Asztali böngésző-alapfunkciók a v1 kötelező részei

**Kontextus:** a Gemini-terv ezeket nem tartalmazta; ezek nélkül a böngésző nem használható napi eszközként.
**Döntés:** letöltéskezelő, keresés az oldalban, zoom, PDF, PiP, nyomtatás, popup-kezelés, összeomlás
utáni helyreállítás, billentyűzetes navigáció és akadálymentesség, import/export, keresőmotor-választás a
v1 része (F7).
**Következmény:** (+) tényleg használható termék; (−) az F7 hosszabb lesz, és a béta csak utána jön.

## ADR-019 — Munkamenet-ledger: a bejelentkezések mérhető életben tartása

**Kontextus:** a felhasználó pontosította a követelményt: nem „mindenhol automatikus bejelentkezés" kell,
hanem a **fontos bejelentkezések életben tartása kis memóriaigénnyel**, és **legyen mérhető, mit veszítünk**.
**Döntés:** minden `konténer × origin` párról nyilvántartás (`session` tábla): állapot, sütik száma/mérete,
tároló-méret, Service Werkerek száma, utolsó igazolt bejelentkezés, mért RAM (ha fut), politika, és a
„ha törlöm, ezt veszíted" lista. A felület **Munkamenetek** panelje ezt mutatja; a költség kimondva:
dormant bejelentkezés ≈ 0 RAM.
**Következmény:** (+) a „mit veszítünk" nem marketing, hanem adat; (+) a felhasználó látja, miért kerül sok
a RAM-ba (a futó oldalak, nem a bejelentkezések); (−) új tábla és panel, valamint a `state` heurisztika
(`unknown` állapot is létezik). Részletek: `10-MUNKAMENET.md`.

## ADR-020 — Erőforrás-profilok

**Kontextus:** a felhasználó kérése: „legyenek profilok, úgyis az éles teszt dönt", és a gép konfigurálható.
**Döntés:** öt profil (Gyors / Kiegyensúlyozott / Takarékos / Ultra / Kézi) csomagolja a motor-élettartamot,
hibernálási időt, előnézet-frissítést, médiaminőséget, GPU-választást és büdzsé-sávokat; automatikus váltás
(konnektor ↔ akkumulátor, memórianyomás) rögzíthető felülírással; gépenként megjegyzett konfiguráció.
**Következmény:** (+) ugyanaz a termék gyengébb gépen is használható; (+) a mérés dönt, nem a hit;
(−) több konfigurációs felület és több teszt-eset. Részletek: `09-CELHARDVER.md`.

## ADR-021 — A fejlesztést AI-ágens végzi, minőségbiztosítási szabályokkal

**Kontextus:** a projektgazda nem fejleszt; a kódot AI-ágens írja. Ez gyors, de fokozott
ellenőrzés-igényt jelent, és szerzőségi/jogi kérdéseket vet fel.
**Döntés:** a fejlesztés AI-ágenssel folyik, de:
1. minden fázis végén **mérés** (memória, idő) és **teszt** dönt, nem a „kész van" állítás;
2. a kritikus rétegek (széf, IPC, adatbázis-migráció, snapshot-integritás) **kézi felülvizsgálatot** kapnak;
3. a repó **nyilatkozik** az AI-segítségről (`NOTICE`, `AUTHORS`, SPDX), és nem állít kizárólagos emberi
   szerzőséget;
4. minden állítás bizonyítékkal (parancs kimenete, teszt, mérés) kerül a jegyzőkönyvbe.
**Következmény:** (+) gyors fejlődés, dokumentált döntések; (−) az emberi tesztelés (dogfooding) és a
kritikus kód átnézése nem spórolható meg; (−) a jogi/proveniencia kezelése kötelező. Részletek: `13-LICENC.md`.

## ADR-022 — Csatolási mód véglegesítése

**Kontextus:** a natív ablak-beágyazás Wayland alatt nem megbízható; az OSR hordozható, de drágább.
**Döntés:** Windows 11 = **natív ablak**; Linux/Wayland = **OSR**; a kettő ugyanazon `EngineSurface`
absztrakció mögött. Az OSR-ág **minimális prototípusa az F0 spike**, hogy a kockázat korán kiderüljön.
**Következmény:** (+) mindkét platformon működő út; (+) a motor folyamata bármikor leállítható (a kép marad);
(−) két út karbantartása; (−) OSR-nél akadálymentességi hidat kell építeni. Részletek: `11-OSR.md`.

## ADR-023 — Passkey-stratégia

**Kontextus:** a CEF-nek **nincs beépített WebAuthn-támogatása** (a beágyazói `WebAuthenticationDelegate`
hiányzik), a hardveres út CEF-patch vagy külön segédprocesszor, ami nem reális egyéni projektnél.
**Döntés:** v1.0 = nincs passkey (jelszó + TOTP), a széf adatmodellje készen áll; v1.1 = **szoftveres
passkey a széfben**, CDP virtuális hitelesítővel (kulcsexport → széf → visszatöltés); a felhasználói
jelenlét-ellenőrzést a **széf zárolása** helyettesíti; CEF-patch és külön segédprocesszor kizárva (kivéve,
ha a passkey-only fiókok miatt elkerülhetetlen).
**Következmény:** (+) platformfüggetlen, hordozható passkey; (−) nincs hardveres védelem; (−) a CDP belső
API verziófüggő, ezért spike előzi meg, és a kudarc esetére az „A" út (passkey nélkül) a tartalék.
**Megerősítve (2026-11):** a projektgazda elfogadta a szoftveres kompromisszumot; a passkey-only fiókok
ellenőrzése a `12-PASSKEY.md` 2.5. pontja szerint történik. Részletek: `12-PASSKEY.md`.

## ADR-024 — Kódaláírás

**Kontextus:** Windows-on a SmartScreen az aláírás nélküli telepítőt figyelmezteti; 2024 óta az EV
tanúsítvány **nem** kerüli meg a SmartScreen-t; egyéni, EU-s fejlesztőnek az Azure Artifact Signing nem
elérhető (csak szervezetnek EU-ban).
**Döntés:** v0.x = aláírás nélkül, de **Ed25519/minisign aláírt frissítés-manifesttel** (fail-closed);
v1.0 = **SignPath Foundation** ingyenes aláírás (OSI-licenc feltétel → MPL-2.0 megfelel) + továbbra is
Ed25519 a frissítésekhez; EV kihagyva; OV/Azure csak szervezetté válás esetén.
**Következmény:** (+) nulla költség; (+) a frissítések integritása kriptográfiailag ellenőrzött;
(−) az első időszakban SmartScreen-figyelmeztetés, és a kiadó neve „SignPath Foundation" lesz — ezt
kommunikálni kell. Részletek: `14-KODALAS.md`.
**Pontosítás:** **fizetős tanúsítvány nincs a tervben** (a projektgazda döntése). A frissítések teljesen
automatizáltak: Ed25519-aláírt manifest (fail-closed, két kulcs, rollback) + Sigstore provenance; az
Authenticode a SignPath ingyenes aláírásával, kiadásonként legfeljebb egy kattintással.

## ADR-025 — Lemezkvóta

**Kontextus:** a lemezhibernálás és az időutazás miatt a pillanatképek több tíz GB-ot is elérhetnek; a
projektgazda 50 GB-ot kért alapértékként, felülbírálhatóan.
**Döntés:** globális alapkvóta **50 GB**, kártyánként 100 MB, LRU + archívum-alapú gyűjtés; gépenként
felülbírálható (`config/devices/<hostname>.toml`), például a VECTOR-on 100 GB.
**Következmény:** (+) kiszámítható helyfoglalás; (+) a célgépen bővíthető; (−) a kvóta-GC kódot és
felületet igényel (a felhasználó látja, mi foglal helyet).

## ADR-026 — Közreműködés és biztonsági kormányzás

**Kontextus:** a projektgazda elvárása: „bárki szabadon közreműködhessen, mert egy böngészőnél lehetnek
biztonsági kockázatok". A licenc ehhez kevés: a biztonság folyamat kérdése.
**Döntés:** **MPL-2.0 + DCO** (nincs CLA, nincs engedélykérés); **védett kódrészek** (széf, IPC,
snapshot-parser, migrációk, platform/sandbox) maintainer-jóváhagyással; kötelező automatizált kapuk
(teszt, **fuzz** a nem megbízható bemenetekre, memória-regresszió, statikus elemzés, titok-szkennelés,
SPDX-ellenőrzés); `SECURITY.md` privát bejelentési csatornával és 30 napos javítási céllal; aláírt
kiadások + SLSA/Sigstore provenance.
**Következmény:** (+) szabad közreműködés és komoly biztonsági folyamat egyszerre; (+) az AI-segítség
átlátható (NOTICE/AUTHORS/PR-sablon); (−) a védett területek lassítják a fejlődést — ez tudatos.
Részletek: `15-HOZZAJARULAS.md`.
