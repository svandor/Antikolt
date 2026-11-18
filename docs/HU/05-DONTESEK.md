# 05 — Döntési napló (ADR)

Minden visszafordíthatatlan vagy drága döntés itt van rögzítve: **kontextus → döntés → következmény**.
A döntés megváltoztatása új ADR-t igényel (a régi „Elavult" állapotot kap).

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
