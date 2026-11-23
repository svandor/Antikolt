# 05 — Döntési napló (ADR)

Minden visszafordíthatatlan vagy drága döntés itt van rögzítve: **kontextus → döntés → következmény**.
A döntés megváltoztatása új ADR-t igényel (a régi „Elavult" állapotot kap).

## ADR-086 — A passkey F0 spike: a CDP virtuális hitelesítő működik a CEF 158-ban (2026-11-23)

**Kontextus:** a 12-PASSKEY.md szerint a szoftveres passkey „B" útja a CDP
`WebAuthn.addVirtualAuthenticator` virtuális hitelesítőre épül, de az F0 spike-nak kellett eldöntenie,
működik-e ez a CEF 158-ban — mert a CDP WebAuthn-domain a beágyazói WebAuthn-megvalósításhoz kötődik,
amit a CEF elvileg nem ad.

**Döntés (a spike eredménye):**

1. **A WebAuthn-domain elérhető, de előbb `WebAuthn.enable` kell:** az első próba
   `addVirtualAuthenticator` közvetlen hívása `-32000 „The Virtual Authenticator Environment has not been
   enabled"` hibát adott; a `WebAuthn.enable` → `addVirtualAuthenticator` sorrend viszont **működik**.
2. **A virtuális hitelesítő létrejön:** a `--webauthntest` `webauthn_authenticator=ok id=<authenticatorId>`
   kimenettel zár (exit 0) — tehát a B út a CEF 158-ban járható, CEF-patch nélkül.

**Következmény:** (+) a passkey B útja de-kockáztatva: a CDP virtuális hitelesítő működik, nincs szükség
CEF-patchre vagy segédfolyamatra; (+) a `--webauthntest` a kapuban méri; (−) a teljes
`navigator.credentials.create/get` folyamat (secure-context tesztoldal, kulcs-export/import a széfbe) még
külön szeletek.

## ADR-085 — A jelszó-CSV-import név szerinti oszlop-leképezéssel, a magban (2026-11-23)

**Kontextus:** a Chrome és a Firefox a jelszavakat CSV-be exportálja, de eltérő oszlopkészlettel:
Chrome `name,url,username,password`, Firefox `url,username,password,…`. Az importnak a széfbe kell
írnia (a titok a magban van, ADR-083), és a `vault_import_csv` IPC-n keresztül jön.

**Döntés:**

1. **Oszlop-leképezés név szerint, nem pozíció szerint:** a `url`/`username`/`password`/`name`
   fejléceket keressük (a UTF-8 BOM-ot is kezelve), így mindkét exportáló és bármilyen sorrend
   működik.
2. **Az import a magban (`import.rs`) + `vault_import_csv`:** a parse tiszta függvény, a sorok
   `kind = password` hitelesítővé válnak; a széfet a híváshoz nyitni kell (a többi titkos művelethez
   hasonlóan).
3. **A hiányos sort kihagyjuk, nem importáljuk:** URL vagy jelszó nélkül a sor nem lehet hitelesítő,
   mert a későbbi autofillt félrevezetné.

**Következmény:** (+) a Chrome/Firefox export importálható és mérve (`cargo test` **187** zöld);
(−) a Firefox „csak URL, jelszó nélkül" sorai kimaradnak (ez a valóság: az ilyen sor nem jelszó);
(−) az import egyelőre csak a `vault_import_csv` IPC-n keresztül érhető el, a shell menüje a
shell-felület szeletével jön.

## ADR-084 — A TOTP a magban van, RFC 6238, és a kódot a szolgáltatás számolja (2026-11-23)

**Kontextus:** a széf tárolója (ADR-083) már tartja a TOTP-titkot (`totp_secret_ct`), de a kód
előállítása hiányzott. A TOTP a „jelszavak + TOTP + passkey" döntés középső lába, és a kódnak a mag
oldalán kell születnie: a titok soha nem hagyhatja el a core-t.

**Döntés:**

1. **RFC 6238 a magban** (`totp.rs`): `totp_code(secret, digits, period, algorithm, now_ms)` — a
   HMAC-SHA-1/256/512 + dinamikus csonkolás (RFC 4226) + base32 titok; a `now_ms` paraméter, így az
   RFC vektorai óra nélkül tesztelhetők.
2. **A kódot a szolgáltatás számolja** (`vault_totp` IPC): a shell az `id_of_credential`-t adja, a
   core megnyitja a titkot és visszaadja a `{code, seconds_remaining}`-et — a titok a core-ban marad.
3. **A paraméterek a `totp_params_json`-ben** (digits 6|8, period, algorithm), az RFC-alapértékekkel
   (6 jegy, 30 s, SHA-1).

**Következmény:** (+) a TOTP-kód mérve az RFC 6238 vektoraival (a „klasszikus csapda" elkerülve: a
vektorok algoritmusonként más titkot használnak, 20/32/64 bájt); (+) a titok a core-ban marad;
(−) a passkey (WebAuthn) és a CSV-import még külön szeletek. `cargo test` most **180** zöld.

## ADR-083 — A széf tárolója SQLite-only: a titkoknak nincs JSON-exportja (2026-11-23)

**Kontextus:** az ADR-082 bekötötte a titkosítást (AES-256-GCM + Argon2id + DPAPI), de a széf még nem
tárolt semmit. Az ADR-069/070 szabálya szerint minden store egy íróval (SQLite) működik, a `*.json`
dokumentumok pedig származtatott exportok — egy titok (jelszó, TOTP-kulcs, passkey) azonban soha nem
kerülhet egy exportba, mert az hordozható, másolható és nem titkosított.

**Döntés:**

1. **A széf SQLite-only:** a `credential` tábla (séma 0011) tartja a titkos mezőket `*_ct` oszlopokban
   (AES-256-GCM, AAD = origin + id), és **nincs** `vault.json` export — a `db_status`-szinkron sem ír ki
   titkot semmilyen származtatott dokumentumba.
2. **A kulcs DEK/KEK** (ADR-010): a `vault_meta` egysoros tábla a módot (`master` | `dpapi`), a sót és a
   becsomagolt adatkulcsot tartja; az első feloldás rögzíti a módot, a továbbiak csak olvassák.
3. **A lista meta, a get titok:** `vault_list` csak metaadatot ad (id, origin, kind, label, last_used_at),
   akár zárt állapotban is; `vault_get` a nyílt szöveget csak nyitott széffel adja vissza; a rossz jelszó,
   a hamisított rekord és a másik originre átjátszott blob egyaránt `vault_locked`.
4. **Az 5 perces idle auto-lock a szolgáltatás-hurokban:** minden kérés `tick`-eli a széfet, a titkos
   műveletek (`get`/`create`/`delete`) pedig `touch`-olják — így a használaton kívüli széf magától zár
   (ADR-010), nem csak kézi `vault_lock`-ra.

**Következmény:** (+) a jelszavak, TOTP-kulcsok és passkey-k most már tárolhatók és visszaolvashatók,
mérve (`cargo test` **174** zöld: a 7 credentials-egységteszt + az end-to-end
`the_vault_seals_serves_and_persists_credentials`); (+) a titkok soha nem érnek JSON-exportot; (−) a
TOTP-kódgenerálás, a passkey (WebAuthn) és a Chrome/Firefox CSV-import még külön szeletek; (−) a
profil-köteg (`profile_export`) sem viheti a széfet, amíg a titkosítás nincs hozzá rögzítve.

## ADR-082 — A széf titkosítása: AES-256-GCM + Argon2id + Windows DPAPI (2026-11-23)

**Kontextus:** az ADR-031 a széf alakját elkészítette, de a titkosítást `UnavailableSecrets` mögé
rejtette, mert a buildkörnyezetben nem volt crates.io-elérés, az offline registry pedig egyetlen
kriptográfiai crate-et sem tartalmazott. A jelszó-importnak (M7) viszont cél-boltozat kell, ami
titkosítás nélkül nem építhető. A karbantartó döntése: **jelszavak + TOTP + passkey**, titkosítás
**Windows DPAPI + opcionális mesterjelszó** (az ADR-010 DEK/KEK terve), és **Chrome/Firefox CSV-import**.

**Döntés:**

1. **A crate-ek bekötve** (`aes-gcm` 0.10, `argon2` 0.5, `zeroize` 1): a buildkörnyezet most eléri a
   crates.io-t, így a korábbi blokkoló megszűnt.
2. **A rekord-titkosítás AES-256-GCM** (`AesGcmSecrets`, `secrets.rs`): rekordonként friss 96 bites
   nonce (a ciphertext elé írva), a rekord AAD-ja az **origin + rekord-azonosító** (ADR-010) — a GCM
   címke a kulcsot, a testet és az AAD-ot együtt ellenőrzi, így a rossz kulcs, a hamisított rekord és
   a másik originre átjátszott blob egyetlen hiba (`Corrupt`), soha nem plaintext-találat.
3. **A mesterjelszó-út Argon2id** (`from_master_password` + `generate_salt`): a 256 bites kulcs a
   jelszóból és a tárolt sóból jön; ugyanaz a jelszó+só ugyanazt a kulcsot adja, a rossz jelszó nem nyit.
4. **A DPAPI-út Windows alatt** (`secrets::dpapi`): `CryptProtectData`/`CryptUnprotectData` a
   `crypt32`-ből, `CRYPTPROTECT_UI_FORBIDDEN`-nel; a modul a crate egyetlen `unsafe`-t használó helye
   (`#[allow(unsafe_code)]` modulban, SAFETY-kommentekkel), mert a crate `#![deny(unsafe_code)]`.
5. **A kulcsanyag `zeroize::Zeroizing`-ben él**, a `Debug` impl nem ír ki titkot.

**Következmény:** (+) a széf most már tud pecsételni és nyitni — a `can_store: false` megszűnt a
mesterjelszó- és a DPAPI-út számára; (+) a 10 új teszt méri (round-trip, AAD-kötés, hamisítás-elutasítás,
rossz jelszó, só-egyediség, DPAPI round-trip Windows alatt); (−) a DEK/KEK-tárolás, a `credential` tábla
írása/olvasása, a TOTP, a passkey és a CSV-import még külön szeletek; (−) a DPAPI-blob gépre+felhasználóra
kötött, a profil hordozhatósága csak mesterjelszóval adott (ADR-010). A `cargo test` most **166** zöld.

## ADR-081 — A könyvjelző-import a Netscape-HTML formátumot eszi (M7) (2026-11-23)

**Kontextus:** az M7 „jelszó- és könyvjelző-import" nyitott volt. A jelszó-importnak nincs cél-boltozata a
termékben (külön, nagyobb szelet), a könyvjelző-import viszont a kártya-modellre természetesen illik.

**Döntés:**

1. **A bemenet a Netscape-HTML** (`<!DOCTYPE NETSCAPE-Bookmark-file-1>`): ez az egyetlen formátum, amit a
   Chrome, a Firefox és az Edge is exportál, így egy parser mindhármat lefedi.
2. **Toleráns regex-parser** (`bookmarks.h` `parseBookmarksHtml`): a formátum nem érvényes XML (bare `<p>`,
   `<H3>` mappák), ezért a `<A HREF>`-horgonyokat regexszel szedjük ki, nem stream-parserrel; a `javascript:`
   hivatkozásokat és a mappákat kihagyjuk, a sorrend megmarad.
3. **Minden könyvjelző egy kártya:** cím + URL, az alapértelmezett munkaterületre, a már meglévő URL-ek
   kihagyásával (az újra-import nem duplikál). Az azonosító a rács saját `newCardId`-jának C++ párja
   (`bookmarks.h` `newCardId`, 26 karakter, Crockford base32).

**Következmény:** (+) a könyvjelző-import kész, a `--bookmarkimporttest` méri (a parser 3 esetet, az id-alakot
és a kártya-upsertet); (−) a jelszó-import nincs (nincs cél-boltozat — külön szelet); (−) a fájlválasztó dialógust
a harness nem kattinthatja — a menüpont a termék belépési pontja, a `--bookmarkimporttest` az alatta lévő utat méri.

## ADR-080 — Az „Automatikus" profil a magban oldódik fel (power + pressure → built-in, M6) (2026-11-23)

**Kontextus:** az M6 „Öt erőforrás-profil, automatikus váltással" nyitott fele volt az automatikus váltás. Az
ADR-020 a `ProfileKind::auto(power, pressure)` leképezést már definiálta és tesztelte, de nem volt bekötve.

**Döntés:**

1. **Az `Auto` a `ProfileKind` hatodik értéke** (serde `auto`), `params()`-a `None` — nincs rögzített
   paramétere, mint a `Manual`-nak. A mag a `resource_profile="auto"`-t tárolja és adja vissza.
2. **A feloldás a magban történik** (`resolve_profile` IPC: `{on_ac_power, pressure}` → `{kind, params}`), a
   `ProfileKind::auto` + `params()` alapján, hogy az ADR-020 táblája egy helyen maradjon (a Rust magban).
3. **A shell a feltételeket méri és újra-kérdez:** a tápforrást a rendszerből olvassa (`GetSystemPowerStatus`,
   `ANTIKOLT_AC_POWER` felülírással az önteszthez), a nyomást a meglévő memória-mintavételező adja; minden
   minta után újra `resolve_profile`-ot hív, ha a profil `auto`. Az első feloldás `normal` nyomással indul
   (a tápforrás a Balanced/Saver választója), a `checkMemoryPressure` utána pontosítja.

**Következmény:** (+) a profil követi a gépet — a kemény nyomás `Ultra`-ra esik akár hálózaton is, a
visszatérés `Balanced`-re lép fel; a `--autoprofiltest` méri a feloldást (6 eset) és a tápforrás-felülírást;
(−) a sávok a következő minta-ciklusban lépnek életbe (a már meghozott osztályozás még a régi sávokkal
történt); (−) a `Fast` a `auto`-ból soha nem jön — az mindig kézi választás.

## ADR-079 — A rács nyelve is a shellt követi (HU/EN), a `data-language` attribútummal (2026-11-23)

**Kontextus:** az ADR-078 a shell **chrome**-át tette válthatóvá; a rács (`home/app.js`) még magyar volt. A
rács külön folyamatban fut (a motorban), ezért a nyelvet át kell adni neki — ugyanúgy, ahogy a téma (ADR-037)
már átmegy.

**Döntés:**

1. **A nyelv a `data-language` attribútumban él** a rács `<html>`-jén, a téma `data-theme`-jének mintájára.
   A shell a `pushLanguageToGrid`-del küldi (load-kor és `setLanguage`-kor), és a `window.antikoltSetLanguage`
   horgon át kéri a helybeni újrarenderelést.
2. **A rács a saját karakterláncait egy `t()`-tel fordítja** (`home/app.js`, HU→EN `EN` térkép), a
   `data-language` alapján; ismeretlen kulcs/nem-`en` a magyar forrást adja vissza. A `system.language` híd
   a tartalék (ha a push még nem érkezett meg).
3. **A nyelvváltás újratöltés nélküli a rácson is:** a `setLanguage` újraírja a statikus címkéket (kereső,
   új kártya gomb) és újrarendereli a lapot (számláló, üres állapot, konténer-legenda, kártyák).

**Következmény:** (+) a rács HU/EN vált, a `--gridlangtest` méri (a kereső placeholder „Keresés" → „Search",
a `window.antikoltLanguage` tanúja); (−) a kártya **inline** szerkesztőcímkéi („Mentés", „＋címke" az
`antikolt-card.js`-ben) és a rács statikus aria-címkéi egyelőre magyarok (következő szelet).

## ADR-078 — A shell nyelve futásidőben váltható (HU/EN), először a chrome (2026-11-23)

**Kontextus:** a felület teljes egészében magyar (a magyar a hiteles nyelv), az angol fordítás a HU/EN nyitott
tétele volt (M7). A teljes i18n (shell + rács) nagy; ez a szelet az **alapot** rakja le: a nyelvváltó
beállítást, a fordítási mechanizmust és a shell **chrome**-át (eszköztár, tálca, beállítás-dokk, keresősáv).

**Döntés:**

1. **A nyelv a QSettings-ben él** (`ui/language`: `hu`/`en`, default `hu`), mint a téma és a sötét oldalak — a
   felületi beállítás, nem a hordozható profil adata.
2. **A fordítás futásidőben, táblázatból történik** (`i18n.h` `uiText(hu, lang)`): a forrás a magyar, az
   `en` egy `QHash`-ból adja az angolt, ismeretlen kulcs/nem-`en` nyelv a forrást adja vissza (a hiányzó
   fordítás soha nem üres címke). Nincs `.ts`/`.qm` build-lépés, mert a chrome-karakterláncok száma kicsi, és
   a teszt így motor nélkül fut.
3. **A váltás újraindítás nélküli:** a `MainWindow::setLanguage` beállítja, elmenti és helyben átcímkézi a
   chrome-ot; a beállítás-dokk `Nyelv:` lenője hívja.

**Következmény:** (+) a chrome HU/EN váltható, a `--languagetest` méri (a tiszta `uiText`-táblát, a
QSettings-perzisztenciát és a helyben átcímkézést); (−) a **státuszüzenetek és a rács** egyelőre magyarok
(következő szeletek); (−) a `tr()`-alapú belső hibaüzeneteket (`core_client`/`engine_client`) ez a szelet nem
érinti.

## ADR-077 — A preview-név a tartalom, nem a kártya-azonosító (2026-11-23)

**Kontextus:** a preview-fájl neve eddig a kártya azonosítója volt (`<id>.png`), és a `sha256`-alapú
tartalom-szerinti blob-tár nyitott tételként állt, a Rust `sha2` crate hiányára hivatkozva („nincs hálózat a
`cargo vendor`-hoz"). A valóságban a név a **shellben** keletkezik (a capture ott történik, a bájtok ott
vannak), így a mag Rust oldala nem is szorul `sha2`-re: a Qt `QCryptographicHash::Sha256` ugyanazt adja,
hálózat nélkül.

**Döntés:**

1. **A preview neve a tartalom hash-e** (`<sha256-hex>.<ext>`, `previewContentName` a `previews.h`-ban,
   `QCryptographicHash::Sha256`). A kártya rekordjának `preview_ref`-je így a **kép** azonosságát hordozza,
   nem a kártyáét.
2. **A dedup a nevezésből adódik:** két kártya, amelynek az oldala bájtra azonosan fényképeződik, egyetlen
   fájlt oszt meg — a második capture már nem másol új fájlt, csak a rekord tanulja meg ugyanazt a nevet.
   A tartalom-változás új hash-t ad, a régi fájl az árva-takarítás (`removeOrphanPreviews`) áldozata lesz.
3. **A frissesség-ellenőrzés a meglévő névvel fut:** a `previewNeedsRefresh` a tárolt `preview_ref`-re néz
   (nem a kártya-azonosítóra), így a 7 napos szabály a tartalom-címzés után is érvényes.

**Következmény:** (+) a tartalom-szerinti blob-tár megvan, a `sha2` crate-függőség megszűnt (a hash a
shellben, Qt-vel); (+) a dedup egy fájlra fogja az azonos képeket; (+) a `--previewhashtest` (publikus
tesztvektorokra tűzve), a `--previewgentest` és a `--previewworkertest` (`preview_content_addressed=ok` /
`worker_content_addressed=ok`) méri; (−) a kártya-azonosító alapú nevek megszűntek — a régi profilban
keletkezett `<id>.png` fájlok az árva-takarításban tűnnek el.

## ADR-076 — A `video_max_height` megjelenítési sapka, nem dekódolási sapka (2026-11-23)

**Kontextus:** az ADR-020 erőforrás-profiljai `video_max_height` mezőt hordoznak (Fast 2160, Balanced 1080,
Saver 720, Ultra 480), és a profilszöveg azt ígérte, hogy „a governor csökkenti a felbontást, hogy memóriát
spóroljon". A mezőt a shell eddig csak **jelentette** (`--memoryreport`), nem alkalmazta — ez volt az F6 utolsó
nyitott vezérlő-művelete. A kutatás megállapította: a CEF/Chromium **nem ad** laponkénti videó-dekódolási
felbontáskorlátot (nincs ilyen CDP-op), és a CSS-injekció is csak a top-level `video` elemeket éri el (a
cross-origin iframe saját dokumentum — biztonsági határ).

**Döntés:**

1. **A sapka megjelenítési sapka:** a motor egy nevesített stíluselemmel (`antikolt-video-cap`) a
   `video{max-height:Npx}` szabályt injektálja (`set_video_cap` op), a profil `video_max_height`-je hajtja. A
   szabályt a profilváltás cseréli, a `manual` profil (nincs paraméter) eltávolítja — ugyanaz a trükk, mint a
   sötét oldalak kapcsolója (ADR-044/045).
2. **A dekódolási felbontás `not-measurable`:** a megjelenítési sapka a *renderelt dobozt* korlátozza, nem a
   *dekódolt folyamot* — a CEF nem ad laponkénti videó-dekódolási korlátot, így a memória-megtakarítás közvetett
   (kisebb komponált doboz), nem a dekódolási puffer csökkenése. Ezt a teszt **kimondja**
   (`video_cap_resolution=not-measurable`), nem hallgatja el.
3. **A hatókör top-level `video`:** a cross-origin iframe-ek kívül esnek (saját dokumentum); a sapka ezt nem
   kerüli meg.

**Következmény:** (+) a profil `video_max_height`-je most már **alkalmazva** van (nem csak jelentve), és a
`--videocaptest` méri (Fast 2160px → Saver 720px a renderelt magassággal → Manual eltávolítva); (−) a valódi
dekódolási felbontás-csökkentés CEF-ben nem megvalósítható, ezért a memória-ígéret ezen a téren közvetett és
dokumentáltan korlátos.

## ADR-075 — A `<antikolt-card>` önálló csomag (2026-11-22)

**Kontextus:** a kártya web-komponense eddig a kezdőlap saját szkriptjében (`app.js`) élt, a rács belső globálisaival
(`pageContainers`, `pageWorkspaces`, `pendingFocusId`) összenőve — épp az, amit a `04-UTEMTERV` nyitott tétele
(„a mai kártya a lap saját komponense") felszámolni akart. Egy felhasználó által szerzőtt kezdőlap így nem tudta
betölteni a kártyát.

**Döntés:**

1. **A komponens egy saját fájlba kerül** (`home/antikolt-card.js`), IIFE-be csomagolva: `hashHue`, `STATUS`,
   `cardStyles`, az `AntikoltCard` osztály és a `customElements.define` (őrzött). A rács (`app.js`) és egy
   felhasználói lap **ugyanezt** a fájlt tölti be — egy forrás, nem másolat.
2. **A menedzsment-vezérlők adatai a hosttól jönnek**: a konténer/munkaterület `<select>` a
   `setOptions({ containers, workspaces })`-ből olvas; a rács ezt a betöltött táblákkal hívja, egy felhasználói lap
   üresen hagyja (a kártya akkor is kirajzolódik, a `<select>`-ek nélkül). A fókusz-visszaállítás `pendingFocusId`
   állapota a csomag belső `let`-je, `getPendingFocusId()`-dal kiolvasva.
3. **A kártya továbbra is kívülről kapja az adatot** (`element.card = …`), nem a magtól kéri — a komponens
   agnosztikus arra, honnan jön a rekord, és a shell push-ja továbbra is a rács dolga.

**Következmény:** (+) egy forrás a kártyára, a felhasználó-szerző kezdőlap betöltheti; (+) a csomag önálló, a
host lapja nem törheti el a kártyát; (−) a **teljes** szerkeszthető kezdőlap (a felhasználói HTML tárolása,
kiszolgálása és egy szerkesztő-felület) külön, nagyobb tétel marad — ez a szelet a csomagot adja, nem a
kezdőlap-szerkesztőt.

## ADR-074 — A memória-büdzsé mint profil-beállítás (F6) (2026-11-22)

**Kontextus:** a `system.memory` híd `budgetBytes`-e eddig **mindig 0** volt („nem kitalálunk büdzsét"), a rács
pedig ezért csak a felhasznált értéket írta ki. Az F6 büdzsé-vezérlője ehhez egy valódi, a felhasználó által
állítható számot kér, amit a felület sávként mutat.

**Döntés:**

1. **A büdzsé a `user_prefs` sora** (`memory_budget_mb`, séma **`0007`**): default `512` (MiB), validáció
   `64..=4096`. Az `ALTER TABLE … ADD COLUMN` csak akkor fut, ha az oszlop hiányzik (a 0005 mintájára), különben
   egy félbehagyott migráció „duplicate column"-nal bukna. A mező `serde(default)`-ot kap, hogy a F6 előtti
   profil-köteg importja ne bukjon el a hiányzó mezőn.
2. **A shell cache-eli, nem kérdezi per-kérés**: `CoreClient::userPrefs` + `MainWindow::refreshUserPrefs()` a mag
   `ready`-je és a profil-import után; a `system.memory` válasz `budgetBytes = memory_budget_mb × MiB`. A mérés
   (working set) továbbra is a motoré, a büdzsé a profilé — a kettő nem keveredik.
3. **A rács sávot rajzol**: a `#mem` mező `used / budget MB` szövegét egy used/budget kitöltés egészíti ki
   (`--mem-fill` inline változó), így a túllépés látható, nem csak szám.

**Következmény:** (+) a büdzsé valódi, perzisztens és a profil-költöztetés viszi; (+) a `--metricstest` most már
`page_budget_bytes > 0`-t is állít (a régi kemény 0 helyett); (−) a beállítás-felület (a `user_prefs_set` natív
vezérlője) és az F6 többi része — kártyánkénti bontás, erőforrás-profilok, raszter-kezdőlap — továbbra is nyitott.

## ADR-071 — A konténer izolációs határ: saját profilkönyvtár, proxy és engedély-politika (2026-11-21)

**Kontextus:** a konténer eddig szűrő és címke volt. A motor minden felületet `nullptr`
`CefRequestContext`-tel hozott létre, tehát **minden kártya egyetlen cookie-jar-t és cache-t használt** — az ADR-009
„kártyánként egy partíció" ígéretét a motor soha nem teljesítette, a `partition` kulcs dísz volt. Proxy és
engedély-politika pedig nem is létezett.

**Döntés:**

1. **Partíciónként saját `CefRequestContext`**, a profil gyökere alatti **egyetlen** könyvtárral
   (`<user data>\persist_card_<hash>`). A könyvtárat a CEF hozza létre; a ProfileManager csak közvetlenül a gyökér alatti
   profilt fogadja el (a `partitions\` alkönyvtár „Cannot create profile at path" hibát adott, és a partíció csendben
   memóriába esett — mérve).
2. **A konténer a házirend kulcsa**: a felület a `container` mezővel viszi a konténer id-jét, a hozzá tartozó
   `proxy` / `proxy_bypass` / `permissions` mezőkkel. Két kártya egy konténerből közös proxyt és engedélyeket kap, de
   soha nem közös cookie-jar-t. A mezők adott esetben hiányozhatnak (additív, `serde(default)`), ezért nem kell
   protokoll-verziót emelni (proto/README.md kompatibilitási szabálya).
3. **A proxy a konténer beállítása a magban** (`proxy_mode`: `system`/`direct`/`fixed`, `proxy_host`, `proxy_port`,
   `proxy_bypass`), séma `0005`. A motor a CEF `proxy` preferenciájába fordítja, a
   `CefRequestContextHandler::OnRequestContextInitialized` hookban (az első felület létrehozásakor fut, az első kérés
   előtt); a visszajelzést naplózza, nem nyeli el.
4. **Az engedélyek konténerenként, alapból megtagadva**: `permissions` = név → `allow`/`deny` (geolocation,
   notifications, camera, microphone, clipboard, …). Amit a konténer nem nevez meg, azt a motor **megtagadja**
   (`CefPermissionHandler`), mert nincs prompt-felület, és a néma engedélyezés a legrosszabb kimenet. A `media` külön
   kulcs, a `CefMediaAccessCallback`-on keresztül.
5. **A felület még nem szerkeszti a házirendet**: a mezők a magban, a JSON-exportban és a hídon át élnek, a konténer
   átnevezése a teljes rekordot küldi vissza (különben a házirend elveszne) — a proxy/engedély szerkesztő UI a
   következő szelet.

**Következmény:** (+) a kártyák tényleg izoláltak, és egy konténer proxyja mögé állítható anélkül, hogy az egész
böngészőt átállítanánk; (+) a házirend adatbázis-sor, tehát átnevezéskor, újraindításkor és profil-költöztetéskor is
megmarad; (−) a házirend-szerkesztő hiányzik, tehát a felhasználó ma csak a hídon/profil-importon át állítja be;
(−) a partíció-profilkönyvtárak a profilban maradnak, és a kártya törlése nem takarítja őket (a törlés a következő,
takarító szelet dolga).

## ADR-072 — Felhasználói beállítások (`UserPrefs`) SQLite-ban (séma 0006) (2026-11-22)

**Kontextus:** az ADR-070 mind a négy store-t SQLite-first tette (séma 0004): kártyák, konténerek, munkaterületek, munkamenetek. Hiányzik a felhasználói szintű beállítások tárolása: a `max_live_cards` (egyidejűleg élő kártyák száma), az `idle_hibernate_ms` (inaktív kártya hibernálásának ideje) és a `cold_card_ms` (mikortól hideg egy kártya). Eddig nem volt perzisztens állapotuk, és nem volt IPC-olvasás/írás rájuk.

**Döntés:**

1. **Új tábla: `user_prefs`**, egyetlen sorral (`id = 1`, `CHECK (id = 1)`), séma **`0006`** (`PRAGMA user_version = 6`). A migráció egy `CREATE TABLE IF NOT EXISTS` a meglévő v5 fájl fölé; a `DEFAULT` értékek akkor érvényesülnek, ha sosem lett beállítva, a seed sor pedig `ON CONFLICT (id) DO NOTHING`-gal íródik.
2. **`UserPrefs` struct** (`sqlite_store.rs`): `max_live_cards: u32 = 5`, `idle_hibernate_ms: u64 = 600_000` (10 perc), `cold_card_ms: u64 = 86_400_000` (24 óra), `Default` + `defaults()` + `validate()`. A két időzítő szándékosan rendezett: a hideg a későbbi állapot, soha nem a korábbi.
3. **Validáció:** `max_live_cards` ∈ [1, 12]; `idle_hibernate_ms` ∈ [60000, 3600000]; `cold_card_ms` ∈ [3600000, 2592000000]; és a `cold_card_ms <= idle_hibernate_ms` elutasítva (a hideg ablaknak hosszabbnak kell lennie az idle-nél, különben a kártya hamarabb hidegülne, mint hibernálódna). A hibák `SessionError::Rules(&'static str)`-ként jelentkeznek; `is_invalid_argument()` igaz `Rejected(_)` | `Rules(_)`-ra.
4. **IPC opok:** `user_prefs` (olvasás) és `user_prefs_set` (írás, validálás után). `profile_export` olvassa `sessions.user_prefs()`-t és a `Bundle`-be teszi; `profile_import` ha `read.prefs` `Some`, akkor `sessions.set_user_prefs(prefs)`-t hív.
5. **`Bundle`** kiegészül a `prefs: Option<UserPrefs>` mezővel (`#[serde(default)]`), és `Bundle::new(...)` átveszi a `prefs: UserPrefs` paramétert.

**Következmény:** (+) a felhasználói beállítások perzisztensek, a profil-költöztetés viszi őket; (+) a validáció tiltja az értelmetlen kombinációkat (pl. `cold_card_ms <= idle_hibernate_ms`); (−) a séma 4→6, a régebbi build v6 fájlt nem tud visszafelé olvasni.

## ADR-073 — Bejelentkezési popup: gesztus → valódi ablak ugyanabban a partícióban, 60 s (2026-11-22)

**Kontextus:** a „mindenhol bejelentkeztetés" popupon megy (OAuth/SSO, `window.open`). A motor
`CefLifeSpanHandler::OnBeforePopup`-ját eddig nem írtuk felül, így a CEF alapértelmezése szerint *minden* popup
megnyílt, de ugyanarra az `EngineClient`-re esett: a popup böngészője felülírta a kártya `state_->browser`-ét, a
bezárása pedig a kártya felületét vitte volna le. A shell nem tudott a popupról, tehát hibernálhatott a
bejelentkezés közepén.

**Döntés:**

1. **A popup csak felhasználói gesztusra nyílik meg** (`user_gesture == true` → engedélyez, különben blokkolt). A
   gesztus nélküli popup olyan, mint egy nem kért engedély: néma elutasítás. A `popup_requested` esemény viszi a
   döntést (`disposition`: `authenticated_window` | `blocked`) a shell felé.
2. **A popup az opener partícióját örökli** (CEF alapértelmezése: ugyanaz a `client`), tehát a sütik közösek — ez az
   SSO-lánc lényege. A popup **nem kártya**: nincs snapshot, nincs előnézet.
3. **A popup böngészőjét a motor külön követi** (`HostState::popup_browser`): a bezárása (`OnBeforeClose`) csak a
   popupot szünteti meg (`popup_closed` esemény), a kártya felülete él.
4. **A 60 s-os élettartam a motorban él** (steady-clock, `kPopupTimeoutMs = 60000`): lejáratkor a motor
   `CloseBrowser`-rel zárja a popupot — a shell nem ismeri a popup felület-azonosítóját, ezért a motor a felelős. A
   shell a `popup_open` jelzővel csak a hibernálást függeszti fel a `popup_closed`-ig.

**Következmény:** (+) az OAuth/SSO folyamat a kártya sütijeivel, a saját partíciójában fut, és nem töri meg a
kártyát; (+) a kéretlen popupok blokkolva; (−) a gesztus-út a harnessben nem mérhető (a szintetikus `window.open`
nem gesztus), ezért a `popup_requested`/`popup_closed` útja `not-measurable` — a döntés a kódban és a `02` §8.5-ben
rögzített.

## ADR-070 — Mind a négy store SQLite-first (séma 0004) (2026-11-21)

**Kontextus:** az ADR-069 a kártyákat tette adatbázissá, a konténerek, munkaterületek és munkamenetek viszont
JSON-dokumentumok maradtak: a tábláikat csak a `db_status(sync)` frissítette, és semmi nem olvasta őket. Így a rács
konténerlistája, a fül-sor és a munkamenet-panel minden kérése továbbra is teljes listát olvasott, a `container_delete`
után pedig a tükör-táblák elavulhattak. A csere érett: a táblák és a séma megvoltak.

**Döntés:**

1. **Mind a négy store a `antikolt.sqlite3`-ból olvas és ír**; a négy `*.json` (kártyák, konténerek, munkaterületek,
   munkamenetek) **export**: az átvétel, a leállás és a `db_status(sync)` írja, atomikusan, és a kapcsoló előtti
   profilon kívül semmi nem olvassa vissza.
2. **Az átvétel store-onként, a `meta` táblában jelölve** (`adopted:cards`, `adopted:containers`,
   `adopted:workspaces`, `adopted:sessions`). A `user_version` csak a sémát jelenti (`0004`); az átvétel állapota nem
   függ attól, melyik store nyílt meg előbb (a négy store négy kapcsolatot nyit ugyanarra a fájlra). A `0004` migráció
   egy v3 fájlban `adopted:cards`-ot rögzít, mert ott a kártyák már átvételre kerültek — enélkül a `cards.json`
   visszaíródhatna az újabb sorok fölé.
3. **A munkamenet-tábla a ledger teljes sorát hordozza** (állapot, házirend, cookie/háttértár-méretek, service
   workerek, `last_seen`/`last_verified`, RAM, `keep_alive`), a házirendek egy egy-soros `session_rules` táblában; az
   enumerációk a `as_db`/`from_db` leképezéssel, ismeretlen érték **hangos hiba**, nem tipp.
4. **Egy store átvétele:** a dokumentum nyer (az volt az író); hiányzó dokumentum → seed (4 konténer, 3 munkaterület,
   üres ledger); üres dokumentum → üres marad; olvashatatlan → a mag **eláll** (`*Error::Legacy`, a fájlnévvel), nem
   seedel az adat fölé.
5. **A `db_status` mind a négy exportot ellenőrzi soronként** (`checked`, `mismatches`, `verified`); olvashatatlan
   export `-1` (`not measured`), soha nem „egyezik”.
6. **A sorrendek** a felületéi: konténerek és munkaterületek a beszúrás (legend/tükör) sorrendjében (`ORDER BY rowid`),
   munkamenetek origin szerint.

**Következmény:** (+) nincs több kézi tükrözés — a `sync_reference_tables` megszűnt, mert nincs mit tükrözni; (+)
minden lista- és panelkérés indexelt olvasás; (+) az átvétel store-onként újraindítható, és egy korrupt dokumentum nem
viszi magával a többit; (−) négy kapcsolat nyílik ugyanarra a fájlra (WAL mellett rendben; a séma-migráció egyszer fut);
(−) a régi build egy v4 fájlt `user_version=4`-gyel olvas, de a JSON-t írná tovább, tehát a visszafelé ugrás nem
támogatott.

## ADR-069 — A kártyatár SQLite-first: a JSON export lesz (2026-11-21)

**Állapot:** az 1–5. pont él; a 6. pontot (a másik három store egyelőre JSON marad) az ADR-070 váltotta fel.

## ADR-069 — A kártyatár SQLite-first: a JSON export lesz (2026-11-21)

**Kontextus:** az ADR-064…066 egy tükröt épített: a JSON-fájl írt, az adatbázis követte. Ez bizonyította a migrációt
valódi adaton, de a két írás szinkronját kézzel kellett tartani — az ADR-066 épp azért született, mert a
`container_delete` `move_to`-s ága néma elavulást hagyott a tükörben. A rács listája már az adatbázisból jött, tehát
a csere értelmes fele megvolt; hiányzott az írás.

**Döntés:**

1. **A kártyák egyetlen írója az `antikolt.sqlite3`** (`CardStore` a `Db`-n keresztül): a `card_upsert`, `card_open`,
   `card_page_text`, `card_delete`, a konténer-áthelyezés és a profil-import mind oda ír. A tükör (`Db::mirror`,
   `mirror_card`, `mirror_delete`) megszűnt.
2. **A `cards.json` export**, nem store: a mag az átvételkor, leálláskor és `db_status(sync: true)`-ra írja ki (atomi
   cserével), és **egyszer** olvassa: a kapcsoló előtti profil átvételekor. Induláskor nem írja újra: az export
   származtatott, és a teljes lista sorosítása minden indításnál visszahozná a régi O(n) írást a startup útjára.
3. **Egyszeri átvétel, séma `0003`-mal:** a `Db::open` megjegyzi, milyen `user_version`-t látott; ha az régi (0–2),
   akkor a `cards.json` volt az igazság, ezért azt veszi át (a store normalizálásával), és csak **az átvétel után**
   írja be a `3`-at (`mark_switched`). Ha a JSON olvashatatlan, a mag **eláll** (`StoreError::Legacy`, a fájlnevet
   kimondva) — nem seedel a felhasználó kártyái fölé. Ha az átvétel **sorokat dob el** (a tükör elmaradt, vagy a
   dokumentum kiürült), előbb `VACUUM INTO`-val menti az adatbázist (`antikolt.pre-adoption.sqlite3`): a JSON nyer,
   de a másik másolat nem tűnik el.
4. **Az üres profil nem seedel újra:** a példakártyák csak akkor jönnek létre, ha az adatbázis fájl új **és** nincs
   átveendő JSON. Aki minden kártyáját törölte, üres rácsot kap a következő indításnál is.
5. **A `db_status` a DB ↔ export egyezést méri, soronként** (`checked`, `mismatches`, `verified`). A `sync` a
   származtatott másolatokat frissíti: az exportot a kártyákból, a konténer/munkaterület referenciatáblákat a
   JSON-store-okból. Az olvashatatlan export `mismatches = -1` (`not measured`), soha nem „egyezik”.
6. **A konténerek, munkaterületek és munkamenetek maradnak JSON-dokumentumok** — a saját cseréjük külön szelet;
   addig a `db_status` referenciatáblái igény szerint frissülnek, és semmi nem olvassa őket.

**Következmény:** (+) egy író van, tehát nincs többé kézi tükrözés és nincs „a darabszám egyezik, mégis elavult sor”
hibaosztály (ez volt az ADR-066 oka); (+) a `container_delete` áthelyezése egyetlen SQL-`UPDATE`; (+) a JSON megmarad
emberi szemnek és biztonsági másolatnak, de már nem tévedhet a termék útjára; (−) a `cards.json` az utolsó
export/leállás után elavulhat — ezt a `db_status` ki is mondja (`verified: no`), a napló pedig rögzíti; (−) a régi
build egy új fájlt `user_version=3`-mal olvas, de a tükröt írná tovább, tehát a visszafelé ugrás nem támogatott.

## ADR-068 — A törlés két lépés: előbb felold, aztán töröl (2026-11-19)

**Kontextus:** a `cards.delete` (ADR-061) egy kattintásra törölt. A kártya viszont **nagy kattintási felület**,
amire a felhasználó azért kattint, hogy megnyissa — egy `×` ezen belül egyetlen téves kattintással adatot visz el.

**Döntés:** a kártya meta-sorában egy `×` van, és a törlés **két lépés**:

1. az első kattintás **feloldja** a kártyát: a gomb szövege `Biztos?`, piros háttérrel, és a lap-oldali állapot
   (`armedDelete = {id, at}`) ezt mutatja;
2. az **öt másodpercen belüli** második kattintás töröl (`cards.delete`, a mag válasza után rajzol);
3. öt másodperc után a feloldás **magától lejár**, mert egy percre feloldva hagyott kártya csapda a következő téves
   kattintásnak.

**Következmény:** (+) egy téves kattintás nem visz el kártyát; (+) a megerősítés **nem dialógus**, tehát nem ütközik
a CEF `prompt`-problémával (ADR-053) és mérhető; (−) a mérés sokáig elmaradt: előbb a CEF GPU-folyamat halt el
(`GPU state invalid …`, `exit_code=-2147483645`), majd a motor stdio-életciklusa (`stdin closed` → a motorház kilép).
Mindkettő feloldva (napló 90–91. szelet), és a `--addcardtest` azóta **mért, zöld**:
`delete_armed=ok, delete_card=ok (remaining=10 expected=10), add_card_check=ok` (`exit=0`, 11,9 s).

## ADR-067 — Az adatbázis a store sorrendjében adja a kártyákat (2026-11-19)

**Kontextus:** a JSON-store Rustban rendezett (`last_opened_at_ms` szerint csökkenő, majd cím). Miután a rács listája
az adatbázisból jön (ADR-065), a rendezés nélküli `SELECT` **beszúrási sorrendet** adott — a rács csendben
abbahagyta volna az átrendezést egy kártya megnyitásakor. Ez pontosan az a fajta néma viselkedésváltozás, amit egy
store-csere nem okozhat.

**Döntés:** `ORDER BY last_opened_at_ms DESC, LOWER(title) ASC` az `all_cards()`-ban, és egy Rust-teszt
(`the_database_returns_the_stores_order_not_the_insertion_order`), amely **szándékosan fordított beszúrási
sorrenddel** ír be két kártyát, és elvárja a store sorrendjét. Ha valaki kiveszi az `ORDER BY`-t, a teszt elbukik.

**Következmény:** (+) a rács sorrendje a váltás után is ugyanaz, mint előtte, és ezt teszt védi; (−) a rendezés most
két helyen él (Rust-store és SQL) — a JSON-store kivezetésekor az SQL marad az egyetlen.

## ADR-066 — A tükör soronként követi az írásokat; a lista már csak olvas (2026-11-19)

**Állapot:** elavult — az ADR-069 felváltotta: a kártyáknak egyetlen írója van (az adatbázis), a tükör megszűnt,
ezért többé nincs mit kézzel tükrözni.

**Kontextus:** az ADR-065 a lista-kérésnél írta újra a teljes tükröt — helyes, de O(n) írás kérésenként.

**Döntés:**

1. **Minden kártya-írás frissíti a saját sorát** (`mirror_card`), a törlés törli (`mirror_delete`); a lista csak
   olvas.
2. **Egy op, ami több kártyát ír, mindegyiket tükrözi** — a `container_delete` `move_to`-s ága különben néma
   elavulást okoz: a darabszám egyezik, tehát semmilyen önjavítás nem indul, és a rács a régi konténert mutatja.
   Ezt a `--containertest` fogta meg (`container_move_delete=FAILED … card_in=c-proba`).
3. **Önjavítás csak darabszám-eltérésnél**, és akkor is **hangosan** (`[core] db_list_out_of_step … — rebuilding`).

**Következmény:** (+) a lista-kérés olvasás, nem írás; (+) a több-kártyás műveletek tükrözése explicit és mérve;
(−) a JSON-store még mindig az író, tehát a két írás szinkronját kézzel kell tartani — ezt szünteti meg a következő
szelet (írás a SQLite-ba, JSON mint export).

## ADR-065 — A rács listája az adatbázisból jön (a csere második fele) (2026-11-19)

**Állapot:** elavult — az ADR-069 felváltotta: a lista továbbra is az adatbázisból jön, de már nem kell előtte
tükrözni, mert az írás is oda megy.

**Kontextus:** az ADR-064 megteremtette a tükröt, de a rács továbbra is a JSON-fájlból olvasott. A csere értelme az
olvasás: amíg a lista a JSON-ból jön, a séma, az indexek és a lekérdezések nem jelentenek semmit.

**Döntés:** a `cards_list` op **előbb tükröz, aztán olvas** (write-through):

1. a JSON-store továbbra is az **író** (a `card_upsert`/`card_open`/`card_page_text`/`card_delete` oda írnak),
2. a lista kérésekor a mag a JSON-listát beírja az adatbázisba (`Db::mirror`, tranzakciós csere), majd **az
   adatbázisból** olvassa vissza a kártyákat,
3. ha a darabszám nem egyezik (tehát a tükör nem lehet jó), a mag **nem szolgál ki bizonytalan adatot**: a JSON-listát
   adja vissza, és `[core] db_list_mismatch` sorral jelzi a stderr-en. Ugyanígy, ha az adatbázis nem nyílik meg.

**Következmény:** (+) a rács ma már az adatbázisból olvas, és a váltás nem kockáztat adatvesztést, mert az írás
helye nem változott; (+) a hibás esetek **hangosak** (stderr), nem csendes visszaesés; (−) a tükrözés minden
lista-kérésnél O(n) írás, ami 5 000 kártyánál pazarlás — tudatos köztes állapot, amit a következő szelet szüntet meg
azzal, hogy az **írás** is az adatbázisba megy, és a JSON csak export lesz.

## ADR-064 — Az adatbázis megérkezik: SQLite-séma `0002`, tükrözéssel (2026-11-19)

**Állapot:** elavult — az ADR-069 felváltotta: a séma `0003`, és a kártyák írása is az adatbázisba megy, nem
tükörbe.

**Kontextus:** a négy store JSON-dokumentum, atomikus írással — olvasható és elég 10 kártyához, de **semmire nem
tud kérdezni**. Minden „mely kártyák vannak ebben a konténerben", „mi jár le", „mennyi van ebben a
munkaterületben" kérdés az egész listán végigmegy. 5 000 kártyánál ez a rossz válasz, és ezért szerepel a
store-csere a munkarendben.

**Döntés:**

1. **`rusqlite` `bundled` funkcióval**: a SQLite a binárisba fordul, nincs rendszer-DLL függés.
2. **A séma `PRAGMA user_version`-ben** (nem táblában): `2` a mai verzió; a `cards` tábla a JSON-rekord minden
   mezőjét viszi (`page_text`, `workspace`, `tags` JSON-szövegként — nincs join-tábla egy 12 elemű listára).
3. **A migráció idempotens**, és a JSON-store-ok **maradnak az igazság** egyelőre: a `db_status` op `sync: true`
   mellett tükrözi a négy store tartalmát a fájlba, majd **visszaolvassa az első kártyát** és összehasonlítja a
   mezőket (`verified`). A csere akkor történik meg, ha ez a tükör a valódi adaton bizonyítottan egyezik.
4. **WAL naplózás**: olvasó ne várjon íróra, és egy összeomlás ne vigye el az utolsó tranzakciót.

**Következmény:** (+) van adatbázis, migráció, kártya-round-trip és két valódi lekérdezés (munkaterület-számlálás),
mind mérve a Rust-tesztekben; (+) a JSON-store-ok érintetlenül működnek, tehát a termék nem lett kockázatosabb;
(−) **a csere még nem történt meg**: a shell továbbra is a JSON-fájlokat olvassa, a `db_status` op egyelőre
mellékvágány; a tükrözés (a `sync`) csak akkor hasznos, ha valaki hívja — a shell mai útja nem hívja.

## ADR-063 — A webcím is szerkeszthető a kártyán (2026-11-19)

**Kontextus:** a cím (ADR-062) és a jegyzet szerkeszthető volt, a **webcím** nem — pedig a kártya *célja* az a
mező, ahol egy elgépelés csendben megmarad: a kártya továbbra is a régi oldalt nyitja, és a felhasználó azt hiszi,
átírta.

**Döntés:** a meta-sor domainje a fogantyú — **dupla kattintás → beviteli mező → `Enter` ment, `Esc` mégse**, a
mentés a teljes rekord `cards.upsert`-jével. A beírt cím normalizálódik (ha nincs séma, `https://` lesz), és a
megjelenített domain a mag válaszából újraszámolódik (`pageCardFromCore`), tehát a felület sosem hazudik arról,
hová mutat a kártya. A mentés **előbb ír, aztán állítja vissza a DOM-ot** — az a sorrend, amit a cím-szerkesztő a
`blur` újrabelépése miatt tanult meg (ADR-062).

**Következmény:** (+) a kártya minden emberi mezője (cím, webcím, jegyzet, címke, konténer, munkaterület)
szerkeszthető a felületen; (+) a domain a mag válaszából jön, nem abból, amit a felhasználó *gépelt*; (−) a
webcím-váltás nem nyitja újra a kártyát (a következő megnyitás a friss címet használja), és a régi oldal
rögzített szövege (ADR-049) a régi tartalom marad, amíg a kártyát újra nem fagyasztják — ezt a napló kimondja.

## ADR-062 — A kártya átnevezése helyben (2026-11-19)

**Kontextus:** az ADR-061 után kártyát tenni és venni lehetett, de a **címét nem** lehetett átírni a felületen — a
`cards.upsert` tudta, a lap nem hívta.

**Döntés:**

1. **Dupla kattintás a címre** → a cím helyén beviteli mező (`Enter` ment, `Esc` mégse), és a mentés ugyanazon az
   `cards.upsert` úton megy, ami a teljes rekordot viszi (tehát a konténer, a címkék, a jegyzet és a rögzített
   oldalszöveg nem veszik el).
2. **A lap-oldali út nevén is elérhető** (`window.antikoltRenameCard(id, title)`), mert egy jövőbeli menüpontnak
   nem kell dupla kattintást szimulálnia.
3. **A szerkesztő a mentést közvetlenül a lap-úton hívja** (nem eseményen át): a kártya shadow-tartalmából indított
   esemény nem ért el a rácsig, és egy felhasználó által gépelt változás nem függhet egy eseménytől, ami nem érkezik
   meg. Az esemény csak tartalék egy lap nélküli környezetben.

**Következmény:** (+) a cím átírása a felületen működik — a dupla kattintás megnyitja a mezőt, az `Enter` ment, és a
mag válasza rajzolódik ki (`--addcardtest`: `wrote_by=gesture`); (+) a teljes rekord megy át, tehát nem veszhet el
konténer, címke, jegyzet vagy oldalszöveg; (−) a szerkesztő **előbb ír, aztán állítja vissza a DOM-ot**: az első
változat a cím visszaállításával kezdte, mire a böngésző `blur`-t küldött, annak `finish("", false)` ága
újra belépett és elszállt, így a mentés elmaradt — ezt a mérés (`wrote_by=path`, üres `editor`) mutatta ki, és ezért
fordult a sorrend; (−) a webcím (URL) szerkesztése továbbra sincs.

## ADR-061 — A kezdőlap szerkeszthető: kártya hozzáadása és törlése a lapon (2026-11-19)

**Kontextus:** a felhasználó nem tudott kártyát létrehozni a felületen — a rács a mag kártyáit mutatta, de új
kártya csak a mag `cards.upsert`-jével kerülhetett bele. Egy böngésző, amibe nem lehet kártyát tenni, nem
böngésző. A törlés sem létezett: a magban nem volt `card_delete`.

**Döntés:**

1. **`＋` gomb a fül-sor mellett**, soros űrlap a rácson (`webcím` + opcionális `cím`), `Enter` ment, `Esc` mégse.
   A dialógus ugyanazért nincs, mint máshol (ADR-053): a CEF alatt a `prompt` csendben `null`-t adna.
2. **Egy kártyának két dolga van, amit egy űrlap nem kérdezhet meg**: a munkaterület és a konténer. Ezért az új
   kártya az **aktív fülre** és az **első konténerbe** kerül — ez a kettő a felhasználó számára „itt és most".
3. **Az id ULID-alakú** (`01M7T7…`, 26 karakter), mert a store a létrehozás sorrendjét így tudja lexikografikusan
   is; a cím, ha üres, a webcím domainje lesz.
4. **`card_delete` a magban** (`CardStore::remove`): a törlés a hozzáadás párja, és a felületnek szüksége van rá,
   hogy egy elgépelt webcímet ne kelljen a fájl szerkesztésével javítani.
5. **A törlés a kártyán** a lap-úton (`antikoltDeleteCard`) megy, a mag válasza után rajzol — mint minden más írás.

**Következmény:** (+) a kezdőlap „szerkeszthető" a legfontosabb értelmében: kártyát tenni és venni lehet; (+) a
hozzáadás pontosan ugyanazon az `cards.upsert` úton megy, mint a kitűzés/jegyzet/címke/konténer, tehát a rács
soha nem mutat olyat, amit a mag nem fogadott el; (−) a kártya **szerkesztése** (cím/webcím/jegyzet átírása a
felületen) még nincs, csak a létrehozás és a törlés; (−) a törlés megerősítés nélkül azonnali — a
`confirmed`-lépés egy későbbi döntés, mert a megerősítő dialógus ugyanaz a CEF-probléma.

## ADR-060 — A köteg útja a hídon: az útvonalat a hívó adja, a híd nem nyit dialógust (2026-11-19)

**Kontextus:** az ADR-059 a magban kész tette a költöztetést, de a lap nem érte el: sem a híd, sem a oldal nem
tudott exportálni. A „Mentés fájlba" gomb viszont natív `QFileDialog`-ot kíván, amit a harness nem tud
kattintani — és egy híd-kérésen belül megnyitott dialógus meg is akasztaná a lap válaszát.

**Döntés:**

1. **A híd `profile.export` / `profile.import` metódust kap, `path` argumentummal.** Az útvonalat **a hívó adja**;
   a híd soha nem nyit natív felületet egy kérés közepén.
2. **A lap-oldali belépő:** `window.antikoltProfileWrite(path)` / `window.antikoltProfileRead(path)` — ez az a
   pont, amit a gomb (és a teszt) hív. A dialógus később *ezek elé* kerül, a híd változatlan marad.
3. **A teszt a valódi úton mér:** `--profiletest` a két fejetlen mag-pár után egy **ablakot** nyit, a lapon
   keresztül exportál a kötegbe, majd visszaolvassa — a választ a lap saját változójából (`window.__profWrite`)
   olvassa, mert az `evaluate` nem várja meg a promise-t.

**Következmény:** (+) a költöztetés a felületig ér, és mérhető marad (nincs dialógus a mérés útjában); (+) a
híd szerződése nem tartalmaz felületi döntést; (−) a *gomb* és a fájlválasztó még nincs, tehát a felhasználó ma
még nem tud kattintani — ezt a `19-FOLYTATAS.md` §12 és a `21-ALLAPOT.md` mondja ki.

## ADR-059 — A profil egy hordozható JSON-kötegben utazik (`profile_export` / `profile_import`) (2026-11-19)

**Kontextus:** a felhasználó adata négy külön fájlban él a profilban (`cards.json`, `containers.json`,
`workspaces.json`, `sessions.json`). A dokumentáció „költöztetést" ígér, de nem volt rá mód: egy gépről a másikra
vinni a profil mappáját kellett volna — kézzel, a fájlok nevének ismeretében. Ez nem felhasználói funkció.

**Döntés:**

1. **Egy köteg (`bundle`): `schema`, `created_at_ms`, `cards`, `containers`, `workspaces`, `sessions`, `rules`.**
   JSON, nem zip: a négy store már JSON, a köteg kicsi, és egy szövegszerkesztővel olvasható biztonsági mentés az,
   amiben az ember meg tud bízni.
2. **`profile_export {path}`** a store-okból ír (atomikusan: temp + átnevezés, mint minden más írás), és
   visszamondja, mit tett át (kártyák/konténerek/munkaterületek/munkamenetek).
3. **`profile_import {path}` előbb validál, aztán ír.** Az ismeretlen `schema` **elutasítás** (`version_mismatch`),
   nem találgatás — és a fájl olvasása az *első* lépés, mert egy félig sikerült import rosszabb, mint egy el sem
   kezdett. A store-ok `replace_all`-lal cserélődnek, a kártyák ugyanazon a normalizáláson mennek át, mint egy
   írásnál (tehát importtal sem lehet megkerülni a címke-szabályt).
4. **A `sessions` a kötegben van**, mert a bejelentkezések és a takarítási szabály is a profil része.

**Következmény:** (+) a profil egy fájllal költöztethető és menthető, és a `--profiletest` méri, hogy a friss
profilba történő import után a *fájl* azt tartalmazza, amit az export kivitt; (+) a hibás/újabb köteg
elutasítása kimondja, mit beszél ez a mag; (−) a köteg **titkokat is tartalmaz** (munkamenet-sorok, sütik
metaadatai), ezért a fájl védelme a felhasználó dolga — a titkosítás egy későbbi döntés, és ezt a
`19-FOLYTATAS.md` kimondja.

## ADR-058 — A kártya munkaterülete a kártyán váltható, natív `<select>`-tel (2026-11-19)

**Kontextus:** az ADR-056/057 után a munkaterület-tábla és a fül-sor életciklusa a magban élt, de egy kártya
munkaterületét a felületen nem lehetett áttenni — csak a programozott úton (`antikoltMoveToWorkspace`). Egy
beállítás, amit a felhasználó nem ér el, nem beállítás.

**Döntés:** a kártya meta-sorában, a konténer-választó mellett egy **második, keskenyebb natív `<select>`** van
(`.workspace-select`, `aria-label="Munkaterület"`). A váltás ugyanaz az út, mint a konténeré: `change` esemény →
`setWorkspace` → `cards.upsert` → a mag válasza rajzol. **Miért nem fogd-és-vidd:** az drag a rács virtualizált
DOM-jában (a kártya egy `antikolt-card` shadow-host, ami a görgetéskor újra felépül) törékeny, és a
billentyűzettel nem használható; a `<select>` mindkettőt ingyen adja. A drag később kerülhet *mellé*.

**Következmény:** (+) a munkaterület-váltás a felületen is elérhető, a konténer-váltással azonos gesztussal; (+)
a teszt ugyanazt a választót használja, amit a felhasználó (`--workspacetest`); (−) két legördülő a meta-sorban
zsúfoltabb, ezért a munkaterület-választó kisebb betűvel és keskenyebben jelenik meg.

**Megerősítve (123. szelet):** a „később kerülhet *mellé*" ág megérkezett — a kártya `draggable`, a fülre dobható
(`application/x-antikolt-card` MIME-típus, a fül `dragover`-je csak ezt fogadja el), a `drop` ugyanazt a
`setWorkspace`-et hívja, mint a `<select>`; a `<select>` marad a billentyűzetes út. A huzalt a `--workspacedragtest`
bizonyítja (`workspace_drag_wired`/`_gate`/`_move`/`_restored` mind `ok`).

## ADR-057 — A munkaterület életciklusa a fül-soron, ugyanazzal a gesztussal, mint a konténereké (2026-11-19)

**Kontextus:** az ADR-056 a munkaterület-táblát a magba tette, de a felületen nem lehetett munkaterületet
létrehozni, átnevezni vagy törölni — a híd tudta, a felület nem hívta. Ugyanaz a hiány, mint a konténereknél az
ADR-053 előtt.

**Döntés:** a fül-sor ugyanazt a három gesztust kapja, ugyanabban a formában: `＋` (soros beviteli mező, `Enter`
ment, `Esc` mégse), dupla kattintás a fülön (átnevezés), `×` (törlés). **Nincs dialógus** — a `prompt` a CEF
alatt csendben `null`-t adna (ADR-053). Egy használt munkaterület törlése a mag `in_use` válaszát **megjeleníti**
a fül-soron („1 kártya még ebben a munkaterületben van"), nem nyeli el. Az új id a névből származó slug
(`w-` + ékezet nélküli alak, ütközésnél sorszám).

**Következmény:** (+) a munkaterület-életciklus a felületen teljes, és minden művelet a magon megy át, amelynek
a válaszát a fül-sor rajzolja; (+) a két tábla (konténer, munkaterület) ugyanazt a mintát követi, tehát egy
felhasználónak egy gesztust kell megtanulnia; (−) a kártya munkaterületének *áthelyezése* még csak a programozott
úton (`setWorkspace`) megy, fogd-és-vidd/menü nélkül.

## ADR-056 — A munkaterületek is a magban élnek, és a kártya munkaterülete valódi mező (2026-11-19)

**Kontextus:** a lap `pageCardFromCore`-ja a kártya *helyéből* találta ki a munkaterületet
(`index > 12 ? "w-read" : "w-home"`). Ez nem közelítés volt, hanem hiba: minden tizenharmadik kártya némán az
„Olvasnivaló" fülre került, akár akarta a felhasználó, akár nem — és a fül sora is a lapban élt, nem a magban.

**Döntés:**

1. **`workspaces.json` a magban** (`workspaces_list` / `workspace_upsert` / `workspace_delete`), az első
   indításkor a három leszállított munkaterülettel (`w-home` Otthon, `w-work` Munka, `w-read` Olvasnivaló).
2. **`Card.workspace: String`** (`#[serde(default)]`): a kártya munkaterülete **tárolt adat**. Az üres mező az
   első munkaterületet jelenti — ez az, amit a tábla előtti kártyák jelentenek, és egy hiányzó mező nem
   költöztethet kártyát olyan fülre, amit a felhasználó nem választott.
3. **A tábla pushban megy** a lapra a kártyalistával és a konténer-táblával együtt (`antikoltAcceptWorkspaces`),
   ugyanazért, mint a konténereknél (ADR-052): a híd szkript a load végén települ, ami később lehet, mint a lap
   saját szkriptje.
4. **A törlés ugyanaz a szabály**: amíg kártya van benne, `in_use` — a csendes áthelyezés tilos. Az áthelyezés a
   kártyán keresztül megy (`cards.upsert`, `workspace` mező), a lap-út (`setWorkspace`) kész; a fogd-és-vidd
   felület még nincs.

**Következmény:** (+) a tizenharmadik kártya már nem kerül magától az Olvasnivalóba; (+) a tabok neve és sora a
magé, tehát egy átnevezés túlél egy újratöltést; (+) a kártya munkaterülete ugyanazon az úton íródik, mint a
konténer, a címke, a jegyzet és a kitűzés; (−) a kártya munkaterületének *felületi* mozgatása (drag/menü) még
nincs, csak a programozott út — ezt a `19-FOLYTATAS.md` §12 mondja ki.

## ADR-055 — A címkék a kártyán, a magban normalizálva (2026-11-19)

**Kontextus:** a keresőmező placeholderje ősidők óta „címke"-t ígér (`Keresés — cím, webcím, jegyzet, címke,
oldalszöveg`), a kártyarekordban viszont nem volt címke-mező: az ígéret hazugság volt. A lap `matchesTerm`-je
mégis vizsgált egy `card.tags` tömböt, ami soha nem létezett.

**Döntés:**

1. **`Card.tags: Vec<String>`** a magban (`#[serde(default)]`, tehát a régi `cards.json` gond nélkül betöltődik).
2. **A normalizálás a magé**: trim, kis-nagybetűtől független deduplikáció (az első írásmód marad), legfeljebb 12
   címke, egyenként legfeljebb 32 karakter. A szabály a store-ban él, mert ott találkozik minden író (a lap, egy
   későbbi importőr, a tesztek) — a lapban élő szabályt az importőr nem követné. A hosszra vágás **nem hiba**:
   egy hosszú címke rövidítve is címke, a mentés elvesztése viszont adatvesztés.
3. **A szerkesztés a kártyán, helyben**: a chipek mellett `＋címke` gomb nyit egy beviteli mezőt; `Enter` ment,
   `Esc` mégse (ugyanaz a minta, mint a konténereknél — nincs JS-dialógus, ADR-053). A törlés a chip `×`-e.
4. **A keresés ékezet nélkül is látja** (`kavezo` → `kávézó`), mert a címkék ugyanabba a haystackbe kerülnek, mint
   a cím, a webcím, a jegyzet és az oldalszöveg.

**Következmény:** (+) a kereső ígérete igaz lett, és a `matchesTerm` halott ága él; (+) a címkék ugyanazon a
`cards.upsert` úton íródnak, mint a kitűzés és a jegyzet, tehát a rács a mag válaszát mutatja; (−) a címkék még
nem szerepelnek a `.antikolt` exportban külön listaként (a kártyarekord részei), és nincs címke-autocomplete.

## ADR-054 — A használt konténer törlése áthelyezéssel (2026-11-19)

**Kontextus:** az ADR-052 óta a mag megtagadja egy olyan konténer törlését, amiben még kártya van (`in_use`).
Ez helyes volt — nincs csendes áthelyezés —, de **zsákutca**: a felhasználó nem tudta törölni a régi
csoportosítását.

**Döntés:**

1. **A `container_delete` opcionális `move_to` mezőt kap.** Ha a konténer használt és van `move_to`, a mag
   **előbb áthelyezi** a kártyákat (egy mentésben, `CardStore::move_container`), **azután töröl**, és a válasz
   megmondja, hány kártya vándorolt (`moved`). Ha nincs `move_to`, marad az `in_use` hiba; ha a `move_to` nem
   létezik vagy maga a törölt konténer, `invalid_argument`/`not_found`.
2. **A sorrend nem véletlen:** előbb az áthelyezés íródik ki, aztán a törlés. Ha a törlés elhasal, a kártyák
   akkor is valahol vannak — nem tűnnek el.
3. **A felület a zsákutcát feloldássá alakítja:** a `×` után, ha a mag `in_use`-t mond, a legenda egy sort nyit
   („1 kártya még ebben a konténerben van"), benne egy választóval a **többi** konténerre és egy
   „Áthelyezés és törlés" gombbal. Egy kattintás elvégzi mindkettőt.

**Következmény:** (+) a régi csoportosítás mindig törölhető, de soha nem csendben; (+) a mag dönt, a lap csak
felajánl; (−) a felület csak *egy* célt kínál egyszerre (nincs „mindet ide, kivéve ezt"); és a kártyák
áthelyezése nem kérdez, amiért az `in_use` hibaüzenet továbbra is létezik a hídon.

## ADR-053 — A konténer szerkesztése a helyén, dialógus nélkül (2026-11-19)

**Kontextus:** az ADR-052 a táblát a magba tette, de a felületen nem lehetett új konténert létrehozni, átnevezni
vagy törölni — a híd tudta, a felület nem hívta.

**Döntés:**

1. **Egy soros, helyben szerkesztő** a legenda végén: a `＋ konténer` gomb vagy egy chip dupla kattintása kiír egy
   beviteli mezőt, `Enter` ment (a form `submit`-ja), `Esc` mégse. **Nincs `prompt`/`confirm`**: a CEF oldali
   JS-dialógushoz a házigazdának `CefJSDialogHandler`-t kellene adnia, ami nincs — a `prompt` hívás csendben
   `null`-t adna, és a felület „nem csinál semmit".
2. **Az új id a névből származik** (`c-` + ékezet nélküli slug, ütközésnél sorszám), hogy egy ember is ki tudja
   mondani; a színt a mag generálja (ADR-052).
3. **Törlés `×`-szel**, és a mag `in_use` válasza **megjelenik** a legendában („1 kártya még ebben a
   konténerben van") — egy csendben elnyelt hiba pont az a fajta hazugság, amit ez a projekt nem enged.

**Következmény:** (+) a konténer-életciklus a felületen teljes: létrehozás, átnevezés, törlés, szűrés; (+) minden
művelet a magon megy át, és a legenda a mag válaszát rajzolja; (−) a beviteli mező a legenda sorában jelenik meg
(a rács egy sorral lejjebb csúszik), és nincs „mégse" gomb, csak az `Esc`/`Enter` — dokumentálva a hintben.


## ADR-052 — A konténer-tábla a magban él, és pushban érkezik a lapra (2026-11-19)

**Kontextus:** a konténerek neve és színe eddig a lapban volt (`PAGE_CONTAINERS`, egy konstans), a mag csak a
kártyákon tárolt egy `container` id-t. Egy átnevezés így nem élt túl egy újratöltést, és a „ugyanaz" a konténer
két rétegben két dolgot jelenthetett.

**Döntés:**

1. **A tábla a magé** (`containers.json`, `containers_list` / `container_upsert` / `container_delete`). A mag az
   első indításkor a négy leszállított konténert veti el, tehát egy régi profil semmit nem veszít. A szín
   `#RRGGBB`; ha hiányzik vagy hibás, a mag determinisztikus színt generál (ugyanaz az id → ugyanaz a szín).
2. **A törlés nem mozgat csendben.** Amíg kártya hivatkozik egy konténerre, a `container_delete` `in_use`
   hibával utasít: egy csendes áthelyezés az „eltűntek a kártyáim" hibaosztály.
3. **A lap a magét rajzolja, és a tábla pushban jön** a kártyalistával együtt (`antikoltAcceptContainers`). Nem
   a lap kérdez: a híd szkriptje a load *végén* települ, ami később lehet, mint a lap saját szkriptje — mért
   hiba volt, hogy a legenda a fallback táblát rajzolta, miközben a magban már ott volt az ötödik konténer.
4. **A lap konstansa csak fallback**: böngészőben, mag nélkül a négy leszállított konténer jelenik meg.

**Következmény:** (+) az átnevezés és az új konténer túlél egy újratöltést, mert a magban van; (+) a szín- és
névforrás egy helyen van, tehát a pont és a legenda nem tud széttartani; (+) a törlés nem rendez át kártyákat
észrevétlenül; (−) a konténer *létrehozásának/átnevezésének* felületi gombja még nincs (a híd tudja, a felület
nem hívja), és a lap fallback táblája az `app.js`-ben marad.


## ADR-051 — A konténer-jelmagyarázat egyben a szűrő (2026-11-19)

**Kontextus:** a kártyákon lévő színes pont megmutatta, melyik konténerbe tartozik egy kártya, de a színek
jelentését semmi nem mondta meg, és a konténer szerint szűrni nem lehetett.

**Döntés:**

1. **Egy sor chip a rács fölött**, konténerenként: színes pont + név + **darabszám**. A szám a jelenlegi
   keresésre vonatkozik, nem a teljes tárra — egy jelmagyarázat, ami „3 kártyát" ír, miközben a keresés elrejt
   kettőt, hazudik.
2. **A chip maga a szűrő**: kattintásra a rács csak az adott konténer kártyáit mutatja, újabb kattintás törli.
   Egyszerre egy konténer lehet aktív: a kérdés, amire válaszol, az „ez melyik konténer?", nem az „ez melyik
   három konténer metszete?".
3. **A szűrő nézet-állapot, nem tárolt beállítás.** Új indítás mindent mutat: egy elfelejtett szűrő csapda
   („hol vannak a kártyáim?"). A chipek `aria-pressed`-szel jelzik az állapotot, és a számok a szűréssel együtt
   frissülnek.

**Következmény:** (+) a színek jelentése látható, és a konténer szerinti szűrés egy kattintás; (+) a számok a
kereséssel együtt mozognak, tehát nem tudnak ellentmondani a rácsnak; (−) a szűrő nem marad meg munkamenetek
között (tudatos), és a konténer *tábla* továbbra is a lapban él, nem a magban — új konténer létrehozása és a
tábla magba költöztetése nyitott.

## ADR-050 — A konténer-váltás a kártyáról, natív `<select>`-tel (2026-11-19)

**Kontextus:** a konténerek eddig csak *látszottak* (színes pont a kártyán, a lap saját táblájából), de a
kártya-konténer kötést a felületen nem lehetett megváltoztatni — a roadmap „kártya-konténer kötés, konténer-
váltás" tétele nyitott volt.

**Döntés:**

1. **A választó a kártyán van**, a meta sorban: egy natív `<select>`, aminek az opciói a konténerek. Nem
   egyedi legördülő: a natív elem hozza a billentyűzet-kezelést, a képernyőolvasót és a platform menüjét
   ingyen, a színes pont pedig megmarad mellette (a pont szín = a konténer színe).
2. **Az írás a megszokott úton megy**: `antikolt-container` esemény → a rács `setContainer`-je →
   `cards.upsert` → a mag. A rács csak azt mutatja, amit a mag elfogadott (ugyanaz a szabály, mint a
   kitűzésnél és a jegyzetnél), és a hiba a `window.antikoltContainerState()` witnessben látszik.
3. **A modell és a rekord külön marad**: a nézet konténer-*objektumot* tart (szín + név), a rekord csak id-t,
   ezért a válasz id-ját a lap ugyanazon a táblán fordítja vissza, amivel a kártyákat olvasta.

**Következmény:** (+) a kötés a kártyán, egy mozdulattal átállítható, és a magé a döntés; (+) a szín és a név
egy helyről (a lap táblájából) jön, tehát nem tud széttartani a pont és a menü; (−) a konténer *tábla* még a
lapban él, nem a magban — új konténer létrehozása, átnevezése és a konténer szerinti szűrés továbbra is nyitott
(a `19-FOLYTATAS.md` §12).

## ADR-049 — Az oldalszöveg is kereshető, de csak a lemezre mentett kártyáknál (2026-11-19)

**Kontextus:** az ADR-047 kimondta, hogy a kereső nem ígér oldalszöveget, mert semmi nem indexeli a cikkek
szövegét. Ez őszinte volt, de hiányos: a felhasználó gyakran egy **mondatra** emlékszik, nem a címre.

**Döntés:**

1. **A szöveg a fagyasztáskor keletkezik.** Amikor a kártya lemezre megy, a shell megkérdezi a laptól a
   `document.body.innerText`-jét (max. 8000 karakter), és a mag `card_page_text` opjával a kártya rekordjába
   írja. Nem külön index: a kártyarekord egy mezője, tehát nincs új szolgáltatás, amit karban kell tartani.
2. **Külön op, nem `card_upsert`.** A shell nem birtokolja a teljes rekordot (konténer, jegyzet, cím);
   egy félkész kártya visszaküldése felülírná, amit sosem olvasott. A `card_page_text` ezért csak a szöveget
   írja, az id alapján.
3. **Csatolás és sapka.** A mag whitespace-t összevon, és `MAX_PAGE_TEXT = 16 000` karakterre vág. A
   kártyalista egyetlen JSON-dokumentum: 500 kártya × 16 kB a felső korlát, nem 200 MB. A lap oldala is vág
   (8000), tehát a hálózati út is korlátos.
4. **A pin és a jegyzet nem törli.** A lapról érkező `card_upsert` sosem hoz szöveget; ha üresen jön, a mag
   megtartja a korábbi capture-t. Csak egy új fagyasztás írja felül.
5. **A keresés mindhárom helyen látja**: a mag `cards_list`-je kiadja a `page_text` mezőt, a shell
   `cards.search`-je a foldolt haystackbe teszi, a lap `matchesTerm`-je pedig a `pageText` mezőt is nézi.

**Következmény:** (+) egy cikkben látott szó megtalálja a kártyát, ékezet nélkül is; (+) nincs új index, nincs
új szolgáltatás; (−) csak a **fagyasztott** kártyáknál működik (élő kártyánál nincs még szöveg), és a szöveg a
fagyasztás pillanatában érvényes — későbbi oldalváltozást nem követ; (−) a kártyarekord nő, ezért a sapka.

## ADR-048 — A takarítás sem némítja el a játszó kártyát (2026-11-19)

**Kontextus:** a hibernálás régóta kivételt tesz a médiát játszó kártyával (docs/HU/02 §4.2), a **munkamenet-
takarítás viszont nem**: ha a `wipe`/`sweep` politika szerint esedékes origin éppen a képernyőn lévő kártyáé volt,
a shell azonnal törölte a storage-ját, és egy előtérben játszó videó elhallgatott. A `19-FOLYTATAS.md` §2 ezt
nyitott tételként tartotta számon.

**Döntés:** a takarítás ugyanazt kérdezi meg a laptól, mint a hibernálás — egyetlen, mindkét helyen használt
szkripttel (`LiveMediaPlayingScript`: `video`/`audio`, `!paused && !ended`) —, és ha a válasz `playing`, akkor az
**élő** origin törlése kimarad, a státuszsor pedig megmondja, miért („a kártya médiát játszik — az élő
munkamenet megmarad"). A nem élő originok terve változatlanul a következő megnyitásra vár.

**Miért a lap a döntéshozó, és nem a shell találgatása:** a lejátszás állapota a dokumentum tulajdonsága
(`paused`/`ended`), nem a shell könyveléséé; a címből vagy az URL-ből következtetni pont az a fajta tippelés,
amit a projekt máshol is elkerül. Egy oldal, amelyik nem válaszol (hiba), nem kap privilégiumot: ilyenkor a
takarítás a régi módon lefut.

**Mérés (ezen a gépen):** a `--mediacleantest` a magon keresztül vet fel egy esedékes `wipe` sort, megnyitja az
origin kártyáját, és takarítást kér. A gépnek **nincs hangeszköze**, ezért a Chromium el sem indítja a lejátszót
(mért: `paused=true`, `readyState=0`, és a `/tone.wav` kérés **el sem indult**) — ezt a teszt ki is mondja
(`media_skip=not-measurable`), nem tesz úgy, mintha mérte volna. Amit mért: a takarítás az új, média-tudatos
úton fut le, és a `playing` válasz nélkül (idle) a törlés megtörténik (`media_wipe_when_idle=ok`), a kártya
storage-ja üres lesz. Hangeszközös gépen ugyanez a teszt a `playing` ágat is méri (a kód ott van, a
`media_skip` sor vár rá).

**Következmény:** (+) a háttértakarítás nem tud elhallgattatni egy előtérben játszó oldalt; (+) a két út
(hibernálás, takarítás) ugyanazt a szkriptet használja, tehát nem tud széttartani; (−) a kártya state-je egy
DevTools-körrel később dől el, tehát a takarítás egy kicsit lassabb, és egy nem válaszoló oldalnál a régi
viselkedés marad.

## ADR-047 — A keresés ékezet-független, és nem ígér oldalszöveget (2026-11-19)

**Kontextus:** a rács keresője eddig `toLowerCase()`-vel hasonlított, tehát a `kave` **nem** találta meg a
`Kávézó` kártyát — pont azt a szót, amit egy magyar felhasználó gépel. A helyőrző szöveg ezen kívül
„oldalszöveg"-et ígért, miközben semmi nem indexeli a cikkek szövegét (a mag és a shell csak címet, webcímet,
jegyzetet és címkét keres).

**Döntés:**

1. **Ékezet-független egyezés mindkét oldalon.** A lap `foldSearch`-je és a shell `FoldForSearch`-je ugyanazt
   teszi: kisbetűsít, Unicode NFD-re bont és eltávolítja a kombináló jeleket (`ő` → `o`, `ű` → `u`), majd a
   felbontás-álló betűket (`ł`, `ø`, `đ`, `ß`) kézzel képezzi le. Azért mindkét oldalon, mert a kérdés nem
   válaszolhat másképp aszerint, hogy a lap vagy a shell szűrt.
2. **Az üres találat a keresésre utal, nem a semmire.** A rács „Nincs találat erre: »…«" szöveget mutat a
   keresett kifejezéssel, a kártyák száma pedig üres — a korábbi néma üresség azt sugallta, hogy elromlott a lap.
3. **A helyőrző nem ígér oldalszöveget.** „cím, webcím, jegyzet, címke (ékezet nélkül is)" — az oldalszöveg-
   keresés a hibernált oldal szövegének indexelését igényelné, ezért a `19-FOLYTATAS.md` §12 nyitott tétele
   marad, és amíg nincs meg, a felület nem állítja, hogy van.

**Következmény:** (+) a keresés a felhasználó gépeléséhez igazodik, nem a tárolt alakhoz; (+) az üres állapot
megmondja, mit lehet tenni (kevesebb szó, törlés); (−) a fold közelítő: a `ß` → `ss` és a lengyel `ł` → `l`
kézzel karbantartott lista, tehát ritka nyelvekben maradhat kivétel; (−) az oldalszöveg továbbra sem
kereshető — ez tudatos ígéret-visszavonás, nem rejtett hiányosság.

## ADR-046 — A shell naplófájlja a konzol, ami nincs (2026-11-19)

**Kontextus:** az 55. szelet eldobja a konzolablakot, amit a Windows a parancsikonos indításhoz ad — és ezzel
együtt a stderr-t is, tehát a motor üzenetei (GPU-hibák, lap-oldali hibaüzenetek) a semmibe mentek. Egy olyan
gépen, ahol senki nem néz oda, egy hiba így nyom nélkül eltűnik.

**Döntés:** a shell a **meglévő** üzenetkezelőn (`ForwardQtMessages`) keresztül minden sort a
`%APPDATA%\Antikolt Project\Antikolt\logs\shell.log` fájlba is ír, időbélyeggel, és 1 MB-nál **egy generációt**
tart meg (`shell.log.1`). A naplófájl útja **csak a sima indításnál** áll be (üres parancssor): a terminálból
futtatott programnak már van stderr-je, az öntesztek pedig nem írhatnak a felhasználó naplójába. A határérték
környezeti kapcsolóval állítható (`ANTIKOLT_SHELL_LOG_LIMIT`), így a `--logtest` néhány kilobájttal rotál.

**Miért nem új naplózási réteg:** a hibák nagy része a motortól jön, ami a saját stderr-jét a shell `qInfo`-jára
irányítja; egy új, párhuzamos naplózó csak azt biztosítaná, hogy a két helyen más legyen a szöveg. Egy helyen
formázunk, és ugyanaz a string megy a stderr-re és a fájlba.

**Következmény:** (+) a parancsikonos indítás is diagnosztizálható, és a rotáció miatt a lemez nem telik meg;
(+) a napló ugyanaz, mint amit egy terminálban látnánk, tehát a hibajegybe másolható; (−) a napló tartalmazhat
oldalcímeket és hibaüzeneteket (de **jelszót, TOTP-t nem** — az I5 továbbra is áll: a titok a fővilágba és
naplóba nem kerül); (−) a fájl írása a hibakezelési úton is fut, ezért szándékosan buta (open/append/close), és
egy tele lemez sem fordíthatja hibába a naplózást.

## ADR-045 — Az oldal-sötétítésnek több fokozata van, a témaváltó pedig ikon (2026-11-18)

**Kontextus:** a felhasználó az ADR-044 után azt kérte, hogy **az oldalakhoz is legyen többféle sötét mód**, ne
csak a böngészőhöz, és hogy a témaváltó gombok **ikon + tooltip** formában jelenjenek meg (a „light grey dark
amoled" felirat négy szó volt a vezérlősáv legszélesebb, legkevesebb információt adó részén).

**Döntés:**

1. **Négy oldal-sötétítési fokozat** (`set_forced_dark` `mode` mezője, a korábbi `enabled` logikai mező
   visszafelé kompatibilis marad): `off`, `invert` (közel fekete), `dim` (sötét szürke, `contrast(0.72)`),
   `warm` (meleg sötét szürke, `contrast(0.78) sepia(0.35)`). Mindegyik ugyanaz a trükk más erősséggel: a lap
   invertálása és a képek visszainvertálása, egy **nevesített** stíluselemmel, amit a módváltás *lecserél*,
   a kikapcsolás *eltávolít* — újratöltés nélkül.
2. **A témaváltó ikonok**: a négy felületi téma egy-egy generált ikon (a téma háttere + szövegszíne, 20 px),
   a szó a **tooltipben** és a képernyőolvasó-névben; az oldal-sötétítés egy holdsarló-ikon, ami a menüből
   választható, és a **ikonja mutatja az éppen érvényes fokozatot**.
3. **A beállítás megmarad** (`QSettings`, `ui/page_dark_mode`), és a régi `ui/forced_dark` logikai kulcsot az
   első indítás átváltja `invert`-re, majd törli.

**Miért nem a Chromium saját auto-darkja:** lásd ADR-044 (processz-szintű, verziófüggő). A `contrast()` és a
`sepia()` ugyanazt a lap-szintű mechanizmust használja, tehát nincs új mechanizmus, csak más fokozat.

**Következmény:** (+) a felhasználó választhat a teljes sötét és a hosszú olvasáshoz kíméletesebb fokozatok
között; (+) a vezérlősáv keskenyebb lett, és a szavak a tooltipben vannak, ahol keresik őket; (−) az inverzió
közelítő: a fényképek visszainvertálva is eltérhetnek, az árnyékok és színátmenetek pedig fokozatonként
másképp viselkednek — ezért marad a „Kikapcsolva" alapállapot.

## ADR-044 — A sötét oldalak a felhasználó kapcsolója, és a felület nem hazudik a témáról (2026-11-18)

**Kontextus:** a felhasználó azt jelentette, hogy „a sötét módok nem működnek", egy amoled témájú ablakban
(amit a képe bizonyít: a vezérlősáv és a státuszsor `#0A0A0A`), mert a **Wikipedia cikke fehér maradt**. A
kettő nem ugyanaz a dolog: az ADR-037 szándékosan nem festi át a weboldalakat, csak *megmondja* nekik
(`prefers-color-scheme`), a cikknek pedig nincs saját sötét témája. A felület tehát a saját témáit jól
alkalmazta, a **lapot** viszont nem sötétítette — és a felhasználó ezt látta „nem működő sötét módnak".

**Döntés:** három, egymástól független dolog, mindegyik mérve:

1. **A felületi témák** (`light`/`grey`/`dark`/`amoled`) a **termék** felületét festik: az ablakot, a
   vezérlősávot és a **rácsot** is. A rács saját `data-theme` attribútuma eddig külön kapcsoló volt (sőt egy
   klikkel le is maradt), mostantól a vezérlősáv döntése hajtja meg, minden témaváltásnál és minden
   rács-betöltésnél. Ezt a `--themetest` **pixelenként** méri: a vezérlősáv átlagos fényessége 241 / 60 / 42 /
   15 a négy témában, a rács képe 239 → 68 (light → amoled).
2. **Az „Oldal-téma: auto/dark/light"** marad az ADR-037 szerinti jelentés: a böngésző megmondja a lapnak,
   mit szeretne a rendszer; a lap dönt. Ez nem sötétít.
3. **Új: „🌙 Sötét oldalak" kapcsoló** (alapból **ki**, megjegyzett): a motor a lapba egy **nevesített**
   stíluselemet szúr (`set_forced_dark`), ami invertálja a dokumentumot és visszainvertálja a képeket — és
   ugyanazt az elemet eltávolítva állítja vissza, újratöltés nélkül. Mérve: egy fehér próbaoldal fényessége
   251 → 13.

**Miért nem Chromium saját „auto dark" feature-je:** az processz-szintű kapcsoló (`WebContentsForceDark`),
ami minden lapot érint, és a viselkedése a Chromium verziójával változik. A stíluselem lap-szintű, pontosan
visszavonható, és a felhasználó döntése — ez illik az ADR-037 szelleméhez (a lap marad a lap).

**Következmény:** (+) a felhasználó egy kattintással sötét oldalakat kap, és ezt a következő indítás is tudja;
(+) a rács és a vezérlősáv soha nem mond ellent egymásnak; (−) az inverzió nem tökéletes: a képek/videók
színvilága közelítőleg marad, de a színátmenetes grafikák és az árnyékok eltérhetnek az eredeti dizájntól —
ezért **alapból ki**, és nem az „Oldal-téma" gomb része, hanem külön kapcsoló; (−) a beállítás a felhasználói
profilban (`QSettings`, `ui/forced_dark`) van, tehát az első indításnál még nincs bekapcsolva.

## ADR-043 — A tálcaikon az indító és a futásjelző: egy felhasználó, egy Antikolt (2026-11-18)

**Kontextus:** a programot a hordozható mappa és a parancsikon indítja, de a bezárás gomb **kilépett**, és
nem volt semmilyen jelzés, hogy fut-e a böngésző; a parancsikon másodszori megnyitása pedig egy *második*
Antikoltot indított — második motorral, második profillal, második ~150 MB-tal ugyanarra a kártyatárra. A
felhasználói kérés szó szerint ez volt: „tálcára az indítóikon, ami egyben a futás ikonja is".

**Döntés:** a tálcaikon a parancsikon ikonjával **azonos** kép, és az él, amíg a folyamat él:

1. **Egy felhasználó, egy Antikolt.** Indulás előtt a shell egy nevesített helyi socketet
   (`antikolt-shell-<felhasználó>`) foglal le (`QLocalServer`). Ha már valaki válaszol rá, ez az indítás
   elkéri a futó ablaktól, hogy jöjjön elő, és **ablak nélkül, 0-val kilép** — nem indít motort, profilt,
   folyamatot. A socketnév felhasználónként különbözik, tehát két fiók a gépén két Antikoltot futtathat.
2. **Az ikon a tálcán** (`QSystemTrayIcon`) a program ikonjával és a „Antikolt — fut" tippel: ez a
   futásjelző. Kattintásra (vagy duplán) ki-be váltja az ablakot; a menüje „Megnyitás" és „Kilépés".
3. **A bezárás gomb nem kilépés**: a tálcára teszi az ablakot, és egyszer elmagyarázza, hol van (balloon).
   Az egyetlen igazi kilépés a tálcamenü „Kilépés" pontja (vagy a folyamat leállítása).
4. **Tálca nélkül** (tálca nélküli munkamenet, vagy `ANTIKOLT_NO_TRAY`) minden a régi: a bezárás kilép.
   Minden önteszt ebben a módban fut, csak a `--traytest` kapcsolja be a tálcát.

**Miért nem egy háttérben maradó processz indítja a böngészőt:** az a memória ígéretével ellentétes lenne
(a „launcher" egy második, mindig futó program), és a Windows tálcájának nincs olyan eseménye, ami egy nem
futó programnak szólna. A parancsikon + őr kombináció ugyanazt a felhasználói élményt adja, folyamat
nélkül.

**Következmény:** (+) a bezárás nem viszi el a böngészőt, a felhasználó ugyanoda tér vissza; (+) a
parancsikon nem indít véletlenül második böngészőt, tehát a kártyatárat nem írja két folyamat; (+) az ikon
és a parancsikon ugyanaz a kép, tehát a tálcán látszik, hogy fut; (−) a tálcára tett ablak **nem ad vissza
memóriát**: a motor és a rács él (~150 MB, mért érték), amíg a felhasználó „Kilépés"-t nem nyom — a nyitott
kártya viszont a szokásos 10 perces szabály szerint magától hibernál, tehát a tálcán várakozó böngésző a
kártya memóriáját is elengedi; (−) egyetlen socketnév véd egy gépen belül, egy időben (két indulás
versenye ms-es ablakban elméletileg mindkettő primary lehet — a gyakorlatban a Windows pipe-foglalás ezt
megfogja).

## ADR-042 — A motorház két felületet tart, és minden parancs megnevezi a magáét (2026-11-17)

**Kontextus:** az ADR-041 munkás-felülete a motorház eddigi feltevésébe ütközött: `RunEngineHostServer`
egyetlen `HostState`-et tartott, és minden parancsot arra hajtott végre — a `surface` mezőt a huzal
ellenőrizte, a motor viszont figyelmen kívül hagyta. Egy második felület ezért nem fért be, és a hibás
(nem létező) felületre címzett parancs csendben a rácsot szolgálta ki.

**Döntés:** a motorház **legfeljebb két** felületet tart (rács + munkás), felületenként külön állapottal,
és minden felületre vonatkozó parancs a **megnevezett** felületre hat:

1. `create_surface` adja a felület azonosítóját; az állapotok a felület-azonosító kulcsán élnek, a
   `bridge_reply` a kérelmet a *tulajdonos* felület állapotában keresi.
2. Ismeretlen felületre címzett parancs `surface_not_found` („no live surface") — nem esik vissza a rácsra.
3. A harmadik felületet a motor `engine_busy`-vel utasítja el („at most two surfaces at a time"), tehát a
   korlát a motorban is kényszerítve van, nem csak a shell szándékában.
4. A `hidden` jelző felületenként adható (`create_surface { …, hidden }`), a `--hidden` folyamatkapcsolóval
   és nélküle is; a munkás ezzel nem jelenik meg a felhasználó előtt.
5. A `WM_CLOSE` ablakonként csak a saját felületét zárja be (ablak → felület regisztráció).

**Mérés (memória, a „4 MB" helyett):** a munkás élettartamára a motorházi folyamatfa
**9 folyamat / 167 MB** volt a **8 folyamat / 143 MB** alapállapothoz képest ugyanabban a futásban
(`--previewworkertest`, 150 ms-os mintavétel) — azaz a második felület **egy külön renderelő folyamat,
~24–25 MB**. Az ADR-041 5. pontjának 4 MB-os ígérete ezért téves volt; a valódi korlát a rövid élettartam
(≤ 8 s), az egyidejűleg egy munkás és a rácson állás.

**Következmény:** (+) a munkás felülete ugyanazon a motoron fut, tehát nincs második motor/Cache; (+)
minden teszt, amely eddig egy „csendben a rácsra eső" parancsra épült, most **őszintén elbukik** — a
szelet során tíz ilyen teszt-hiba került elő (dangling `id` referenciák deferred lambdákban,
elavult felület-azonosítók a hibernált rács után), és mindegyik javítva lett; (−) a motor állapotkezelése
egy szinttel bonyolultabb (felület-regisztráció, felületenkénti felszabadítás), és a shell-nek garantálnia
kell, hogy kártyanyitáskor a munkás *előbb* szűnjön meg, mint ahogy az új felület parancsa megy.

## ADR-041 — A háttér-előnézet rövid életű második felületet kap (2026-11-17)

**Kontextus:** a 28. szelet óta a kártya-előnézet csak akkor készül, amikor a felhasználó megnyitja a
kártyát. A rács így addig üres kereteket mutat, amíg minden kártyát meg nem nyitottak egyszer. A
`nextPreviewCard` már eldönti, *melyik* kártya kapjon képet, de a **végrehajtás** eddig falba ütközött az
**I1** invarianensen: „egyszerre egy élő felület" — a rács is felület.

**Döntés:** a háttér-előnézet **ideiglenes munkás-felületet** kap, szigorú korlátokkal:

1. **Csak akkor indul**, ha nincs élő kártya (a felhasználó a rácson van), nincs más munkás, és a napi keret
   engedi (`previewBudgetAllows`).
2. **Egyidejűleg egy** munkás van, saját partícióval (`persist:preview`), hogy ne keveredjen egy kártya
   sütijeivel.
3. **8 másodperces plafon** a betöltésre és a felvételre; utána a felület megszűnik, a kártya kimarad.
4. **Azonnal megszűnik**, ha a felhasználó kártyát nyit (a munkás nem versenyezhet a felhasználóval).
5. **A plusz memória korlátja a rövid élettartam**, nem egy szám: a mérés szerint a munkás-felület egy
   **külön renderelő folyamat** (~25 MB a motorházi folyamatfán; lásd ADR-042). Az eredeti „legfeljebb
   4 MB" ígéret mérés alapján **téves volt**, és helyette a korlát ez: egyidejűleg egy munkás, csak a
   rácson állva, legfeljebb 8 másodpercig.

**Miért nem alternatíva a rács felületének újrahasználata:** az megszüntetné a rácsot (a felhasználó
visszatérne egy üres képernyőre), és megsértené a „haza" jelentését.

**Következmény:** (+) a rács előnézetei maguktól feltöltődnek; (+) a felhasználó kártyája soha nem lassul
miatta (5. pont: azonnali megszakítás); (−) az I1 **szigorúan** él, de egy dokumentált, rövid és egyidejűleg
egy kivétellel — ezt a kódban is kényszeríteni kell (állapotgép: `idle → loading → capturing → idle`), és
önteszttel bizonyítani.

**Állapot:** **megvalósítva** (2026-11-17). A munkás a `MainWindow`-ban él (`previews.h` őrei), a motorház
két felületet tart (ADR-042), és a `--previewworkertest` méri: megszakítás a plafonon, sikeres felvétel,
megszakítás kártyanyitáskor, a felület eltűnése és a keret. A memória-ígéret helyesbítése az ADR-042-ben.

## ADR-040 — Aláírás: Store + SignPath, két csatorna (2026-11-17)

**Kontextus:** a kiadás ma egy hordozható mappa + asztali parancsikon (`scripts/dist.ps1`), aláírás nélkül.
Az aláírás nélküli exére a SmartScreen figyelmeztet, ami egy böngészőnél elfogadhatatlan a végfelhasználónak.
Az ingyenes utak 2026-ban: **Microsoft Store MSIX** (a Microsoft újraaláírja a csomagot, a fejlesztői fiók
egyéneknek ingyenes) és **SignPath Foundation** (ingyenes OV-szintű aláírás OSS-projekteknek, a kulcs az ő
HSM-jükben). Az Azure Artifact Signing (korábban Trusted Signing, ~10 USD/hó) egyéni fejlesztőknek csak
USA/Kanadában érhető el, EV-tanúsítvány pedig 2024 óta nem kerüli meg a SmartScreen-t — a 400+ USD/éves
prémium ezért nem indokolt.

**Döntés:** **két csatorna, nem választás.**

1. **Saját letöltés (ez a termék ma):** a `dist\Antikolt` mappa exéit **SignPath** írja alá — ingyenes OSS
   program, a feltétel nyilvános repó és CI-build. A jelentkezés akkor esedékes, amikor van reprodukálható
   kiadás; addig self-signed tanúsítvány csak a saját gépen érvényes tesztelésre való (a Microsoft
   összehasonlítása szerint ez másoknál **blokkolja** a telepítést).
2. **Store-ból telepítők:** **MSIX + Microsoft Store**, ahol az aláírást a Microsoft végzi (ingyenes, nincs
   tanúsítvány). Ez a csomag a Store-ból települ, nem a hordozható mappánkat írja alá.

**Miért nem elég az egyik:** a Store nem írja alá a saját oldalról letöltött exét (a hordozható mappát és a
parancsikont), a SignPath pedig nem tesz semmit a Store-ból telepített MSIX-szel. A kettő **különböző
artefaktumot** fed, ezért mindkettő kell a teljes lefedettséghez.

**Következmény:** (+) a saját kiadás is aláírt lesz, tanúsítvány-vásárlás nélkül; (+) a Store-út ingyenes és
figyelmeztetésmentes a Store-felhasználóknak; (+) a tanúsítvány soha nem kerül a fejlesztő gépére (nincs
kiszivárgó kulcs); (−) a SignPath-aláírás az ő folyamatukon megy át (CI-integráció + elbírálás), a Store-út
pedig MSIX-csomagolást és Store-szabályokat hoz; (−) Linux DEB-aláírás az ingyenes tierben **nem biztos**
(RPM igen, DEB csak „Semantic Code Signing" jelöléssel) — ezt a SignPath-nál meg kell erősíteni.

**Részletek és lépések:** `20-ALASAS.md`. Az eszköz a repóban: `scripts/sign.ps1` (PFX / thumbprint /
self-signed mód, és **tanúsítvány nélkül elutasít**).

## ADR-039 — A munkamenet-szabályokat a felhasználó állítja, a mag ellenőrzi (2026-11-17)

**Kontextus.** A 24 órás politika eddig csak *megjeleníthető* volt: a panel mutatta a szabályokat, de
módosítani nem lehetett őket (a ledger a `session_list` válaszában küldte a `rules`-t, írás nem volt
rá). Egy „beállíthatatlan" biztonsági ígéret pedig nem ígéret.

**Döntés.** Új huzal-op: `session_rules { rules }` → `{ rules }`, a panelen óraben megadott lejárat
(1–720 óra), „app-zárnál törlés" jelölő és egy **Mentés** gomb. A **ellenőrzés a magban** van
(`SessionStore::set_rules`): a TTL-nek valódi visszatartási ablaknak kell lennie (1 óra – 30 nap), mert a
nulla TTL minden munkamenetet azonnal elseperne, és a 30 napon túli érték elírás, nem politika. Az
elutasítás saját hiba-változat (`SessionError::Rules`), hogy a válasz azt mondja, mi a baj — nem egy
sor-szintű hibát kölcsönöz.

**Következmény.** (+) a felhasználó tényleg állíthatja a politikát, és a mag ugyanazt a `plan_cleanup`-ot
hajtja végre, mint eddig; (+) a szabályok a `sessions.json`-ban maradnak, tehát újraindítás után is élnek;
(−) a TTL-t órában adja meg a felület, ezért a nem egész órás értékek nem érhetők el (szándékos
egyszerűsítés); (−) a pane­lenkénti (kártyánkénti) felülírás továbbra sem létezik, csak a konténer-szintű.
## ADR-038 — Scroll és zoom: a dokumentumon, illetve újratördeléssel (2026-11-17)

**Kontextus.** A huzal-szerződés régóta hirdette a `scroll` és a `set_zoom` opokat, a motor `not_supported`-ot
válaszolt. Mindkettőnek legalább két megvalósítása van, és a rossz választás látszólag működik, valójában
mást csinál.

**Döntés.** A `scroll` **`window.scrollBy`**: determinisztikus, nem kell hozzá pointer-pozíció, és pontosan az,
amire a scroll-pozíció visszaállításához szükség van. Nem görget be belső overflow-konténert — az gesztus-alapú
görgetés (`Input.synthesizeScrollGesture`) lenne, ami egy *másik*, későbbi funkció. A `set_zoom`
**`Emulation.setDeviceMetricsOverride`**: a CSS-viewport zsugorodik, a szöveg újratördelődik (mint egy
böngészőben), nem a kép nagyítódik. A méretarány **szorzódik** a képernyő saját DPR-jével (amit előbb
DevTools-szal mérünk), ezért 150 % egy 125 %-os kijelzőn 1,875 DPR-t ad, nem 1,5-et — egy abszolút tényező
csendben kicsinyítene HiDPI-n. A 100 % törli a felülírást.

**Következmény.** (+) a görgetés és a nagyítás mérhető a lap saját `scrollY`/`devicePixelRatio` értékén; (+)
a HiDPI helyes; (−) a görgetés a dokumentumot mozgatja, nem a fókuszban lévő belső konténert; (−) a nagyítás
felülírja a lap méretét, tehát a `resize` eseményeket a lap megkapja (ez helyes, de a shellnek nem szabad
`set_bounds`-szal visszaírnia).
## ADR-037 — Az oldal-témát a lap dönti el, a böngésző csak jelenti (`set_media`) (2026-11-17)

**Kontextus.** A sötét oldal-téma (F7) kétféleképp képzelhető el: a böngésző **rákényszeríti** a színeket
minden oldalra (invertálás, `forced-colors`), vagy **jelzi** a lapnak, hogy a felhasználó sötétet kér, és a
lap saját CSS-e dönt. Az első erőszakos: tönkreteszi a képeket, a márkaszíneket és a kontrasztot, és a
felhasználó nem tudja visszavonni oldalanként. A második az, amit a platform is kínál
(`prefers-color-scheme`).

**Döntés.** A shell a motor `set_media` opján keresztül `Emulation.setEmulatedMedia`-val állítja a
`prefers-color-scheme`-et (`auto` = nincs felülírás, `dark`, `light`), és **emlékszik** a választásra: minden
felület-becsatolásnál és betöltésnél újra alkalmazza, mert az emuláció ahhoz a laphoz tartozik, amelyik
éppen látszik. A lap, amely nem támogatja a lekérdezést, **változatlan marad** — ez szándékos: a
rákényszerített színek külön, kockázatosabb funkció lennének (és nem ez az op).

**Következmény.** (+) a felhasználó egy kattintással sötét oldalt kap ott, ahol az oldal támogatja, és
visszakapcsolhatja; (+) a lap saját márkája és képei épek maradnak; (+) a beállítás felület- és
navigáció-független (újra alkalmazzuk); (−) azok az oldalak, amelyek nem ismerik a lekérdezést, nem
változnak — a felület ezt nem ígéri; (−) az oldalankénti kivétel („ez az oldal maradjon világos”) még nincs.

## ADR-036 — A memória-számot a motor méri, a lap a híd kérés-csatornáján kapja meg (2026-11-17)

**Kontextus.** A termék ígérete a „memóriatakarékos" böngésző, de a felület semmit nem mutatott a
fogyasztásról: a lap `system.memory` hívása a mockból nullát kapott. Három rossz út: a shell tippeljen
(nem mérés), a lap számoljon (nem láthatja a motor folyamatát, és a lap adata nem megbízható), vagy a
státuszsor a semmit mutassa (ez volt a helyzet).

**Döntés.** A **motor** méri, ami mérhető: `metrics` op, ami a `Runtime.evaluate`-vel lekéri a lap saját
nézetszámát (`document.getElementsByTagName('*').length`) és a `performance.memory` JS-halmot, a
`GetProcessMemoryInfo`-val pedig a **saját munkakészletét**, és `{dom_nodes, js_heap_bytes,
engine_working_set_bytes}`-t válaszol. A shell a lap `system.memory` **kérésére** ezt továbbítja
`{usedBytes, budgetBytes}` alakban; a `budgetBytes` 0, amíg a profilok nem hordoznak memória-büdzsét (F6),
és a lap ilyenkor csak a felhasznált értéket írja ki — nem talál ki büdzsét.

**Következmény.** (+) a felület valódi, mért számot mutat, és a lap a kétirányú hídon keresztül kapja
(ugyanaz a csatorna, mint a kártya-kérdéseké); (+) a mérés helye a motor, mert csak ő látja a dokumentumot
és a saját folyamatát; (−) a JS-halom értéke **kvantált**: a Chromium adatvédelmi okból kerekíti
(`10 000 000` pontosan), tehát indikátor, nem könyvelési adat — ezt a napló kimondja, a felület pedig
kerekítve, MB-ban mutatja; (−) a büdzsé és a kártyánkénti bontás (F6) továbbra is nyitott.

## ADR-035 — A kártya-előnézet a motorból jön, `antikolt://blob/` sémán, nem a hálóról (2026-11-17)

**Kontextus.** A rácsnak előnézet kell, és a lap már hivatkozik rá (`antikolt://blob/<ref>`). Két rossz
út: a lap a hálóról töltse (az oldal lassú lehet, a kép elavul, a kártya adatai kiszivároghatnak a
távoli szerverhez), vagy a kép az adatbázisban utazzon (a kártyalista mérete és a protokoll költsége
robban). A helyes válasz: az előnézet **helyi fájl**, amit a *motor* szolgál ki a profilból, a lap pedig
egy stabil névvel hivatkozik rá.

**Döntés.** A motor regisztrálja az `antikolt` sémát (`STANDARD | SECURE | CORS_ENABLED |
FETCH_ENABLED`), és a `blob/<név>` kérést a `<user data>/previews/<név>` fájlból válaszolja meg
(`CefStreamResourceHandler`, MIME a kiterjesztésből). A **nevet a könyvtáron belül oldja fel**: üres név,
`..` vagy elválasztó → elutasítás; nem létező fájl → 404. A fájlt a **shell termeli** (a `capture` op
kimenete), tehát az előnézet nem letöltés, hanem előállítás.

**Következmény.** (+) a rács előnézete helyi, gyors és a lap felé is hivatkozható; (+) egy kártya adata
nem tud tetszőleges fájlt kiolvasni a böngészőn át (a névfeloldás a könyvtárban marad); (+) a
kártyalista marad kicsi (egy név, nem kép); (−) az előnézet **előállítása** még nincs kész: egy kártyát
háttérben meg kell nyitni, `capture`-rel képet csinálni és a profilba írni (ugyanaz az I1-korlát, mint a
takarításnál), és a kártya-rekordnak szüksége lesz egy additív `preview_ref` mezőre.

## ADR-034 — A lap kérdezhet: a híd kétirányú, és ez protokoll v2 (2026-11-17)

**Kontextus.** Az ADR-032 egyirányú hidat rögzített, mert a lap nem tudott kérdezni: a shell tolt, a
lap csak jelezni tudott (`open_card`, `freeze_card`). Ez elég a rács feltöltéséhez, de nem elég ahhoz,
hogy a lap a mag adataiból **választ** kapjon — keresés, pin, jegyzet —, és a „lap a saját másolatát
tartja” állapot hosszú távon két igazságot jelent.

**Döntés.** A lap `window.antikoltNative.request(method, args)` hívása **perzisztens `cefQuery`**: a
motor a callbacket a kezében tartja, a kérést `bridge_call` eseményként továbbadja egy
**kérés-számmal**, a shell `bridge_reply`-vel válaszol (`json` vagy `error`), és ezzel a lap promise-a
lezárul. Az egyirányú hívások (`open_card`, `freeze_card`) változatlanok — a motor azonnal `{}`-t
válaszol. Ez **protokoll v2**: új `Message::BridgeReply` variáns, `PROTOCOL_VERSION` 1 → 2, és a
`bridge_reply` op a `proto/README.md`-ben. A verziót **mindkét oldal ellenőrzi**, tehát egy v1 shell és
egy v2 motor nem „félműködik”, hanem elutasítja egymást.

**Következmény.** (+) a lap a mag adataiból kap választ, nem a saját másolatából; (+) minden kérés
pontosan egy választ kap — ismeretlen metódusra is elutasítás jön, nem csend (a lógó promise a legdrágább
hiba egy lapon); (+) a híd továbbra is csak azokon a felületeken él, amiket a shell engedélyezett, és
csak a main worldben; (−) a perzisztens query-k élettartamát a motornak kell kezelnie (a lap eltűnésekor
a callbackek eldobódnak), és a verzió-lépés miatt a shell és a motor együtt szállítható; (−) a lap által
kérdezhető metódusok **szerződése** mostantól a `proto/README.md`, nem a `home/app.js`.

## ADR-033 — A munkamenet-politikát a mag értékeli ki, a shell csak végrehajt (2026-11-17)

**Kontextus.** A 24 órás politika (`keep`/`sweep`/`wipe`) eldönti, hogy egy origin tárhelye mikor
törlődik, és a `wipe` **kijelentkeztet**. Ha ezt a shell számolná, akkor (a) a döntés egy ablak
élettartamához kötődne, (b) két helyen élne ugyanaz a szabály, és (c) a panel és a takarítás
eltérhetne egymástól. Ráadásul a `plan_cleanup` a lemezen tárolt `last_seen_ms`-re és a
konténer-szabályokra épül, amik a mag adatai.

**Döntés.** A politika **kiértékelése a magban** történik: `antikolt-core::session` a szabály
(`SessionRules`, `plan_cleanup`, `CleanupPlan`), a mag-szolgáltatás `session_due` op-ja adja vissza a
**végrehajtandó sorokat a hozzájuk tartozó tervvel együtt**. A shell csak végrehajt (motortól
`clear_origin_data`), és megjelenít. A ledger `sessions.json`-ban él, a kártyatártól külön fájlban —
egy hibás munkamenet-sor ne kerülhesse a kártyalistába.

**Következmény.** (+) egy helyen él a szabály, a panel és a takarítás ugyanazt a kiértékelést látja;
(+) a terv és a döntés együtt utazik, így nem lehet rossz tárhely-készletet törölni; (−) a shell
minden takarítási körben oda-vissza beszél a maggal (ez szándékos: a döntés nem a shellben születik);
(−) a `clear_origin_data` motor-op még `not_supported`, tehát a terv végrehajtása a következő szelet.

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
| ADR-033 | A munkamenet-politikát a mag értékeli ki (`session_due`), a shell csak végrehajt és megjelenít | Elfogadva |
| ADR-034 | A lap kérdezhet: perzisztens `cefQuery` + `bridge_reply` = protokoll v2 (az ADR-032 továbbra is érvényes az egyirányú hívásokra) | Elfogadva |
| ADR-035 | A kártya-előnézetet a motor szolgálja ki a profilból (`antikolt://blob/`), a nevet a könyvtáron belül oldva fel | Elfogadva |
| ADR-036 | A memóriát a motor méri (`metrics`), a lap a kérés-csatornán kapja; a büdzsé 0, amíg a profilok nem hordozzák | Elfogadva |
| ADR-037 | Az oldal-témát a lap dönti el: `set_media` a `prefers-color-scheme`-et állítja, nem kényszerít színt | Elfogadva |
| ADR-038 | `scroll` = `window.scrollBy`, `set_zoom` = újratördelő device-metrics felülírás, a DPR-rel szorozva | Elfogadva |
| ADR-039 | A munkamenet-szabályokat a felhasználó állítja (`session_rules`), a mag ellenőrzi (TTL 1 óra – 30 nap) | Elfogadva |
| ADR-040 | Aláírás: Store + SignPath, két csatorna (Store = MSIX az MS-től, SignPath = a saját hordozható kiadás) | Elfogadva |
| ADR-041 | A háttér-előnézet rövid életű második felületet kap (8 s, egyidejűleg egy, azonnali megszakítás) | Megvalósítva |
| ADR-042 | A motorház két felületet tart, és minden parancs megnevezi a magáét (felület-azonosító, `hidden`, `engine_busy` a harmadikra) | Elfogadva |
| ADR-043 | A tálcaikon az indító és a futásjelző: egy felhasználó, egy Antikolt (helyi socket őr, bezárás a tálcára, `--traytest`) | Elfogadva |
| ADR-044 | A sötét oldalak a felhasználó kapcsolója (`set_forced_dark`), a vezérlősáv témája pedig a rácsot is hajtja (`--themetest`) | Elfogadva |
| ADR-045 | Az oldal-sötétítésnek négy fokozata van (`off`/`invert`/`dim`/`warm`), a témaváltó ikon + tooltip | Elfogadva |
| ADR-046 | A shell naplófájlja (`logs/shell.log`, 1 MB-nál rotálva) a konzol nélküli indítás stderr-je | Elfogadva |
| ADR-047 | A keresés ékezet-független (lap és shell azonos folddal), az üres találat megnevezi a kifejezést, oldalszöveget nem ígér | Elfogadva |
| ADR-048 | A takarítás sem némítja el a játszó kártyát: ugyanaz a média-kérdés, mint a hibernálásnál | Elfogadva |
| ADR-049 | Az oldalszöveg a fagyasztáskor a kártyarekordba kerül (`card_page_text`, max. 16 000 karakter), és a keresés mindhárom rétegben látja | Elfogadva |
| ADR-050 | A konténer-váltás a kártyán, natív `<select>`-tel; az írás a magon át megy, a lap csak azt mutatja, amit a mag elfogadott | Elfogadva |
| ADR-051 | A konténer-jelmagyarázat egyben a szűrő: chipek színes ponttal és a keresés szerinti darabszámmal, `aria-pressed`-szel | Elfogadva |
| ADR-052 | A konténer-tábla a magban él (`containers.json`, ma SQLite-tábla az ADR-070 szerint), pushban érkezik a lapra, és nem törölhető, amíg kártya használja | Elfogadva |
| ADR-053 | A konténer szerkesztése helyben, dialógus nélkül (`＋`, dupla kattintás, `×`), a mag válasza megjelenik | Elfogadva |
| ADR-054 | A használt konténer törlése `move_to`-val: előbb áthelyezés, aztán törlés; a felület felajánlja, nem kényszerít | Elfogadva |
| ADR-055 | A címkék a kártyarekordban (`tags`), a mag normalizálja (trim, dedup, 12×32), a keresés ékezet nélkül is látja | Elfogadva |
| ADR-056 | A munkaterület-tábla a magban él (`workspaces.json`, ma SQLite-tábla az ADR-070 szerint); a kártya `workspace` mezője tárolt adat, nem a lap indexe | Elfogadva |
| ADR-057 | A munkaterület életciklusa a fül-soron, ugyanazzal a gesztussal, mint a konténereké (`＋`, dupla kattintás, `×`), dialógus nélkül | Elfogadva |
| ADR-058 | A kártya munkaterülete a kártyán váltható, natív `<select>`-tel (nem drag: a virtualizált rácsban törékeny és billentyűzettel használhatatlan) | Elfogadva |
| ADR-059 | A profil egy hordozható JSON-kötegben utazik (`profile_export`/`profile_import`), `schema`-számmal; az ismeretlen séma elutasítás | Elfogadva |
| ADR-060 | A köteg a hídon érkezik, az útvonalat a hívó adja (`profile.export`/`profile.import`); a híd nem nyit dialógust | Elfogadva |
| ADR-061 | A kezdőlap szerkeszthető: `＋` + soros űrlap, `card_delete` a magban, az új kártya az aktív fülre és az első konténerbe kerül | Elfogadva |
| ADR-062 | A kártya átnevezése helyben (dupla kattintás, `Enter`/`Esc`), a teljes rekord `cards.upsert`-jével; a mentés a lap-úton (`antikoltRenameCard`) megy | Elfogadva |
| ADR-063 | A webcím a domain-sávon szerkeszthető (dupla kattintás), a domain a mag válaszából számolódik újra | Elfogadva |
| ADR-064 | SQLite-store (`rusqlite` bundled, séma `user_version=2`), a JSON-store-ok megmaradnak igazságnak; a `db_status` tükröz és visszaellenőriz | Elavult (ADR-069) |
| ADR-065 | A rács listája a SQLite-ból jön (write-through: tükrözés a lista előtt); a JSON marad az író, a hibás eset hangos | Elavult (ADR-069) |
| ADR-066 | A tükör soronként követi az írásokat (`mirror_card`/`mirror_delete`), a lista már csak olvas; több-kártyás op külön tükröz | Elavult (ADR-069) |
| ADR-067 | Az adatbázis a store sorrendjében adja a kártyákat (`ORDER BY last_opened_at_ms DESC, LOWER(title)`), teszt védi | Elfogadva |
| ADR-068 | Kétlépéses törlés a kártyán (`×` → `Biztos?` → törlés 5 s-en belül); mért és zöld (`delete_armed=ok delete_card=ok add_card_check=ok`) | Elfogadva |
| ADR-069 | A kártyatár SQLite-first (`user_version=3`): egy író a `antikolt.sqlite3`, a `cards.json` export; egyszeri átvétel a kapcsoló előtti profilból; a `db_status` a DB ↔ export egyezést méri soronként | Részben elavult (6. pont: ADR-070) |
| ADR-070 | Mind a négy store SQLite-first (`user_version=4`): a négy JSON export; store-onkénti átvétel a `meta` táblában; a ledger teljes sora + egy-soros `session_rules`; a `db_status` mind a négy exportot soronként ellenőrzi | Elfogadva |
| ADR-071 | A konténer izolációs határ (`user_version=5`): partíciónként saját `CefRequestContext` és profilkönyvtár, konténerenként proxy (`system`/`direct`/`fixed`) és engedély-politika (alapból megtagadva); a házirend adatbázis-sor | Elfogadva |
| ADR-072 | Felhasználói beállítások (`UserPrefs`) SQLite-ban (`user_version=6`): `user_prefs` tábla egyetlen sortal (`CHECK (id = 1)`); `max_live_cards`, `idle_hibernate_ms`, `cold_card_ms` defaulttel és validációval; IPC `user_prefs`/`user_prefs_set`; `profile_export`/`profile_import` bekötése | Elfogadva |
| ADR-073 | Bejelentkezési popup: gesztus → valódi ablak az opener partíciójában (közös sütik), 60 s, külön követve (`popup_browser`); gesztus nélkül blokkolva; `popup_requested`/`popup_closed` események; a shell a `popup_open` jelzővel függeszti fel a hibernálást | Elfogadva |
| ADR-074 | Memória-büdzsé (F6): `memory_budget_mb` a `user_prefs`-ben (`user_version=7`, default 512, 64..=4096, `serde(default)`); a shell `refreshUserPrefs`-szel cache-eli, a `system.memory` `budgetBytes`-ként adja; a rács used/budget sávot rajzol; `--metricstest` `page_budget_bytes > 0` | Elfogadva |
| ADR-075 | `<antikolt-card>` önálló csomag: `home/antikolt-card.js` IIFE, a rács és a felhasználói lap ugyanezt tölti; a konténer/munkaterület `<select>` a `setOptions`-ból, a fókusz-állapot `getPendingFocusId()`-dal | Elfogadva |
| ADR-076 | A `video_max_height` megjelenítési sapka: a motor `set_video_cap` opja nevesített stíluselemmel (`video{max-height:Npx}`), a profil hajtja, a `manual` eltávolítja; a dekódolási felbontás `not-measurable` (a CEF-nek nincs laponkénti korlátja), a hatókör top-level `video` | Elfogadva |
| ADR-077 | A preview-név a tartalom hash-e (`previewContentName`, `QCryptographicHash::Sha256` a shellben — a `sha2` crate-függőség megszűnt); dedup a nevezésből; a frissesség-ellenőrzés a tárolt `preview_ref`-re néz | Elfogadva |
| ADR-078 | A shell nyelve futásidőben váltható (`ui/language` QSettings, default `hu`); `i18n.h` `uiText` HU→EN táblázatból; a `setLanguage` helyben átcímkézi a chrome-ot; a státuszüzenetek és a rács a következő szeletek | Elfogadva |
| ADR-079 | A rács nyelve is a shellt követi (`data-language` attribútum + `pushLanguageToGrid` + `window.antikoltSetLanguage`); a rács saját `t()`-je (`home/app.js` EN térkép) újratöltés nélkül újrarenderel; a kártya inline címkéi és a statikus aria-címkék a következő szelet | Elfogadva |
| ADR-080 | Az `Auto` profil a magban oldódik fel (`ProfileKind::Auto`, `params()`=None; `resolve_profile` IPC a `ProfileKind::auto` + `params()` alapján); a shell méri a tápforrást (`GetSystemPowerStatus` + `ANTIKOLT_AC_POWER`) és a nyomást, és minden minta után újra-kérdez; `Fast` az auto-ból nem jön | Elfogadva |
| ADR-081 | A könyvjelző-import a Netscape-HTML-t eszi (az egyetlen Chrome/Firefox/Edge közös export); toleráns regex-parser (`bookmarks.h`), minden könyvjelző kártya, az újra-import nem duplikál; a jelszó-import nincs (nincs cél-boltozat) | Elfogadva |

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
