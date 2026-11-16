# 12 — Passkey / WebAuthn: miért nehéz, és mit tehetünk (Q9 részletesen)

Ez a dokumentum a legfájóbb technikai korlátot tárgyalja. Rövid összefoglaló:

> **A CEF-nek nincs beépített WebAuthn-támogatása.** A passkey a böngésző *beépített* hitelesítőjét
> igényli, amit a Chromiumban a beágyazó félnek (a böngészőnek) kell megvalósítania. A CEF ezt nem
> teszi — a `CefRequestHandler`-nek nincs WebAuthn-horga, és nincs `CefWebAuthn*` API. Ezért
> passkey nélkül a modern oldalak egy része „csak passkey-jel" enged be, és a böngésző nem tud belépni.

## 1. Mi az a passkey, egy mondatban

A passkey egy **kulcspár**: a privát kulcs a felhasználó eszközén (hardveresen védett chipben vagy a
rendszer hitelesítőjében) van, a nyilvános kulcs a szolgáltatónál. Bejelentkezéskor a böngésző aláír egy
kihívást — jelszó nélkül, adathalászat ellen védetten (mert az aláírás az **originhoz** kötött).

A böngésző feladata ebben: **a platform hitelesítő meghívása** (Windows Hello, Touch ID, FIDO2 kulcs) és a
kriptográfiai protokoll lefolytatása. Ezt a Chromiumban a *beágyazó* implementálja — böngészőként nekünk
kellene. A CEF ezt a réteget nem adja.

## 2. Mi hiányzik pontosan

| Réteg | Ki valósítja meg Chromiumban | CEF-ben |
|---|---|---|
| `navigator.credentials.create/get` (JS API) | Chromium renderer | ✅ megvan |
| `WebAuthenticationDelegate` (beágyazói döntések: melyik hitelesítő, milyen UI) | a böngésző (Chrome/Edge) | ❌ **hiányzik** |
| `AuthenticatorRequestClientDelegate` (a felhasználói felület és a CTAP2 hívás) | a böngésző | ❌ **hiányzik** |
| CTAP2 / Windows Hello / libfido2 hívás | a böngésző | ❌ **hiányzik** |

A hiányzó rész nem egy kapcsoló, hanem a Chromium beágyazói integrációjának egy szelete. Mérete a
kutatás szerint **egy részleges Chromium-port nagyságrendje** — nem egyhetes munka.

## 2.5. Mi az a „passkey-only" fiók, és hogyan ellenőrizd?

**Sima (jelszavas) fiók:** van jelszavad, a passkey csak *kényelmi* alternatíva. Ha a böngésző nem tud
passkey-t, akkor jelszóval + TOTP-vel belépsz — nincs gond.

**Passkey-only fiók:** a szolgáltatónál **törölted a jelszót**, és kizárólag passkey-jel lehet belépni.
Ilyenkor nincs mire visszaesni: ha a böngésző nem tud passkey-t, **egyáltalán nem tudsz belépni** — sem
jelszóval, sem „elfelejtett jelszó"-val, mert nincs jelszó. Ez a kemény fal.

**Hol fordul elő leggyakrabban:**

| Szolgáltatás | Mit jelent a gyakorlatban |
|---|---|
| **Google** | a fiók beállításaiban a jelszó **törölhető**, ha van passkey („jelszó nélküli belépés") |
| **Microsoft** | „Passwordless account" bekapcsolva → csak Microsoft Authenticator / passkey / Windows Hello |
| **Apple ID** | passkey vagy biztonsági kulcs kötelezővé tehető |
| **GitHub / X / más** | passkey *támogatott*, de a jelszó általában megmarad → **nem** passkey-only |
| **Vállalati/iskolai SSO** | néha FIDO2/passkey kötelező (ide tartozhat a munkahelyi belépés) |

**Hogyan ellenőrizd 2 perc alatt:**

1. **Google:** `myaccount.google.com` → *Biztonság* → „Hogyan jelentkezik be a Google-fiókba" — ha csak a
   passkey/ujjlenyomat szerepel, és nincs „Jelszó" sor (vagy van „Jelszó hozzáadása" gomb), akkor
   **passkey-only** vagy. Ha ott van a jelszó is, akkor nem.
2. **Microsoft:** `account.microsoft.com` → *Biztonság* → *Speciális biztonsági beállítások* → ha a
   „Jelszó nélküli fiók" **be van kapcsolva**, passkey-only.
3. **Bármelyik oldal:** próbáld meg „inkognitóban" belépni (ott nincs passkey a rendszerben): ha csak
   passkey-t kér és nem kínál jelszót, akkor passkey-only.
4. A böngésző majd ezt magától is jelzi: ha egy kártya megnyitásakor a `navigator.credentials.get()`
   hívás bukik el, és nincs jelszó-mező az oldalon, a felület azt írja: „ez a fiók passkey-t igényel".

**Ha kiderül, hogy van ilyen fiókod:** a v1.0 (passkey nélkül) számodra nem lesz napi böngésző, ezért a
passkey-kutatást (B út) a v1.0 **elé** kell tenni. Ha nincs ilyen, marad az A → B sorrend.

## 3. A lehetőségek (őszintén, költséggel)

| # | Megoldás | Mit tud | Költség | Kockázat |
|---|---|---|---|---|
| **A** | Passkey nélkül indulunk (jelszó + TOTP + runbook) | a legtöbb oldal működik; a passkey-only fiókok nem | 0 | néhány fiók (pl. Google „passkey-only") kizárva; a felhasználó ezt lássa |
| **B** | **Szoftveres passkey a széfben** (CDP virtuális hitelesítő + kulcsexport) | valódi passkey-élmény, platformfüggetlen, a széf hordozza a kulcsot | 3–5 hét kutatás + 4–6 hét fejlesztés | CDP belső API (verziófüggő); nem hardveresen védett; a `userVerification` szimulált |
| **C** | CEF patch: `WebAuthenticationDelegate` implementálása → Windows Hello | igazi, hardveres passkey | hónapok, CEF-karbantartás örökre | nem reális egyéni projektnél |
| **D** | Külön Chromium-segéd csak a passkey-hez | hardveres passkey, izoláltan | +150–300 MB RAM + bonyolult IPC | sérti a memóriacélt; két motort kell karbantartani |
| **E** | JS-shim a fővilágban (`navigator.credentials` felülírása) | látszólag működik | napok | **elvetve**: a shim az oldal JS-e számára látható/módosítható → biztonsági szint csökken, adathalász-védelem sérül |

### 3.1. A **B** út hátrányai — pontosan

B = szoftveres passkey a széfben (CDP virtuális hitelesítő, kulcsexport/újratöltés).

| # | Hátrány | Mit jelent a gyakorlatban |
|---|---|---|
| **B1** | **Nincs hardveres védelem** | a privát kulcs a széfben van (AES-256-GCM, DEK a DPAPI/Argon2id mögött). Aki a széfet feloldja (vagy a Windows-munkamenetedet átveszi), hozzájut. Egy TPM/Windows Hello kulcs **nem exportálható**, tehát lopással sem vihető el. |
| **B2** | **Nincs valódi felhasználói jelenlét-ellenőrzés** | a valódi passkey minden használatnál ujjlenyomatot/arcot/PIN-t kér. A virtuális hitelesítő `isUserVerified=true`-vel „szimulál", tehát a védelmet nekünk kell pótolni: széf-zárolás + jóváhagyó ablak minden passkey-műveletnél. Ez a mi kódrészünk, nem a platform garanciája. |
| **B3** | **Nincs hitelesítés (attestation)** | a virtuális hitelesítő „none" attestation-t ad. Ahol a szolgáltató **igazi** attestationt követel (egyes bankok, vállalati rendszerek, kormányzati belépés), a B út **elutasításra kerül** — ott továbbra sem lehet belépni. |
| **B4** | **Belső/debug CDP API-ra épül** | a `WebAuthn.addVirtualAuthenticator` egy fejlesztői teszt-funkció, nem termék-API. Egy CEF/Chromium-frissítés eltörheti, és akkor **minden passkey-felhasználó egyszerre akad el**. Kezelés: F0 spike + szerződéses tesztek + automatikus visszaesés jelszóra. |
| **B5** | **Nem marad meg újraindítás után** | a virtuális hitelesítő a memóriában él; minden indításnál újra kell injektálni a széfből. Ha a széf zárolva van, a bejelentkezés addig nem megy — ez plusz egy lépés a felhasználónak. |
| **B6** | **Oldal-oldali kompatibilitás bizonytalan** | ha az oldal `isUserVerifyingPlatformAuthenticatorAvailable()`-lel dönt, a virtuális hitelesítőt „nem valódinak" láthatja, és **elrejtheti a passkey gombot** vagy elutasíthat. Oldalanként tesztelni kell. |
| **B7** | **„Eszközhöz kötött" színlelés** | a virtuális hitelesítő eszközhöz kötöttként (device-bound) jelentkezik, miközben a kulcs hordozható. Ez a felhasználónak **előny** (átvihető), de a szolgáltató számára félrevezető lehet — ezért a felületnek ki kell mondania: „szoftveres hitelesítő, nem hardveres". |
| **B8** | **Sync-modell eltér** | az Apple/Google passkey platformon szinkronizálódik; nálunk csak akkor, ha a széfet exportálod/átviszed. |

**Összességében:** a B biztonsági szintje **körülbelül a széfével egyenlő** — nem több. Adathalászat ellen
viszont **igenis véd** (a kulcs originhez kötött, a hamis oldal nem tudja kérni), és a legtöbb fogyasztói
oldalon működik. Ez a reális kompromisszum.

### 3.2. A **D** út hátrányai — pontosan

D = külön Chromium/Electron segédprocesszor, csak azért, hogy hardveres passkey-t tudjon.

| # | Hátrány | Mit jelent a gyakorlatban |
|---|---|---|
| **D1** | **Memória** | +150–300 MB, akár állandóan, akár csak bejelentkezéskor (utóbbi esetben 1–2 s indulás). Ez **szembemegy a projekt teljes memória-ígéretével** (I1 és az 1 GB-os büdzsé). |
| **D2** | **A munkamenet átadása a valódi probléma** | a WebAuthn az originhez kötött, tehát a segédnek ugyanazt az oldalt kell megnyitnia. Ha ott jelentkezik be, a **sütit át kell vinni** a fő motorba — ami két külön motort, két partíciót és egy titok-átadó csatornát jelent. Vagy: a bejelentkezést a segédben fejezed be, és a kártya ott nyílik meg — ami nem a mi motorunk. |
| **D3** | **Két motort kell karbantartani** | két Chromium-verzió, két biztonsági javítási ciklus, +100–150 MB telepítő, két csomagolási út. |
| **D4** | **Rosszabb felhasználói élmény** | a Windows Hello ablak a segédben jelenik meg; Linuxon a libfido2 út külön munka. A „mindent egy ablakban" elv sérül. |
| **D5** | **Nagyobb támadási felület** | egy második motor, egy új IPC-csatorna és egy titok-átadó protokoll — több kód, több hiba, több biztonsági kockázat. |
| **D6** | **Wayland** | a segédnek is kell ablak/OSR, tehát a legnehezebb platformon duplázódik a munka. |

**Amit a D ad:** igazi hardveres passkey (Windows Hello/TPM), attestation is, mindenhol elfogadják.
**Az ára:** a projekt egyik fő ígéretének feladása (kis memória) és egy második motor örökös karbantartása.

### 3.3. Miért nem a C (CEF patch)

A C a „helyes" megoldás lenne, de a Chromium beágyazói WebAuthn-rétegének implementálása a CEF-ben
**hónapok** és **örökös** karbantartás (minden CEF-frissítésnél újra kell illeszteni). Egy ember + egy
ágens ennek nem tud nekiállni. Ha valaha mégis kell, ez lesz az út — de nem most.

## 4. A javaslatom: A most, B a v1.1-ben, C/D kizárva

**v1.0:** nincs passkey. A széf adatmodellje készen áll (`credential.kind = 'passkey'`, `passkey_ct`),
és a felület őszintén jelzi: „ez az oldal passkey-t kér — ehhez a jelszavas belépést kell használnod"
(vagy a runbook megpróbálja a jelszavas utat).

**v1.1 (a B út), de csak egy kutatási spike után.** A B lényege, hogy a böngésző **saját szoftveres
hitelesítőt** üzemeltet:

1. A motor CDP-n kap egy **virtuális hitelesítőt** (`WebAuthn.enable` + `addVirtualAuthenticator`),
   `hasResidentKey = true`, `hasUserVerification = true`, `isUserVerified = true` (a felhasználói jelenlét
   ellenőrzését a **széf zárolása** pótolja: a passkey-művelet csak feloldott széf mellett megy).
2. Regisztrációkor (`create`) a keletkező hitelesítő adatot (`credentialId`, **privát kulcs PKCS#8**,
   `userHandle`, RP-azonosító) **exportáljuk és a széfbe titkosítva mentjük** — így a passkey
   *hordozható* lesz (gépek közt is), ami a hardveres passkey-nál jobb.
3. Bejelentkezéskor (`get`) a széfből **visszatöltjük** a hitelesítőt a virtuális hitelesítőbe, és a
   művelet lefut.
4. A site-ok többsége elfogadja, mert a protokoll szempontjából valódi (origin-kötött) passkey.

**Amit ez a megoldás NEM ad (ki kell mondani):**
- Nincs hardveres védelem: a kulcs a széfben van, nem a TPM/Windows Hello chipjében.
- Nincs „felhasználói jelenlét" (ujjlenyomat/arc) minden használatnál — ezt a széf zárolása helyettesíti.
- A CDP virtuális hitelesítő **belső/debug API**: egy CEF-verzióváltás eltörheti. Ezért:
  - a spike első kérdése: működik-e, és túléli-e a CEF-frissítést;
  - ha nem: visszaállunk az A-ra, és a felhasználónak ezt kommunikáljuk.

**v2 (ha egyszer nagyon kell):** a C út (CEF patch) vagy a D út (külön segéd) — de csak akkor, ha a
passkey-only fiókok miatt a felhasználók ténylegesen elakadnak.

## 5. Mit jelent ez a napi használatban (őszintén)

| Szolgáltatás | v1.0 (passkey nélkül) | v1.1 (B út) |
|---|---|---|
| Google (jelszó + 2FA engedélyezve) | ✅ működik | ✅ |
| Google (passkey-only, jelszó kikapcsolva) | ❌ nem lehet belépni | ✅ |
| Microsoft (jelszó + Authenticator) | ✅ | ✅ |
| Microsoft (passkey-only) | ❌ | ✅ |
| Bank (jelszó + SMS/TOTP) | ✅ | ✅ |
| Bank (passkey-only) | ❌ | ✅ |
| GitHub, X, Discord (jelszó + TOTP) | ✅ | ✅ |
| Új fiókok regisztrációja passkey-jel | ❌ (jelszót kell választani) | ✅ |

**Következtetés:** a v1.0 a legtöbb embernek elég, de **nem mindenkinek** — és ezt a projektnek a
honlapon is ki kell mondania, mielőtt bárki a napi böngészőjévé teszi.

## 6. Döntések (rögzítve) és a maradék teendő

1. ✅ **A → B sorrend elfogadva** (v1.0 passkey nélkül, v1.1 szoftveres passkey a széfben).
2. ✅ **A szoftveres kompromisszum elfogadva** (széfben tárolt kulcs; a jelenlét-ellenőrzést a széf
   zárolása + jóváhagyó ablak pótolja). A B hátrányai a 3.1. pontban, dokumentálva.
3. ⬜ **Teendő (a projektgazda):** a `2.5.` pont szerint ellenőrizd 2 perc alatt, van-e **passkey-only**
   fiókod (Google / Microsoft). Ha van: a B kutatás a v1.0 **elé** kerül (F0-ba), ha nincs: marad a sorrend.
4. A **hardveres** passkey (C patch / D segédprocesszor) **ki van zárva** — a D indoklása a 3.2. pontban,
   hogy később ne kelljen újra végigbeszélni.

## 7. Források

- CEF `CefRequestHandler` referencia (nincs WebAuthn-horog): <https://cef-builds.spotifycdn.com/docs/stable/classCefRequestHandler.html>
- Chromium beágyazói WebAuthn-interfész: `content/public/browser/authenticator_request_client_delegate.h`
- CDP `WebAuthn` domain (virtuális hitelesítő, `addCredential`, `getCredentials`):
  <https://chromedevtools.github.io/devtools-protocol/tot/WebAuthn/>
- CEFSharp `WebAuthnClient` (példa a CDP-út megvalósítására):
  <https://cefsharp.github.io/api/130.1.x/html/T_CefSharp_DevTools_WebAuthn_WebAuthnClient.htm>
