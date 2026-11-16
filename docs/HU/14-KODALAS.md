# 14 — Kódaláírás: mennyibe kerül, és mi történik aláírás nélkül (Q14 részletesen)

Rövid válasz: **az aláírás nem kötelező, de a felhasználói élményt erősen befolyásolja Windows-on.**
A jó hír: egy nyílt forrású projektnek van **ingyenes** útja (SignPath Foundation), és a frissítések
integritását ingyenes eszközzel (Ed25519/minisign) is lehet garantálni. A rossz hír: a SmartScreen
figyelmeztetés az első hónapokban így is megjelenik, mert a Microsoft „hírnevet" gyűjt, nem aláírást néz.

## 1. Mi történik, ha aláírás nélkül adod ki?

| Állapot | Mit lát a felhasználó | Következmény |
|---|---|---|
| **Aláírás nélkül** | „A Windows megvédte a PC-t" kék figyelmeztetés → „További információk" → „Futtatás mindenképp" | Sokan itt adják fel; bizalmat rombol |
| **Önaláírt tanúsítvánnyal** | Ugyanaz a figyelmeztetés, plusz ismeretlen kiadó | Semmit nem javít, csak bonyolít |
| **OV tanúsítvánnyal** | A kiadó neve látszik, de a figyelmeztetés **addig megmarad, amíg hírnév nem gyűlik** | Javul, de nem azonnal |
| **EV tanúsítvánnyal** | Ugyanaz, mint OV-nál | **2024 óta az EV sem kerüli meg a SmartScreen-t** — a prémiumár nem ezért kell |
| **Microsoft Store-on keresztül** | Nincs figyelmeztetés (a Microsoft újra-aláírja) | Ez a legtisztább út, de Store-függőség |

Forrás: Microsoft Learn, *SmartScreen reputation for developers*.

## 2. Tanúsítvány-típusok és valós 2026-os árak

| Típus | Költség | Magánszemélynek? | Hardveres kulcs kell? | SmartScreen |
|---|---|---|---|---|
| **OV kódaláírás** | $150–300 / év | igen (EU-ban is) | **igen** (USB-token vagy felhős HSM; 2023 júniusa óta kötelező) | hírnév épül |
| **EV kódaláírás** | $400+ / év | igen | igen | ugyanaz, mint OV |
| **Azure Artifact Signing** (korábban Trusted Signing) | ~$10 / hó | **magánszemély: csak USA/Kanada**; szervezet: USA/Kanada/EU/UK | nem (CI-ből aláír) | hírnév épül |
| **SignPath Foundation** | **ingyenes** (OSI-licencű nyílt forrású projektnek) | igen | nem (az ő pipeline-jukban) | hírnév épül, a kiadó a SignPath Foundation |
| **Sigstore** | ingyenes | igen | nem | nincs SmartScreen-hatás, de kriptográfiailag ellenőrizhető |
| **Önaláírt** | ingyenes | — | — | semmit nem javít |

**Kritikus részlet magánszemélynek, EU-ban:** az Azure Artifact Signing **magánszemélyként nem elérhető**
(csak USA/Kanada), szervezetként viszont igen. Vagyis ha később cég lesz belőle, ez a legolcsóbb út;
egyénként a két reális opció: SignPath Foundation (ingyenes) vagy OV tanúsítvány (~$150–300/év + token).

## 3. A javasolt út (költség optimalizálva)

### v0.x — belső/dogfooding szakasz
- **Nincs aláírás.** A telepítőt te telepíted, a SmartScreen-figyelmeztetés nem probléma.
- **DE**: a frissítő már most **Ed25519/minisign aláírt manifest**-tel ellenőrizze a csomagot
  (a publikus kulcs az alkalmazásba égetve, „fail-closed": ha nem stimmel, nem frissít).
- Ez a rész ingyenes, és később is ez marad az update-lánc gerince.

### v1.0 — publikus kiadás, **ingyenes alapon, automatizált frissítéssel**

A cél: **egy fillér tanúsítvány, és a frissítések emberi kéz nélkül menjenek.** Ez három külön réteggel
oldható meg — és fontos látni, hogy a három réteg **független** egymástól:

| Réteg | Mi védi | Automatizálható? | Költség |
|---|---|---|---|
| **1. Frissítés-integritás** (a legfontosabb) | a letöltött csomag valódisága | **teljesen** (CI-generált Ed25519 aláírás a manifesten) | 0 |
| **2. Származás (provenance)** | a bináris a nyilvános forrásból, a nyilvános CI-ben készült | **teljesen** (Sigstore/cosign + GitHub build attestation) | 0 |
| **3. Authenticode** (Windows „kiadó" név) | a SmartScreen/bizalmi lánc | **félig**: a SignPath-aláírás kiadásonként egy kézi jóváhagyás | 0 |

**1. réteg — a frissítő lánc (teljesen automatizált, ingyenes):**

1. CI épít → előállítja a `latest.json` manifestet (verzió, platform, SHA-256, kiadási jegyzet).
2. A CI **Ed25519 kulccsal aláírja** a manifestet (a privát kulcs titkosított secret a CI-ben, a publikus
   kulcs az alkalmazásba égetve).
3. Az alkalmazás a frissítéskor: manifest letöltés HTTPS-en → **aláírás-ellenőrzés** → SHA-256 ellenőrzés
   → letöltés → újra ellenőrzés → **atomikus csere** → hiba esetén **visszaállás az előző verzióra**.
   „Fail-closed": ha bármi nem stimmel, nem frissít.
4. **Két kulcs** (aktuális + következő) az alkalmazásban, hogy a kulcsrotáció ne törje el a telepítéseket.
5. Fokozatos kiadás: a manifestben „csatorna" és „minimális verzió" mező; egy sérült kiadás
   **visszahívható** (kill switch).

Ez a réteg **soha nem igényel embert**, és ez adja a frissítések biztonságát. Az Authenticode ehhez képest
csak a Windows *felhasználói élményét* javítja (kevesebb figyelmeztetés), nem a tényleges védelmet.

**2. réteg — Sigstore/provenance (automatizált, ingyenes):** a kiadási artifactok Sigstore-aláírása és a
GitHub build attestation ingyenes és teljesen automatizált; bárki ellenőrizheti, hogy a bináris a nyilvános
repóból, a nyilvános CI-ből származik. Ez a „nem tudtam máshonnan letölteni" bizonyítéka.

**3. réteg — Authenticode (SignPath Foundation, ingyenes, kiadásonként 1 kattintás):**

- Feltétel: **OSI-licenc** (az MPL-2.0 megfelel), nyilvános repó, publikált „code signing policy" a
  projekt honlapján.
- A GitHub Action beküldi az artifactot; a **kiadás jóváhagyása egy kattintás** a SignPath felületén —
  ez az ára az ingyenességnek (nem lehet teljesen automatikus).
- A tanúsítvány a *SignPath Foundation* nevére szól → a SmartScreen ezt mutatja kiadóként; a honlapon meg
  kell magyarázni.
- **Ha ez a kattintás sem fér bele:** kihagyjuk az Authenticode-ot, és marad az 1+2 réteg (a frissítések
  továbbra is automatizáltak és védettek), csak a SmartScreen figyelmeztet tovább. Ez is teljesen
  vállalható — ezt a döntést a v1.0 előtt hozzuk meg.

### Amit NEM javaslok
- **Bármilyen fizetős tanúsítvány** (OV/EV): a kérésed egyértelmű, és a hozzáadott érték a 2024-es
  SmartScreen-változás óta kicsi.
- **Önaláírt tanúsítvány**: a felhasználónak ugyanolyan rossz, mint az aláírás nélküli, csak zavaróbb.
- **Store-only terjesztés**: a Store kizárná a saját frissítőt és a saját telepítőt.

## 4. Linux (Nobara/Fedora) és Flatpak

| Csatorna | Követelmény | Megjegyzés |
|---|---|---|
| **RPM (saját repo)** | GPG-aláírt repó-metaadat (`repo-gpgkey`), a `dnf` ellenőrzi | A legkevesebb súrlódás; a kulcsot a honlapon közzé kell tenni |
| **RPM (Copr / Fedora repo)** | a Copr maga aláírja | egyszerű, de függőség |
| **Flatpak / Flathub** | **nincs kódaláírás-követelmény**; a review a sandbox-megfelelést, a licencet és a „nincs védjegy-sértés" feltételt nézi | böngésző-szerű app elfogadható, ha megfelel a befogadási szabályzatnak |

## 5. Mire kell felkészülni (kommunikáció)

- Az első **hetekben/hónapokban a SmartScreen figyelmeztetni fog**, akkor is, ha aláírt. Ez nem hiba.
  A honlapra és a telepítő mellé kerüljön egy rövid magyarázat („új kiadó, a Windows hírnevet gyűjt").
- A **kiadó neve** (SignPath Foundation) eltér a termék nevétől — ezt is meg kell magyarázni, különben
  gyanúsnak tűnik.
- A frissítés **soha nem telepít aláírás-ellenőrzés nélkül**, és a kulcs cseréjére legyen terv
  (két kulcs: aktuális + következő, hogy a rotáció ne törjön el minden telepítést).

## 6. Költség-összegzés

| Kiadás | v0.x | v1.0 (ingyenes) | Ha valaha mégis kell |
|---|---|---|---|
| Tanúsítvány | 0 | **0** (SignPath Foundation) | OV ~$150–300/év (nem javasolt) |
| Hardveres token | 0 | **0** | OV esetén ~$50–150 egyszeri |
| Frissítés-aláírás (Ed25519) | 0 | **0** | — |
| Provenance (Sigstore) | 0 | **0** | — |
| CI-integráció | 0 | GitHub Action (ingyenes) | — |
| Emberi munka | 0 | kiadásonként **1 kattintás** (SignPath-jóváhagyás) | — |
| SmartScreen-figyelmeztetés | van | az első időszakban van, később elmúlik | — |

**Döntés (a kérésed szerint):** **nulla költség**, ingyenes tanúsítvány (SignPath Foundation) +
**teljesen automatizált frissítési lánc** (Ed25519 + Sigstore). Fizetős tanúsítvány nincs a tervben; ha a
SignPath-jóváhagyás mégis zavaró, az Authenticode kimarad, és a frissítések akkor is automatizáltak és
védettek maradnak.
