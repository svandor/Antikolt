# 19 — Folytatási munkarend (a 20. szelet után)

> Ez a dokumentum **nem terv**, hanem munkarend: az aktuális állapotból indul, és minden tételhez
> megadja, melyik fájlban, mi a teendő, és mi bizonyítja. A terv a `00`–`15`, a döntések a `05`, a
> kronológia a `16`; ez a lap a következő munkamenet első olvasmánya.

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
> A **mennyiség** oldala kész: `SessionSize` + `loses_login()` a ledger során (41. szelet, 78 core teszt).\n> **Ami hátra van:** `LossEstimate` és `SessionSize` kijelzése a panelen, a takarítás **média-kivétele** (egy játszó kártya
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
> **Ami hátra van:** a jegyzet-szerkesztő fókusz-visszaadása, a lap-oldali `test.hang` hook
> dokumentáltsága a felhasználói doksiban (ma csak a shell README említi), és az, hogy a rács saját
> keresése szándékosan lokális marad (a lap a teljes listát kapja).

## 2b. Munkamenet-szabályok állítása (kész, ADR-039)

> `session_rules` op + panel-szerkesztő (óra, app-zárnál törlés, Mentés). `--rulestest`: `rules_ttl_ms=7200000`,
> `rules_lock_wipe=true`, `rules_refused=refused`, `rules_reread_ttl_ms=7200000`, `session_rules=ok`.
> A szerkesztőmezők a mag állapotát mutatják (`panel_ttl_hours=2`, `panel_lock_wipe=true`), a konténer-alapértelmezés változatlanul megy vissza.
> **Ami hátra van:** kártyánkénti (origin-szintű) felülírás.

## 4. Előnézetek

> **Állapot: kiszolgálás és előállítás is kész, mérve** (30–31. szelet, ADR-035). A motor az `antikolt`
> sémát regisztrálja és a `blob/<név>` kérést a `<user data>/previews/<név>` fájlból válaszolja meg (a nevet
> a könyvtáron belül feloldva); a shell a kártya megnyitásakor `capture`-rel képet készít, a profilba
> mozgatja, és a `preview_ref` mezőbe írja a nevét. `--previewtest`: `preview_load=loaded:4x3`,
> `preview_traversal=refused`. `--previewgentest`: `preview_ref=preview-probe.png`,
> `preview_page=loaded:1560x857`, `preview_generation=ok`.
> A napi ≤100 korlát **tartós** (`previews/budget.json`, 38. szelet, `--previewbudget`), a 7 napos frissítés
> kész (`--previewrefresh`), az árva fájlok takarítása kész (`--previewclean`).
> **Ami hátra van:** a **háttérben** készített előnézetek (I1: ma csak a felhasználó által megnyitott kártya
> kap képet), és a `sha256`-alapú blob-tár (blokkolt: nincs `sha2` crate).

**Miért:** F2 (rács-előnézet) — a `capture` primitív kész, a feldolgozó nincs.

**Hogyan:** `antikolt://blob/<név>` scheme handler a motorban
(`CefRegisterSchemeHandlerFactory`, a profil `previews/` mappájából, név-sanitizálással), majd egy
ütemező: egy kártya egyszerre, 8 s limit, napi 100 frissítés (`02-ARCHITEKTURA.md` §5). A
tartalom-szerinti blob-tár (`sha256`) **blokkolt**: nincs `sha2` crate; addig a fájlnév a kártya id-je.

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
