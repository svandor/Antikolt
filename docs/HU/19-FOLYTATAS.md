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

**Miért:** M3 („saját napi böngésző”) fő tétele: `keep`/`sweep`/`wipe`, látható munkamenet-panellel.

**Hogyan:** a logika kész (`crates/antikolt-core/src/session.rs`, 13 teszt), a tár JSON
(`crates/antikolt-core-service/src/store.rs`) — a `session` rekordok ugyanoda kerüljenek, a
`pending_maintenance` mintájára; a shell csak megjeleníti. A `wipe` végrehajtásához a motor
`clear_origin_data` opja kell (ma `not_supported`).

**Bizonyíték:** mag-oldali egységtesztek + a `--coretest` kiterjesztése egy `session_list` hívással.

## 3. Kérés-irány a laptól (protokoll v2)

**Miért:** a lap ma nem tud kérdezni (keresés, frissítés, új kártya) — ezért van a shell-tolás
(ADR-032). A `cards.upsert` és a keresés e nélkül nem megy.

**Hogyan:** a motor tartsa a `CefMessageRouterBrowserSide::Callback`-et (perzisztens `cefQuery`),
jöjjön egy `bridge_result` üzenet, és **`PROTOCOL_VERSION` 1 → 2** (`crates/antikolt-ipc`,
`apps/engine-host/src/protocol.cpp` `version` ellenőrzés, `EngineClient`) — az ADR-032 szerint a tolás
ekkor visszaminősül eseménnyé („a lista megváltozott”).

**Bizonyíték:** `--gridtest` keresés-fázisa (a lap `cards.search`-je a magtól kap találatot) + a
protokoll roundtrip-tesztek v2-re állítva.

## 4. Előnézetek

**Miért:** F2 (rács-előnézet) — a `capture` primitív kész, a feldolgozó nincs.

**Hogyan:** `antikolt://blob/<név>` scheme handler a motorban
(`CefRegisterSchemeHandlerFactory`, a profil `previews/` mappájából, név-sanitizálással), majd egy
ütemező: egy kártya egyszerre, 8 s limit, napi 100 frissítés (`02-ARCHITEKTURA.md` §5). A
tartalom-szerinti blob-tár (`sha256`) **blokkolt**: nincs `sha2` crate; addig a fájlnév a kártya id-je.

**Bizonyíték:** `--gridtest` második fázisa: a kártya `previewRef`-je betöltődik, a kép nem üres
(`naturalWidth > 0`), és a `capture` fájlja a profilban van.

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
