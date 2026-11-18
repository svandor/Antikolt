# Aláírás — hogyan szerezzünk tanúsítványt, és mi a teendő

> Ez a lap azt írja le, **milyen úton lehet ma (2026) aláírt Antikolt-exét csinálni**, mennyibe kerül, és mi a
> teendő a repóban. A rövid válasz: **ingyenes, nyilvánosan is elfogadott kódaláíró tanúsítvány csak egy
> úton van: a SignPath Foundation** (nyílt forrású projekteknek), és ott a kulcs nem a mi gépünkön van.

## Miért nem lehet csak úgy „ingyen tanúsítványt kérni"

A CA/Browser Forum szabálya (2023 óta) szerint a nyilvánosan elfogadott kódaláíró tanúsítvány **privát kulcsa
hardveres védelmen** (HSM vagy USB-token, FIPS 140-2 Level 2 / Common Criteria) kell legyen. Ezért:

- nincs olyan ingyenes tanúsítvány, amit letöltenél és a saját gépeden használnál;
- a korábbi „ingyenes" ajánlatok (pl. Certum Open Source) is **fizetős kártyát** igényelnek;
- a tanúsítvány általában **személyre** szól, nem a projektre.

Emiatt a nyílt forrású projekteknek két reális út marad: **valaki más aláírja helyetted** (SignPath, Microsoft
Store), vagy **fizetsz** egy szolgáltatásért (Azure Trusted Signing, Certum, SSL.com).

## 1. Ingyenes út: SignPath Foundation (ez a javasolt)

- <https://signpath.org/> — „Free Code Signing for Open Source software".
- A tanúsítvány a **SignPath** nevére szól, ők vállalják, hogy a bináris a te repódból épült; a privát kulcs az
  **ő HSM-jükben** van, hozzád nem kerül.
- **Nincs személyazonosítás** és nincs kártya; a jogosultság feltétele, hogy a projekt **nyílt forrású**,
  nyilvános repóban legyen, aktív legyen, és a build az ő (vagy CI-) folyamatukon menjen át.
- Az Antikolt **MPL-2.0**, tehát megfelelő jelölt.

**Teendők (sorrendben):**

1. **Nyilvános repó** (GitHub) — a SignPath a repó URL-jét és a buildet látja, tehát a kódnak publikusan
   elérhetőnek kell lennie (a licenc már megvan: `LICENSE`).
2. **Jelentkezés**: <https://signpath.org/apply> — projektre, licence, a fő maintainer elérhetősége, a
   kiadások helye (Releases) kellenek. Elbírálás után kapsz egy SignPath.io fiókot.
3. **CI-folyamat**, ami a kiadást építi (GitHub Actions), mert a SignPath ebbe kapcsolódik:
   - a workflow Windows runneren lefuttatja a `scripts/dist.ps1 -Release`-t;
   - az elkészült exéket feltölti a SignPath-ba (SignPath GitHub Action vagy PowerShell),
   - a **SignPath aláírja**, majd a kiadásba az aláírt fájlok kerülnek;
   - a verzió- és build-információt a workflow rögzíti (ezt a SignPath provenance-ként elvárja).
4. **Ellenőrzés a kiadás után**: `pwsh -File scripts/sign.ps1` nem kell (a kulcs náluk van), de
   `signtool verify /pa /v <exe>` helyben megmutatja, hogy a letöltött bináris aláírva van és ki írta alá.
5. **SmartScreen**: az aláírás megmutatja a kiadót, de a *reputáció* idővel épül. Az első hetekben még
   figyelmeztethet; ezt nem lehet megvenni, csak kivárni (és minél több letöltéssel erősödik).

## Mikor jelentkezz? (a sorrend, amit érdemes követni)

A SignPath **kiadásokat ír alá a saját folyamatában**, ezért a jelentkezés akkor a leghasznosabb, amikor már
van **kiadható, reprodukálható build** és (lehetőleg) publikus repó + CI. Addig a helyes út:

1. **most** — fejlesztés és teszt **aláírás nélkül vagy self-signed** tanúsítvánnyal (a saját gépeden), mert a
   self-signed úgysem ad bizalmat idegen gépen;
2. **amikor van release** — SignPath jelentkezés (ingyenes, OSS), a SignPath-os aláírás beépítése a CI-be;
3. **párhuzamosan** — ha az MSIX/Store út is szóba jön, a Store-fiók (ma **ingyenes egyéni fejlesztőknek**),
   mert ott az aláírást a Microsoft végzi (lásd 4. pont).

Korábban jelentkezni nem tilos, de a SignPath a build-folyamatról és a kiadásokról kérdez — üres kézzel
lassabb az elbírálás.

## 2. Ingyenes, de nem nyilvános: saját (self-signed) tanúsítvány

Ez **nem** ad SmartScreen-bizalmat, csak a saját gépeiden működik (és azokon is csak akkor, ha a
tanúsítványt telepítették a „Megbízható legfelső szintű hitelesítésszolgáltatók" tárba). Fejlesztéshez,
belső terjesztéshez, a folyamat kipróbálásához jó:

```powershell
pwsh -File scripts/sign.ps1 -SelfSigned
```

A script létrehoz egy `CN=Antikolt test (NOT TRUSTED)` tanúsítványt a felhasználói tárban, aláírja a
`dist\Antikolt\*.exe` fájlokat, és **ellenőrzi** az aláírást (`signtool verify /pa`). A neve is figyelmeztet:
ez nem megoldás a kiadásra.

## 3. Olcsó, valódi: Azure Artifact Signing (korábban Trusted Signing, kb. 10 USD/hó)

- <https://learn.microsoft.com/azure/trusted-signing/> — felhős HSM, nincs USB-token, CI-ből hívható.
- **Fontos korlát:** szervezeteknek USA / Kanada / EU / UK; **egyéni fejlesztőknek csak USA és Kanada**. Magyar egyéni fejlesztőként tehát ez az út *nem* elérhető (cég kellene hozzá), az OV-tanúsítvány vagy a Store marad.
- Ellentétben a régi helyzettel: **egyéni fejlesztő is igényelhet** (identitás-ellenőrzéssel; cégnél
  legalább 3 éves, ellenőrizhető cégadatok kellenek, és a szolgáltatás régiókhoz kötött).
- Ez a legolcsóbb **valódi** (nyilvánosan elfogadott) út, ha nem akarunk a SignPath folyamatára várni.

## 4. Microsoft Store (MSIX) — aláírás nélkül is „aláírt"

Ez a **valóban ingyenes út az aláíráshoz**: MSIX-csomagnál a Store a minősítés után **automatikusan újraaláírja** a csomagot, tanúsítványt nem kell venni; a fejlesztői fiók 2025 óta **egyéni fejlesztőknek ingyenes** (`storedeveloper.microsoft.com`). Ha viszont **MSI/EXE telepítőt** adunk be a Store-ba, azt magunknak kell aláírni (Microsoft Trusted Root CA-hoz láncolt tanúsítvánnyal). Ez a hordozható mappánkat **nem** írja alá. Ha MSIX-csomagot adunk ki a Store-on keresztül, a **Microsoft írja alá** a csomagot. Ehhez fejlesztői fiók
kell (egyszeri díj), és a Store-feltételeknek megfelelő csomag. A hordozható mappa alternatívája, nem
kiegészítője: a Store-ból telepített MSIX más élmény, mint a „mappa + parancsikon".

## Összefoglaló táblázat (Microsoft saját összehasonlítása, 2026)

| Út | Költség | SmartScreen | Elérhető-e nekünk (HU)? |
|---|---|---|---|
| **Store, MSIX** (a Microsoft írja alá) | **ingyenes** (fiók is, egyéni fejlesztőknek) | ✅ nincs figyelmeztetés | igen, MSIX-csomag kell |
| Store, MSI/EXE telepítő | saját tanúsítvány kell hozzá | ✅ (telepítéskor) | csak tanúsítvánnyal |
| **SignPath Foundation** (OSS) | **ingyenes** | ⚠️ reputáció épül | igen (publikus repó + CI) |
| Azure Artifact Signing | ~9.99 USD/hó | ⚠️ reputáció épül | **egyéninek nem** (csak US/CA); cégként EU-ban igen |
| OV tanúsítvány (DigiCert, Sectigo…) | 150–300 USD/év | ⚠️ reputáció épül | igen (hardveres token) |
| EV tanúsítvány | 400+ USD/év | ⚠️ **2024 óta nem kerüli meg** a SmartScreen-t | igen, de nem éri meg ezért |
| Self-signed | ingyenes | ❌ blokkolja a telepítést másoknál | csak fejlesztéshez |

## Mi a teendő most az Antikolt repóban

| Lépés | Állapot |
|---|---|
| `scripts/sign.ps1` — aláírás és **ellenőrzés**, PFX / store / self-signed módban; tanúsítvány nélkül elutasít | kész |
| `scripts/dist.ps1 -Release` — a csomag, amit aláírni kell | kész |
| Nyilvános repó + SignPath jelentkezés | **emberi lépés** (fiók, e-mail, elbírálás) |
| GitHub Actions workflow (build → SignPath → kiadás) | nincs még; a SignPath jóváhagyása után van értelme |
| Kiadás utáni ellenőrzés (`signtool verify /pa`) | a `sign.ps1` megmutatja a módját |

**Amihez tanúsítvány kell és nem kód**: a jelentkezés, a szerződés (SignPath Terms), illetve fizetős útnál a
cég/egyéni adatok ellenőrzése. Ezeket nem lehet „megoldani a repóban", ezért itt a teendő a fenti sorrend
követése.

## Hivatkozások

- SignPath Foundation: <https://signpath.org/> (jelentkezés: <https://signpath.org/apply>)
- Certum kódaláíró tanúsítványok (fizetős, kártyával): <https://certum.eu/>
- Azure Trusted Signing: <https://learn.microsoft.com/azure/trusted-signing/>
- MSIX aláírás: <https://learn.microsoft.com/windows/msix/package/signing-package-overview>
- Windows SmartScreen és reputáció: <https://learn.microsoft.com/windows/security/operating-system-security/virus-and-threat-protection/microsoft-defender-smartscreen/>

## Mit mértünk ebben a repóban (tényleges kimenet)

```
pwsh -File scripts/sign.ps1
  signtool : C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe
  file     : dist\Antikolt\antikolt-shell.exe  (és a másik kettő)
  → no certificate: pass -Pfx …, -Thumbprint …, -SelfSigned, or sign through SignPath in CI
  exit 1

pwsh -File scripts/sign.ps1 -SelfSigned
  → New-SelfSignedCertificate: CertEnroll::CX509Enrollment::_CreateRequest:
    A hozzáférés megtagadva. 0x80070005 (WIN32: 5 ERROR_ACCESS_DENIED)
```

Az első futás a lényeg: a script **tanúsítvány nélkül elutasít**, nem talál ki aláírást. A második azt
mutatja, hogy ebben a **zárt fejlesztői környezetben** a felhasználói tanúsítványtár írása tiltott, tehát a
self-signed út itt **nem volt kipróbálható** — normál Windows-fiókban működnie kell (a kód a dokumentált
módon hívja a `New-SelfSignedCertificate`-et). Ez a korlát, nem a script hibája.
