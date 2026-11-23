# 19 — Folytatási munkarend (a 135. szelet után)

> Ez a dokumentum **nem terv**, hanem munkarend: az aktuális állapotból indul, és minden tételhez
> megadja, melyik fájlban, mi a teendő, és mi bizonyítja. A terv a `00`–`15`, a döntések a `05`, a
> kronológia a `16`; ez a lap a következő munkamenet első olvasmánya.

## ÁTADÁS — 2026-11-23 (a 135. szelet után, új munkamenethez)

> Ez a fejezet az új munkamenet **első olvasmánya**. A `06b6e54` commitnál állunk; innen indul minden.

### Állapot (mért, nem szándék)

```
git rev-parse --short HEAD  →  06b6e54  (feat(slice-135): name the controls and verify accessibility)
scripts/verify.ps1          →  ALL CHECKS PASSED — 65 ellenőrzés mind „ok"
cargo test                  →  156 zöld: 78 + 53 + 9 + 1 + 15
git status                  →  tiszta munkafa, minden pusholva (origin/main)
```

### Ebben a munkamenetben készült el (123–135. szelet, mind zöld + pusholt)

- **F6 memória-governor befejezve:** `--memorypressuretest` (jel), `--memorypressuree2etest` (e2e), a rács
  mutatja a nyomást (híd-javítás a 126.-ban), `--memoryhibernatetest` (hard = hibernálás), `--memorywarnpausetest`
  (warn = a preview 5 percre szünetel).
- **F7 asztali alapok:** `--printtest` (mentés PDF-be), `--findtest` (Ctrl+F + keresősáv), `--downloadtest`
  (letöltés), `--crashtest` (renderer-összeomlás detektálása **és** automatikus újratöltés), `--pdftest`
  (PDF a PDFium-nézőben), `--accessibilitytest` (nevesített vezérlők + Ctrl+F regisztrálva).

### Ami nyitva van (konkrét következő lépés)

| Tétel | Hol | Mi a teendő | Mi bizonyítja |
|---|---|---|---|
| F6 `video_max_height` | **kész** (ADR-076, 136. szelet) | a profil `video_max_height`-je **megjelenítési sapkaként** alkalmazva (`set_video_cap` op, nevesített stíluselem); a dekódolási felbontás-csökkentés `not-measurable` (a CEF nem ad laponkénti korlátot) | `--videocaptest`: Fast 2160px → Saver 720px (renderelt magasság 720) → Manual eltávolítva |
| OAuth összekötés | `main_window.cpp` (popup, 02-ARCH §8.5) | a mechanizmus kész, a bejelentkezési gesztus-út a harnessben nem kattintható | `not-measurable` gesztus; a kész mechanizmus már mért |
| `sha256` blobtár | `previews.h` / motor | `cargo vendor` egy **hálózatos** gépen (`sha2`), tartalom-szerinti név a fájlnév helyett | új önteszt a tartalom-szerinti címzésre |
| PiP | motor (CEF 158) | a CEF 158 PiP-je **views-alapú** (`CefBrowserViewDelegate`), a shell ablakos (nem views) API-t használ — valószínűleg architektúra-váltás kell, előbb döntés | — |
| HU/EN felület | teljes shell + rács | nagy i18n-szelet (a magyar a hiteles nyelv) | önteszt + doksi |
| Teljes képernyőolvasó-illesztés | külső | képernyőolvasóval (NVDA) manuális | emberi lépés |
| Kiadás | SignPath, Store | emberi lépés (Partner Center, tanúsítvány) | — |

### Kötelező munkarend (a szeletek ettől függnek)

1. **Egy szelet = egy mérhető feature + önteszt.** Minden feature-nek saját `--…test` (vagy tiszta-függvény)
   kell, ami a kapuban fut.
2. **A kapu a bizonyíték:** `scripts/verify.ps1` → `ALL CHECKS PASSED` (most 65 ellenőrzés). Soha ne zöldíts
   „hazugsággal": a számokat a kapu kimenetéből írd a doksiba, ne fejből.
3. **Minden szelet végén:** build + kapu zöld → `16-FEJLESZTESI-NAPLO.md` bejegyzés a **mért** kimenettel →
   commit (`feat/test(slice-NNN): …`) → `git push`. A doksi nyelve a **magyar**.
4. **Dokumentumok:** `16` (kronológia), `21-ALLAPOT` (táblázat), `04-UTEMTERV` (pipa), `19` (ez a lap).

### Sandbox (a felépített minta)

- A `write`/`edit` fájlszerkesztés **nem** kér emelést.
- A `build.ps1`, a `verify.ps1` és a `git push` **kér** `danger-full-access`-et (a cső-capture `EPERM`-je és az
  ssh `CreateFileMapping` miatt) + egy egy mondatos indoklás. Megtagadás után **egyszer** kell újrapróbálni
  ugyanazt a parancsot a szűkebb jogosultsággal — ez a jóváhagyás útja.

### CEF 158 csapdák (ebben a munkamenetben tanultuk, ne fuss újra bele)

- `CefBrowserHost::Find` **4 paramétert** vár (nincs `identifier`); az első `OnFindResult` a találatszámot
  `active=0`-val adja (a kijelölés a következő értesítésben).
- `CefSettings` **nem** ismeri a `download_default_directory`-t; a letöltés helye a `GetFullPath()`-ból tudható meg.
- `CefPdfPrintSettings` mezője `display_header_footer` (nem `header_footer_enabled`), alapból ki.
- A `chrome://crash` a determinisztikus renderer-összeomlás (`STATUS_ACCESS_VIOLATION`) — a `--crashtest` ezt használja.

### Delegálás

A fájlokat író és a kaput futtató munka a **fő szálon** (fizetett/pro útvonalon) maradjon a minőség miatt; a
`subagent` (ingyenes lánc) önálló kutatásra/áttekintésre való.

## 0. Kiinduló állapot (mért)

```
pwsh -File scripts/verify.ps1 -SkipBuild
gridtest ok · lifecycletest ok · opentest ok · embedtest ok · capturetest ok · coretest ok · check-repo ok
ALL SELF TESTS PASSED
```

A termékkör, ami ma működik: indulás → mag kártyalista → rács a valódi, kirenderelt kártyákkal →
kattintás (kitűzött vagy rács kártya) → hídhívás → pillanatkép + felületcsere → élő kártya →
**10 perc tétlenség után magától lemezre megy, és a rács visszajön**.

Amit **kötelező** tiszteletben tartani (a szeletek ettől függnek):
- I1: egyszerre egy élő felület; a rácsot csak a `destroy_surface` nyugtája után szabad újra kérni
  (különben `engine_busy`).
- I2: pillanatkép előbb, felület-megsemmisítés utána.
- A motor `stderr`-je a shellen át látszik (`[shell] info: [engine] …`) — ez az első diagnosztika.
- Minden szelet végén: `pwsh -File scripts/verify.ps1` zöld, és a `16-FEJLESZTESI-NAPLO.md` kap egy
  bejegyzést a tényleges kimenettel (nem szándékkal, méréssel).

## 1. Kitűzött kártya kivétele a hibernálásból

> **Állapot: kész és mérve** (`50ded02` + a 21. szelet) — a `pinned` jelző végigmegy a hídon
> (`app.js` → `open_card` args → `pending_pinned_` → `live_card_pinned_` a felület csatolásakor), az
> `onIdleTimeout` kihagyja a hibernálást, a `surfaceDetached` nullázza. A `--lifecycletest` **mindkét
> ágat** méri: `hibernated=ok` (nem kitűzött) és `pinned_honoured=ok` (kitűzött, 4 óraütés után is él).
> A közben talált hiba (a rács `detach`-ja kitörölte a jelzőt) és a mag vetett kártyáinak kitűzött
> mivolta a 21. szelet bejegyzésében van.

**Miért:** a dokumentált opt-out (`02-ARCHITEKTURA.md` §4.2) — a felhasználó kérheti, hogy egy kártya
maradjon életben. A hibernálás ezt a szelet előtt nem kérdezte.

**Hogyan (a javasolt, legkisebb út):** a `pinned` jelző **a megnyitási kérésben** utazzon, mert a lap
már ismeri a kártyáját — így nem kell a maghoz új API:
1. `home/app.js`, a `withNativeOpen` → `native.openCard({...})`: `pinned: card.pinned,` a `title`
   mellett.
2. `apps/desktop-shell/src/main_window.h`: `bool live_card_pinned_ = false;`.
3. `main_window.cpp` `handleBridgeCall` (`open_card` ág): `live_card_pinned_ = args.value("pinned").toBool();`.
4. `main_window.cpp` `onIdleTimeout`, a média-kérés **előtt**:
   `if (live_card_pinned_) { setStatus("kitűzött kártya — a hibernálás kimarad"); armIdleTimer(); return; }`.
5. `main_window.cpp` `surfaceDetached`: `live_card_pinned_ = false;`.

**Bizonyíték (még hiányzik):** a `--lifecycletest` második fázisa: `eval` a rácson
`window.antikoltNative.openCard({id:'probe', url:'data:text/html,<title>p</title>p', title:'p',
pinned:true})` → a kártya megnyílik, de a rövidített óra lejárta után **nem** záródik be; a teszt
`pinned_honoured=ok`-ot ír, és a két fázis együtt exit 0. Ezt kell megírni a következő lépésben:
`runLifecycleCheck` a `apps/desktop-shell/src/main.cpp`-ben, a meglévő első fázis után.

## 2. 24 órás munkamenet-politika + ledger

> **Állapot: kész és mérve** (22–25. szelet, ADR-033) — a döntés a magban (`plan_cleanup`, `session_due`),
> a ledger `sessions.json`-ban, a panel mutatja, a **„Takarítás most” gomb végre is hajtja** (azonnal a
> nyitott originre, egyébként a kártya következő megnyitásakor), a motor `Storage.clearDataForOrigin`-nal
> töröl. Öntesztek: `--coretest` `session_ledger=ok`, `--sessionpanel` `session_panel=ok`, `--wipetest`
> `wipe=ok`, `--cleanuptest` `cleanup=ok` (`page_wrote=ok`, `storage_after=|`, `origin_rows=0`).
> A **mennyiség** oldala kész: `SessionSize` + `loses_login()` a ledger során (41. szelet, 78 core teszt).\n> A panel **mutatja** is: `7 süti · 5.0 kB` (+ `· belépés` bejelentkezett sornál), 42. szelet.\n> **Ami hátra van:** a takarítás **média-kivétele** (egy játszó kártya
> késleltesse a törlést), és kártyánkénti (origin-szintű) felülírás — a szabály-szerkesztés **kész** (2b).

**Miért:** M3 („saját napi böngésző”) fő tétele: `keep`/`sweep`/`wipe`, látható munkamenet-panellel.

**Hogyan (ami még hátra van):** a `wipe` végrehajtásához a motor `clear_origin_data` opja kell (ma
`not_supported`); a panel a `session_list`/`session_due` válaszára épül, a `LossEstimate`-tel együtt
(`10-MUNKAMENET.md` §5).

**Bizonyíték (kész):** `--coretest` `session_ledger=ok`, `cargo test -p antikolt-core-service` 9 zöld.

## 3. Kérés-irány a laptól (protokoll v2)

> **Állapot: a csatorna kész és mérve** (26. szelet, ADR-034) — a lap `window.antikoltNative.request(...)`
> hívása perzisztens `cefQuery`; a motor `pending_bridge`-ben tartja, `bridge_call` + kérés-számmal adja
> tovább, a shell `bridge_reply`-vel válaszol. `PROTOCOL_VERSION` = 2, mindkét oldal ellenőrzi.
> `--bridgetest`: `asked=ok`, `search_cards=1`, `search_first_title=Wikipedia — kezdőlap`,
> `unknown_method=rejected`, `bridge_request=ok`.
> **A fókusz-visszaadás mérve és működik (62. szelet):** a mentés után a kártya visszakapja a fókuszt, és ezt a
> `--notetest` már *állítja*: `note_focus=01SEED…001:card`, a lánc pedig
> `{"hasFocus":true,"activeTag":"ANTIKOLT-CARD","activeIsHost":true,"shadowActive":"card"}`. Két hiba volt a
> korábbi mérésben: a próba a **rossz elemet** kérdezte (az első kártyát, nem a fókuszálót), és a fókusz
> visszaadása egy 2 másodperces időzítőre támaszkodott, ami lejárhatott a mag küldte újrarenderelés előtt —
> ilyenkor a fókusz a `body`-ra esett (`note_focus=not-host:BODY`). A javítás: az azonosító a következő
> felhasználói eseményig marad (nem időzítőig), és a rács a render **után szinkron** is visszaadja a fókuszt, mert
> egy háttérben lévő ablak animációs képkockát nem feltétlenül kap.
> **A rendered jegyzet mérése is kész:** a próba a state-válasz után, pollozva olvassa a kártya shadow DOM-jából a
> `.card-notes` szövegét (`note_rendered=probe note`).
> **Ami hátra van ebben a fejezetben:** a `test.hang` hook említése a felhasználói doksiban (a
> `07-FOGALMAK.md`-ben már szerepel), és hogy a rács saját keresése szándékosan lokális marad (a lap a teljes
> listát kapja).

## 2b. Munkamenet-szabályok állítása (kész, ADR-039)

> `session_rules` op + panel-szerkesztő (óra, app-zárnál törlés, Mentés). `--rulestest`: `rules_ttl_ms=7200000`,
> `rules_lock_wipe=true`, `rules_refused=refused`, `rules_reread_ttl_ms=7200000`, `session_rules=ok`.
> A szerkesztőmezők a mag állapotát mutatják (`panel_ttl_hours=2`, `panel_lock_wipe=true`), a konténer-alapértelmezés változatlanul megy vissza.
> **Kártyánkénti (origin-szintű) felülírás: kész és mérve** (121. szelet) — a panel legördülője `session_upsert`-tel
> írja a sor `policy`-ját, a mag `SessionRules::resolve`-olja; a `a_rows_own_policy_overrides_the_container_default`
> teszt bizonyítja mindkét irányt (wipe felülírja a sweepet, keep felülírja a wipe-ot).

## 4. Előnézetek

> **Állapot: kiszolgálás és előállítás is kész, mérve** (30–31. szelet, ADR-035). A motor az `antikolt`
> sémát regisztrálja és a `blob/<név>` kérést a `<user data>/previews/<név>` fájlból válaszolja meg (a nevet
> a könyvtáron belül feloldva); a shell a kártya megnyitásakor `capture`-rel képet készít, a profilba
> mozgatja, és a `preview_ref` mezőbe írja a nevét. `--previewtest`: `preview_load=loaded:4x3`,
> `preview_traversal=refused`. `--previewgentest`: `preview_ref=preview-probe.png`,
> `preview_page=loaded:1560x857`, `preview_generation=ok`.
> A napi ≤100 korlát **tartós** (`previews/budget.json`, 38. szelet, `--previewbudget`), a 7 napos frissítés
> kész (`--previewrefresh`), az árva fájlok takarítása kész (`--previewclean`), és a **háttérben** készített
> előnézetek munkása is kész (ADR-041/ADR-042, `--previewworkertest`, lásd §9). A rács a képet **meg is
> jeleníti**: a `home/app.js` a rekord `preview_ref`-jéből `<img src="antikolt://blob/…">`-t csinál (a
> `--gridtest` a `naturalWidth > 0`-val méri), tehát a kör bezárult: munkás → `preview_ref` → csempe.
> **Ami hátra van:** a `sha256`-alapú blob-tár (blokkolt: nincs `sha2` crate), és hogy a queue a *régi*
> képeket is cserélje (a `nextPreviewCard` már sorolja az elavultat, ezt éles használatban kell mérni).

**Miért:** F2 (rács-előnézet) — a `capture` primitív kész, a feldolgozó nincs.

**Hogyan:** `antikolt://blob/<név>` scheme handler a motorban
(`CefRegisterSchemeHandlerFactory`, a profil `previews/` mappájából, név-sanitizálással), majd egy
ütemező: egy kártya egyszerre, 8 s limit, napi 100 frissítés (`02-ARCHITEKTURA.md` §5). A
tartalom-szerinti blob-tár (`sha256`) **blokkolt**: nincs `sha2` crate; addig a fájlnév a kártya id-je.

**Állapot:** az ütemező **kész** (52–53. szelet): `nextPreviewCard` dönt, a munkás-felület fényképez
(ADR-041, ADR-042), a plafon és a keret a `previews.h` őreiben és a motorház két-felületű korlátjában.

**Bizonyíték:** `--gridtest` második fázisa: a kártya `previewRef`-je betöltődik, a kép nem üres
(`naturalWidth > 0`), és a `capture` fájlja a profilban van.

## 4b. Memória-mérés (kész, ADR-036)

> A motor `metrics` opja méri a lap DOM-csomópontjait, JS-halmát és a saját munkakészletét; a shell a lap
> `system.memory` kérésére válaszol vele. `--metricstest`: `dom_nodes=57`, `engine_working_set_bytes=…`,
> `page_used_bytes=…`, `metrics=ok`.
> **Ami hátra van:** a memória-**büdzsé** (F6) és a kártyánkénti bontás; a JS-halom a Chromium miatt
> kvantált, tehát indikátor.

## 4c. Oldal-téma (kész, ADR-037)

> A motor `set_media` opja a `prefers-color-scheme`-et állítja (`auto`/`dark`/`light`), a shell toolbar-gombja
> körbeforgatja és emlékszik rá (minden betöltésnél újra alkalmazza). `--mediatest`: `media_dark=dark`,
> `media_light=light`, `site_theme=ok`.
> **Ami hátra van:** oldalankénti kivétel; rákényszerített sötétítés szándékosan nincs (`scroll`/`set_zoom` kész, 4d).

## 4d. Görgetés és nagyítás (kész, ADR-038)

> A motor `scroll` (`window.scrollBy`) és `set_zoom` (`Emulation.setDeviceMetricsOverride`, a DPR-rel
> szorozva) opja kész. `--scrollzoomtest`: `scroll_y=600`, `dpr_at_150=1.875`, `dpr_at_100=1.25`,
> `scroll_zoom=ok`.
> A shell oldala is kész: toolbar `−` / `🔍 100%` / `+`, emlékezett és minden betöltésnél újra alkalmazott
> nagyítás (`dpr_at_125=1.5625`).
> **A scroll-pozíció kártyánként kész** (39. szelet, `scroll_book.h` + `scroll.json`): `--scrollbook`
> bizonyítja a könyvet (`scroll_a=640`, `scroll_b_after_top=120`, `scroll_reloaded=120`), a bekötést pedig
> `--opentest`/`--scrollzoomtest` fedi.
> **Az end-to-end ellenőrzés kész** (40. szelet, `--scrollrestore`): `first_scroll_y=700`, `restored_scroll_y=700`. Közben három valódi hiba javult: a visszatérő rács hídja, a mentés/navigáció verseny, és a rács nullájának felülírása; a visszaállítás abszolút és késleltetett.`r`n> (b) gesztus-alapú görgetés (belső konténer); (c) a 7 napos küszöb és a 100-as napi keret nem hangolható a felületről.

## 5. Széf és passkey
**Miért:** F4/F4b; a varrat kész (`crates/antikolt-core/src/vault.rs`, 10 teszt, ADR-031).

**Hogyan:** `cargo vendor` egy **hálózatos** gépen (`aes-gcm`, `argon2`, `zeroize`, + a
`rand_core`/`getrandom` lánc), majd `AesGcmSecrets: SecretBox` a rekord-AAD-vel (origin + id, ADR-010)
és a DEK burkolása; utána `credential` tábla (a séma a `0001_init.sql`-ben), majd a passkey
(`WebAuthn.enable` / `addVirtualAuthenticator` / `getCredentials` / `setCredential`) a kész
`DevToolsCall`-on (ADR-029).

**Bizonyíték:** a varrat tesztjei zölden + egy `create`/`get` kör egy teszt-RP-n (F0 S-D spike).

## 5b. Javítva: a `--capturetest` összeomlása kilépéskor

Négy futásból **egy** `0xC0000005` kóddal tért vissza, **miután minden eredményt kiírt** — tehát a lezárás
volt a hibás. Ok: a `destroy_surface` kérést **tűz-és-felejtsd** módon küldte el, majd azonnal lezárta a
csatornát (`client.shutdown()`), így a válasz egy már megszűnő klienshez érkezett.

Javítás: a teszt megvárja a `destroy_surface` válaszát (max. 2 s), és csak utána zárja le a csatornát.
**Mérés: 8/8 futás zöld** (korábban 4-ből 1 összeomlás).


## 6. Ismert csapdák (drágán tanultuk)

| Csapda | Mit okoz | Hol van rögzítve |
|---|---|---|
| `file://` + `type="module"` | a szkript nem fut le, a rács üres | `16`, 16. szelet |
| `shadowRoot.dataset` | minden kártya-render dob, üres elemek | `16`, 16. szelet |
| induláskori híd-döntés | a minta-`open` fut a valódi helyett | `16`, 17. szelet |
| `create_surface` a destroy előtt | `engine_busy`, a rács nem jön vissza | `16`, 19. szelet |
| lap `console` nélkül | a hiba láthatatlan | `16`, 18. szelet |
| `DevToolsReply` dokumentum-élettartam | üres szótár, üres `state.json` | ADR-029 |

## 7. Eszközök

- `scripts/verify.ps1` — egy parancs, minden önteszt + `check-repo` (a munkamenet mérőeszköze).
- `scripts/build.ps1` — eszközlánc-felderítés + build + két `--selftest`.
- `scripts/check-repo.ps1` — a CI kapuja (SPDX, linkek, higiénia, `node --check`, `cargo fmt/clippy/test`).
- A motor naplója a shell `stderr`-jén: `[shell] info: [engine] [engine-host] …` — itt látszik minden
  protokoll-op, DevTools-hívás és lap-`console` üzenet.

## 8. Kiadás és aláírás (ADR-040)

| Elem | Állapot |
|---|---|
| Hordozható build + asztali/Start menü parancsikon | kész (`scripts/dist.ps1`, `-Release`) |
| Saját ikon | kész (`assets/antikolt.icon`, `--selftest` → `icon=ok`) |
| **Tálcaikon + egypéldányos indítás** | kész (ADR-043, `--traytest`; a `dist\Antikolt` frissítve) |
| Aláírás-eszköz | kész (`scripts/sign.ps1`: PFX / thumbprint / self-signed / `-VerifyOnly`) |
| Release workflow | **scaffold** (`.github/workflows/release.yml`, soha nem futott) |
| **MSIX csomag** | kész (`scripts/msix.ps1`, `payload=ok`, 201.5 MB — 2026-11-18-án újratöltve a friss payloadból) |
| Store Identity + helyi telepítés | hátra (Partner Center; tanúsítvány-kötelezettség) |
| SignPath jelentkezés | emberi lépés (publikus repó + elbírálás) |

## 9. Kész: a háttér-előnézet munkása (`--previewworkertest`)

Az ADR-041 kész, a munkás a `MainWindow`-ban él, és a motorház is megtanulta, amit a második felület
igényel (ADR-042). Ami a receptből **többletmunka** volt, és amit a következő szelet már nem talál ki újra:

1. **A motorház eddig egyetlen felületet tartott.** A `surface` mezőt csak a huzal ellenőrizte: minden
   parancs a globális `HostState`-re futott, és egy nem létező felületre címzett parancs csendben a rácsot
   szolgálta ki. Most felületenkénti állapot van (`create_surface` azonosítója a kulcs), a `bridge_reply` a
   kérelem tulajdonosát keresi, ismeretlen felületre `surface_not_found` a válasz, és a motor **kettőnél
   több** felületet `engine_busy`-vel utasít el. A `hidden` jelző felületenként is adható.
2. **A shell oldala** a recept szerint készült: `PreviewWorker` állapotgép a `previews.h` őreivel, saját
   partíció (`persist:preview`), rejtett felület, 8 s-os plafon (`ANTIKOLT_PREVIEW_DEADLINE_MS` a teszthez,
   mint a `ANTIKOLT_HIBERNATE_MS`), megszakítás kártyanyitáskor és a plafonon, a kép átmozgatása a
   `previews/` alá, `previewConsumeBudget`, `preview_ref` a mag rekordjában, és a rács-queue 1,2 s-mal a
   munkás után indítja a következő kártyát.
3. **A szigorúbb motor leleplezte a tesztjeink hibáit.** Kilenc önteszt bukott meg *azonnal*: elavult
   felület-azonosítóra címzett `eval` (a hibernált rács után), és deferred lambdákba másolt `id`
   paraméterek, amelyek a létrehozó closure halála után szemétre mutattak. Mindegyik valódi hiba volt,
   amit eddig a „mindent kiszolgáló" motor eltakart; a javítás után a teljes kapu zöld.
4. **A mérés, amit a szemét helyett kell tudni:** a rejtett felület első `Page.captureScreenshot`-ja ezen a
   gépen (GPU nélküli, szoftveres kompozitálás) néha **20 s-nál tovább** várakozik egy képkockára. Ez a
   termékben nem hiba: a 8 s-os plafon lezárja a munkást, a kártya kimarad, és a queue a következő körben
   újrapróbálja. A teszt ezért ugyanígy tesz (legfeljebb három menet), és kiírja, hányra volt szükség
   (`worker_attempts`); a szerződés a „előbb-utóbb feltöltődik", nem az „elsőre sikerül".

**Bizonyíték:** `previewworkertest ok`, benne `deadline_fired=ok`, `worker_attempts=1`, `worker_pixels=1579x942
colours=13`, `worker_ref=preview-worker-probe.png`, `worker_budget=1`, `abort_state=idle`,
`abort_surface_gone=no live surface`, `user_card_loaded=ok` — a teljes kapu (`scripts/verify.ps1`) zölden.

**Következő szelet jelöltjei** (a `04-UTEMTERV` nyitott tételei közül): a memória-büdzsé (F6) és a
kártyánkénti bontás; a `vault` (F4) `cargo vendor`-ral egy hálózatos gépen (ADR-031 varrata kész); a
konténerek és a kártyánkénti munkamenet-felülírás; a média-kivétel a takarításból; a virtualizált,
500 kártyás rács. Kiadás oldalon egyetlen **emberi lépés** van: a SignPath-jelentkezés.

## 10. Kész: a tálcaikon, ami egyben a futásjelző (`--traytest`, ADR-043)

A felhasználói kérés: „tálcára az indítóikon, ami egyben a futás ikonja is". Két darab kellett hozzá, és
mindkettő mérve van:

1. **Egypéldányos őr** (`instance_guard.h/.cpp`): a shell indulás előtt lefoglal egy nevesített helyi
   socketet (`antikolt-shell-<felhasználó>`). A második indítás **előbb csatlakozik**, és ha válaszol a futó
   példány, elkéri tőle az ablakot, majd ablak és motor nélkül, 0-val kilép. A sorrend fontos: Windowson két
   folyamat is létrehozhat pipe-példányt ugyanarra a névre, tehát a „listen sikerült" önmagában nem
   bizonyíték (mértük: így a második is primary lett). Őr nélkül (nincs helyi socket) a program a régi
   módon indul, csak jelez a naplóban.
2. **Tálcaikon** a `MainWindow`-ban: a program ikonja (ugyanaz, mint a parancsikoné), „Antikolt — fut" tipp,
   kattintásra ki-be váltás, menü: „Megnyitás" / „Kilépés". A **bezárás gomb a tálcára tesz**, nem lép ki,
   és ezt egyszer elmagyarázza; a kilépés a menü „Kilépés" pontja. Tálca nélkül (vagy `ANTIKOLT_NO_TRAY`)
   minden a régi — ebben a módban fut a többi önteszt.

**Amit tudni kell:** a tálcára tett ablak nem ad vissza memóriát (a motor és a rács él, ~150 MB), de a nyitott
kártya a 10 perces szabály szerint hibernál, tehát a tálcán várakozó böngésző a kártya memóriáját elengedi.

**Bizonyíték:** `--traytest` → `instance_primary=ok`, `instance_second=instance=secondary exit=0` (valódi
második folyamat!), `instance_raised=ok`, `tray_icon=ok`, `tray_hides=ok`, `tray_restores=ok`, `tray_quit=ok`,
`fallback_tray=absent close=closed`, `tray_and_instance=ok`.
## 11. Kész: egy ablak, és a sötét oldalak (`--themetest`, ADR-044)

Három panasz, három válasz, mind mérve:

1. **Plusz konzolablak.** A shell console-subsystem exe, és a Windows a parancsikonos indításhoz ad neki egy
   saját konzolablakot (mért: `ConsoleWindowClass` a folyamat ablakai között). Most `DropOwnConsoleWindow`
   eldobja, ha egyedül vagyunk rajta; ha egy shell is kapcsolódik hozzá (minden önteszt), az övé marad. A
   GUI-subsystem út kipróbálva elvetve: a PowerShell nem várja meg és nem adja vissza a GUI-program kimenetét,
   ami a kaput csendben hatástalanná tenné.
2. **A rács nem követte a vezérlősávot** — saját `data-theme` kapcsolója volt, a shell-push pedig egy klikkel
   le is maradt. Javítva: a vezérlősáv döntése hajtja, témaváltáskor és rácstöltéskor is.
3. **A weboldal fehér maradt** (a Wikipedia cikke), miközben az ablak amoled volt — ez az ADR-037 szerinti
   viselkedés volt, nem hiba. Új kapcsoló: **„🌙 Sötét oldalak"** (alapból ki, megjegyzett), ami a motor
   `set_forced_dark` opjával egy nevesített stíluselemmel invertálja a lapot.

**Bizonyíték** (`--themetest`): `theme_chrome` 244 / 58 / 40 / 13 (light/grey/dark/amoled), `grid_capture=light
luma=240` → `grid_capture=amoled luma=50`, `grid_theme_attr=amoled`, `dark_capture=mode_off luma=251` →
`mode_invert 4` / `mode_dim 39` / `mode_warm 33`, `dark_page_modes=ok`, `theme_wcag_aa=ok`, `theme_icons=ok`,
`theme_check=ok`.

**Amit a felhasználónak tudni kell:** a sötétítés **fokozatát** *ő* választja (Kikapcsolva / Sötét / Sötét
szürke / Sötét meleg), a beállítás a profilban marad (`QSettings`, `ui/page_dark_mode`), az inverzió pedig
közelítő (a kép- és videószínek visszainvertálva is eltérhetnek az eredetitől).

**Kapcsolódó szabály:** `08-DIZAJN.md` §8–9 és `15-HOZZAJARULAS.md` §4 — egy látható ablak, és a saját
konzolablak eldobása.

## 12. Függőben lévő fejlesztések (2026-11-19-i állapot)

Ez a lista a kód és a napló alapján készült, nem a `04-UTEMTERV` jelölőiből: az ütemterv pipái elavultak
(több kész dolog pipa nélkül áll, lásd lent), ezért a valós állapot ez a fejezet.

**Nemrég kész (a 2026-11-18/19-i szeletek):** tálcaikon + egypéldányos indítás (ADR-043), egy látható ablak
(a saját konzolablak eldobása), a felületi témák **mérve** pixelenként + WCAG AA, a rács követi a vezérlősávot,
az oldalak négy sötét fokozata (ADR-044/045), a **shell naplófájl** (`logs/shell.log`, rotálva), a **keresés
ékezet-független** (ADR-047: `kave` megtalálja a `Kávézó` kártyát, az üres találat megnevezi a kifejezést), a
**takarítás média-kivétele** (ADR-048), az **500 kártyás rács mérve** (ADR: `--gridscaletest`, 13 renderelt
DOM-elem 503-ból), az **oldalszöveg-keresés** (ADR-049: a fagyasztáskor rögzített lap-szöveg is található),
a **konténerek** teljes életciklusa (ADR-050…054), a **címkék** (ADR-055: a kártyán, a magban
normalizálva, a keresés ékezet nélkül is látja), és a **munkaterületek** (ADR-056: a fül-sor a mag
`workspaces.json`-jából, a kártya `workspace` mezője tárolt adat — a régi `index > 12` kitalálás helyett).

**Termékfunkciók (nagy tételek):**

- **Virtualizált rács 500 kártyával** — **mérve** (`--gridscaletest`): 503 kártya, 13 renderelt DOM-elem, 20 ms
  az első renderig; a görgetéses ugrás ebben a harnessben nem mérhető (nincs layout-magasság), ezt a teszt
  kimondja. Nyitott: konténerenkénti engedély/proxy (a szerkeszthető kezdőlap és a kártyánkénti munkamenet-
  felülírás kész: 106./117./118. és 121. szelet).
- **Munkaterületek** — **kész** (ADR-056/057/058, `--workspacetest`): a fül-sor a mag `workspaces.json`-jából jön
  (`workspaces_list`/`workspace_upsert`/`workspace_delete`), a kártya `workspace` mezője **tárolt adat** (a régi
  `index > 12` kitalálás javítva), a fül-soron van életciklus: `＋`, dupla kattintás = átnevezés, `×` = törlés (a
  mag `in_use` válasza megjelenik), és a kártya munkaterülete a kártya saját választójával váltható (ADR-058).
  A fogd-és-vidd is kész (ADR-058 megerősítve, `--workspacedragtest`, 123. szelet).
- **Profil-költöztetés** — **kész a magban és a hídon** (ADR-059/060, `--profiletest`):
  `profile_export`/`profile_import` egy JSON-kötegben (`schema`, `cards`, `containers`, `workspaces`,
  `sessions`, `rules`); az ismeretlen séma elutasítás, az import előbb validál, a híd `profile.export/import`
  útvonalat vár (a lapon `antikoltProfileWrite/Read`). Nyitott: a **gomb** és a natív fájlválasztó (a harnessben
  nem kattintható, ezért `not-measurable`), és a köteg titkosítása (a munkamenet-sorok miatt valódi döntés).
- **Konténerek** — **kész** (ADR-050/051/052/053, `--containertest`): a kártya színes pontja és konténer-
  választója, a chip-legend + szűrő, a tábla a magban (SQLite-sorok, ADR-070), és a felületi életciklus: `＋`
  konténer, dupla kattintás = átnevezés, `×` = törlés — használt konténernél **áthelyezéssel** (ADR-054:
  a mag `move_to`-val előbb áthelyez, aztán töröl; a legenda felajánlja). **Konténerenkénti engedély/proxy: kész
  és mérve** (ADR-071, 122. szelet) — a mag tárolja, a shell `surfacePolicyFromEntry`-vel építi, a motor alkalmazza
  (`--containerpolicytest`, `--partitiontest`, `--containertest`).
- **Tároló** — **kész** (ADR-069/070, napló 96–97): mind a négy store **SQLite-sorok** (`antikolt.sqlite3`,
  `user_version=4`, WAL) — kártyák, konténerek, munkaterületek, munkamenetek; a négy `*.json` **export** (átvételkor,
  leálláskor, `db_status(sync)`-ra); a kapcsoló előtti profil store-onként egyszer átvételre kerül (`meta` jelölők),
  egy olvashatatlan régi dokumentum megállítja a magot ahelyett hogy felülírná, az üres profil pedig nem seedel újra;
  a `db_status` mind a négy exportot soronként ellenőrzi. Nyitott: a hardveres GPU-út (VECTOR), F3 és az OAuth.
- **OAuth/SSO popup** a helyes partícióban (max. 60 s, dokumentált I1-kivétel — a mechanizmus kész, a
  bejelentkezési folyamat nincs összekötve).
- **Letöltéskezelő, Ctrl+F, PDF, PiP, nyomtatás, összeomlás utáni helyreállítás, HU/EN felület,
  jelszó- és könyvjelző-import, Linux (Nobara/KDE/Wayland) + RPM.**
- **Média-kivétel a takarításból** (§2) **kész** (ADR-048, `--mediacleantest`): a játszó kártya megmarad. A(z)
  §3-beli `test.hang` lap-oldali hook és a renderelt jegyzet mérése viszont még nyitott.
- **Oldalszöveg-keresés** **kész** (ADR-049, `--searchtest` `search_page_text=ok`): a fagyasztott kártyák lap-
  szövege is kereshető. Ami maradt: élő kártyánál nincs capture (a fagyasztás pillanatában készül); a `page_text`
  mezőt a séma már ismeri (`0002`), és a kártya sora az adatbázisban él (ADR-069).
**Széf és passkey (F4/F4b):** DEK/KEK, opcionális mesterjelszó, auto-zárás, KDBX export, kitöltés izolált
világban origin-kötéssel, TOTP + vágólap-tisztítás, szoftveres hitelesítő (`create`/`get` teszt-RP-vel),
őszinte tájékoztatás arról, hogy nincs hardveres védelem. **Blokkol:** `cargo vendor` kell hozzá, és ehhez
hálózat (a függőségek lemezen nincsenek meg).

**Memória (F6):** büdzsé-vezérlő sávokkal és médiaminőség-korláttal, kártyánkénti bontás, technika-létra
luminancia-ellenőrzéssel, öt erőforrás-profil, raszter-kezdőlap (motor leállítva), hangolási riport, valamint
a mérőpad S3/S6 állításai. A `warn`/`hard` sávokból **már `pressure` jel** számítódik (ADR-020, `--memorypressuretest`,
124. szelet); a tényleges **művelet** (a videófelbontás-korlát és a nyomásra kötött akció) még hátra van.

**Kiadás:** SignPath-jelentkezés (**emberi lépés**: publikus repó + elbírálás) → aláírt exe; Store identity +
helyi MSIX telepítés (Partner Center + tanúsítvány); a `release.yml` scaffold sosem futott; frissítő
(Ed25519 aláírás) és telepítő hátra van.

**Apróbb, ismert hiányosságok:**

- A `--previewqueue` „elavult előnézet" ága éles használatban még nincs mérve; a `sha256` blobtár blokkolt
  (`sha2` crate nincs a lemezen).
- Oldalankénti téma-kivétel (§4c) szándékosan nincs; a `04-UTEMTERV` pipáit a kódhoz kell igazítani.
- A GUI-subsystem átállás (a konzolablak helyett) a harness `Start-Process -Wait` átírásával járna — most
  nem éri meg, dokumentálva van.
