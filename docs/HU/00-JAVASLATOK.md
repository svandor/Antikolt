# 00 — Javaslatok: a Gemini-terv bírálata

Ez a dokumentum a `docs/forras/gemini-terv-v1.odt` terv átvizsgálása: mi benne a jó, mi a hibás,
mi hiányzik, és mit javaslok helyette. A javaslatok már figyelembe veszik a tervezési kérdésekre
adott válaszaidat.

## 0. A döntéseid (ezekre épül a terv)

| Kérdés | Döntés |
|---|---|
| Fejlesztő | **az asszisztens (AI-ágens)**; a fő szempont a **stabilitás és a kis erőforrásigény** (Q1) |
| Célgép | **VECTOR**: Intel Core Ultra 9 275HX (24 szál), 32 GB RAM, RTX 5080 Laptop 16 GB + Intel Arc iGPU, 2 kijelző, 1 TB SSD — `09-CELHARDVER.md` |
| Motor | **Natív Rust mag + Qt 6 héj + CEF**, motor-adapter réteggel a későbbi motorokhoz |
| Motor-csatolás | Windows: natív ablak; Linux/Wayland: OSR — `11-OSR.md` |
| Erőforrás-viselkedés | **profilok** (Gyors / Kiegyensúlyozott / Takarékos / Ultra / Kézi), nem egyetlen kapcsoló (Q4) |
| Kártya elhagyásakor | **Lemezhibernálás**: mindent mentsen el, de RAM-ban semmit; lemezen több tíz GB is rendben |
| 10 perc | A motor megszűnik (a kártya visszaolvad a kezdőlapba) |
| 24 óra | **`keep` / `sweep` / `wipe` politika**: a bejelentkezés **alapból megmarad**, csak a melléktermékek törlődnek; a teljes törlés opció — `10-MUNKAMENET.md` |
| Kezdőlap | Egyszerű kártyarács **és** szabadon szerkeszthető weboldal, amibe a kártyák beilleszthetők; a kezdőlap bárhogy kinézhet |
| v1 hatókör | kártyarács + előnézet + keresés; élő nézet életciklus; konténerek; széf + TOTP + autofill; **világos / szürke / sötét** témák; memória-mérés és CI-kapu |
| Sötét mód | Nem elég egy módszer: **előre definiált sötétítési technikák** készlete kell |

---

## 1. Ami a Gemini-tervben jó, és marad

1. **Az „egyetlen élő nézet" elve.** Ez a terv magja, és helyes. A memória nagy része nem a böngészőben
   van, hanem az élő renderelőkben: egy oldal 80 MB (statikus cikk) és 500 MB (videós SPA) között lehet.
   Egyszerre egy élő motor a legnagyobb egyetlen memória-nyereség.
2. **Identitás-konténerek.** Helyes modell: fiókonként izolált tárolópartíció. Ez a többfiókos használat
   egyetlen helyes megoldása (nem inkognitó ablakok).
3. **SQLite + lemezes előnézeti képek.** Jó alap; kiegészítendő (lásd P11).
4. **Rendszerszintű titkosítás a széfhez** (Windows DPAPI / Linux Secret Service). Jó irány; a részletek
   viszont hiányoznak (P9).
5. **Háromzónás felület** (bal navigáció, felső vezérlősáv, központi munkaterület). Ésszerű; a felső sávot
   a minimumra kell venni, mert minden pixel a tartalomtól vesz el.

### 1.1. Amit a Gemini-tervből tudatosan kihagytunk

Nem minden eltérés hiba: az alábbi részeket **felváltotta** egy jobb megoldás, és ezt itt rögzítem, hogy
később ne tűnjön úgy, hogy kifelejtettük.

| Gemini-terv része | Miért nem maradt meg | Helyette |
|---|---|---|
| Kétszintű preload (browser UI ↔ főfolyamat, oldal-figyelő) | Electron-specifikus fogalom; a natív héjban a határ a Qt-héj és a Rust mag FFI-je, illetve a motor izolált világa | Rust mag + FFI + izolált világ (`02` 9.2.) |
| „Screenshot + görgetés" visszaállítás | elveszíti az űrlapokat, az SPA-állapotot, a betöltött tartalmat | lemezhibernálás MHTML + meta (`02` 5–6.) |
| A „lezárt kártya nulla bájtot foglal" állítás | pontatlan (lemez, gyorsítótár, GPU-textúra marad) | „nulla RAM-ot foglal" (`00` P10) |
| Azonnali cross-platform (Windows + Linux az első naptól) | a Wayland-beágyazás külön projekt; szétdobná az energiát | Windows a v1-ben, Linux a v0.3-tól, de hordozható kóddal (`ADR-006`) |
| „Mindenhol automatikusan bejelentkeztetés" mint kész tény | OAuth/passkey/mágikus link nélkül nem teljesíthető | belépési forgatókönyv + széf + TOTP + popup-kezelés (`02` 8.5., 9.5.) |

---

## 2. Amit javítani kell

### P1 — A választott alap (Electron) önmagában nem éri el a célt

**A terv mondja:** Electron + egy élő `WebContentsView`.
**Probléma:** Az Electron minden esetben magával hoz egy Node-főfolyamatot **és** egy saját UI-renderert
(Chromium), amin felül jön az oldal renderere, a GPU-, hálózati és utility folyamatok. Egy „üres" Electron
ablak reálisan ~250–350 MB privát memóriánál tart, és ez az alap **akkor is megvan, amikor egyetlen oldal
sincs nyitva**. Vagyis az „1 GB, és közben a rács nézet olcsó" ígéret Electron alatt nem tartható.
**Javaslat:** A döntésed helyes: **natív héj (Qt 6) + CEF**. A Qt-héj ~60–100 MB, a Rust mag ~10–20 MB,
és nincs külön Node-főfolyamat. Ezzel a kártyarács-nézet ~350–470 MB meleg motorral, és **~120–180 MB,
ha a motort teljesen leállítjuk** (ultra-low mód); egy átlagos oldallal ~450–620 MB, egy nehéz videós
oldallal ~800–1000 MB — az 1 GB tartható. Ezt **mérni kell a kód első hetében** (lásd `03-MEMORIA.md`).

### P2 — A „pillanatkép + görgetés" visszaállítás illúzió (a terv legnagyobb szakmai hibája)

**A terv mondja:** fagyasztáskor mentsük a görgetési pozíciót és egy képernyőképet; visszatéréskor
töltsük újra az oldalt és görgessünk oda.
**Probléma:** Ez pontosan a Chrome „Memory Saver" viselkedése, amit a felhasználók gyűlölnek. Amit elveszít:
- űrlapba írt, még el nem küldött szöveg (hosszú komment, e-mail, jegyzet);
- SPA-állapot (YouTube lejátszási lista, Gmail olvasatlan nézet, Twitter görgetett folyam);
- végtelen görgetésnél a betöltött tartalom (a `scrollTo(0, 15000)` egy üres oldalon landol);
- videó pozíció, szünet/lejátszás állapot;
- a JS-memóriában tartott munkamenet-tokenek (egyes SPA-knál ez „kijelentkezésnek" látszik).
**Javaslat:** A döntésed szerinti **lemezhibernálás**: kártya elhagyásakor teljes pillanatkép készül
lemezre — MHTML (a teljes DOM, beágyazott erőforrásokkal) + tárolók (cookie-kivétel: azok a konténerben
maradnak) + görgetés + űrlapállapot + médialejátszó pozíció. Visszatéréskor azonnali, offline is olvasható
visszaállítás, majd igény szerint élő újratöltés. Részletes specifikáció: `02-ARCHITEKTURA.md` 5. fejezet
(5.1–5.4) és 6. fejezet.
**Amit viszont fizikailag NEM lehet lemezre tenni** (ezt a tervnek is és a felhasználói ígéretnek is
kimondani kötelező):
- élő JavaScript-futásidejű állapot (React/Vue komponens-memória, eseménykezelő-hivatkozások);
- nyitott WebSocket/WebRTC/SSE kapcsolatok;
- folyamatban lévő, szerveroldali tranzakciók;
- futó animációk, `setInterval` számlálók.
Ezért a helyes ígéret: **„a látható és kitöltött állapot megmarad, a futó kapcsolatok nem"**.

### P3 — A 24 órás *teljes* törlés ellentmond az életben tartott bejelentkezéseknek

**A terv mondja:** 24 óra után teljes adattörlés = kijelentkezés.
**Probléma:** ez pontosan azt törölné, amit meg akarsz tartani (a Google/Facebook/saját oldal
bejelentkezését), és napi újrabejelentkezést, CAPTCHA-t, esetenként fiókzárolást hozna. Ráadásul a
bejelentkezés **nem RAM-kérdés**: a süti a motor lemezes partíciójában van, tehát életben tartani
memóriailag ingyen van (lásd `10-MUNKAMENET.md`).
**Javaslat (a követelmény pontosításával):** a 24 óra **háromszintű politika** legyen, kártyánként és
konténerenként:
1. **`keep`** — semmit nem töröl (csak a gyorsítótár LRU-t): a bejelentkezés megmarad. *Ajánlott alap a
   fontos szolgáltatásokra (Google, Facebook, saját oldal).*
2. **`sweep`** *(konténer-alap)* — gyorsítótár, `sessionStorage`, Service Worker regisztrációk, code cache
   törlése; a sütik és a tárolt adatok maradnak → **bejelentkezve marad**.
3. **`wipe`** — teljes origin-törlés = kijelentkezés. Megmarad **opcióként** (bank, egészségügy, munka).
A döntés indoklása és a mérés (mit veszítünk melyik esetben) a `10-MUNKAMENET.md`-ben.
**Kockázat, amit vállalni kell:** ha valaki a `wipe`-et választja, ott a napi újrabejelentkezés + a
CAPTCHA valós; ezt a felület jelzi, nem titkolja.
Megjegyzés a „nem alvó, nem szétkapcsolt kapcsolat" követelményedhez: ezt a terv **szó szerint** teljesíti —
nincs félholt, alvó fül; a kártya **vagy él, vagy lemezen van**, a kettő között csak egy rövid átmenet van
(`02-ARCHITEKTURA.md` 4. fejezet).

### P4 — A bejelentkezés-kezelés (átfogalmazva: munkamenet-gazdálkodás)

**A terv mondja:** jelszószéf + TOTP + űrlapfigyelő.
**Probléma:** (a) a „mindenhol automatikusan bejelentkeztetés" nem valósítható meg teljesen, és nem is ez a
valódi igény; (b) az életben tartott munkamenetek **költségét** senki nem méri; (c) a bejelentkezési út
három fontos darabja hiányzik:
- **OAuth/SSO popupok** („Jelentkezzen be Google-fiókkal") — **valódi, külön ablak** kell, ugyanabban a
  partícióban, különben nem öröklik a sütiket;
- **passkey / WebAuthn** — részletes elemzés: `12-PASSKEY.md` (a CEF-alapú böngésző egyik legnagyobb
  nyitott kockázata);
- **mágikus linkes belépés** (e-mailben küldött link) — kattintható link-fogó vagy IMAP (v2);
- **hitelesítőadat-import**: a Chrome-ból való süti-import **2026-ban nem működik** (Chrome 127+ „app-bound"
  sütititkosítás; a sütik visszafejtéséhez a Chrome aláírását ellenőrző emelt szintű szolgáltatás kell, amit
  harmadik fél nem tud legálisan utánozni). Firefoxból/szöveges exportból viszont igen.
**Javaslat:** „Bejelentkezési forgatókönyv" (login runbook) fogalom: kártyánként/konténerenként egy
deklaratív lépéssor (`url → mező kitöltése → TOTP → várható elem`), amit a böngésző végrehajt, és ami
meghiúsulás esetén átadja a felhasználónak. Ez az, ami a „mindenhol automatikusan bejelentkeztet"
ígéretből megvalósítható. Részletek: `02-ARCHITEKTURA.md` 9. fejezet.

### P5 — A többmotoros követelmény kimaradt a tervből

A Gemini-terv Electronra épül, az Electron pedig **csak Chromium**. A „választható, opcionális
böngészőmotorok" követelményed így nem teljesül. A terv válasza: a motor **külön folyamatfa**, egy
szűk adapter-interfész mögött (7 primitív: létrehozás, navigálás, pillanatkép, szkriptfuttatás izolált
világban, görgetés, kép, megszüntetés). Az első adapter a CEF; a második jelölt a **WebKitGTK** (Linuxon
jelentősen kisebb lábon él, és jó „light mód" lehet), a harmadik a **WebView2** (Windows, rendszerrel
száll). Gecko nem beágyazható, Servo/Ladybird 2026-ban még nem termékképes — ezeket a terv kizárja.
Részletek: `02-ARCHITEKTURA.md` 16. fejezet + ADR-002.

### P6 — Az előnézet-frissítés ellentmond a „nincs háttérmotor" elvnek

**A terv mondja:** a kártyarács mindig az oldal utolsó állapotát mutatja.
**Probléma:** ha egy kártya hetekig nem nyílik meg, az előnézete elavul — a „modern, aktuális előnézet"
ígéret viszont élő betöltést igényel, ami motort indít. Ezt a terv nem oldja meg.
**Javaslat:** **előnézet-ütemező**, ami ugyanazt az egyetlen motort használja újra, soha nem indít másodikat:
csak tétlen gépnél, csak hálózati töltőn (opcionális), kártyánként max. 1 frissítés / 7 nap, egyszerre
egy kártya, 8 másodperces időkorlát, utána azonnali hibernálás. Ez ~500 kártyánál is max. napi néhány
másodpercnyi motorhasználat. Emellett a kártya megnyitása mindig frissít előnézetet (ingyen).

### P7 — A képernyőkép-alapú előnézet nem elég a „weboldal formátumú" könyvtárhoz

**A terv mondja:** tömörített statikus előnézeti kép.
**Probléma:** a kép statikus és olvashatatlan; a „modern könyvjelzőtár" élményhez kell szöveg, cím, ikon,
címkék, jegyzet, és ami a legtöbbet adja: **keresés a mentett oldalak tartalmában** — ez a lemezre
hibernált MHTML-ből kinyerhető szövegen FTS5-tel ingyen megvalósítható.
**Javaslat:** kártyánként: WebP előnézet (kicsi, gyors) + MHTML pillanatkép (offline olvasás) +
kivont szöveg (keresés) + metaadat. Ez a három együtt adja a „tényleg használható" könyvtárat.

### P8 — A sötét mód egyetlen módszere kevés (és a szürke mód teljesen hiányzik)

**A terv mondja:** natív `prefers-color-scheme` jelzés + stílus-injektálás.
**Probléma:** a valóságban az oldalak ~40–60%-a nem elég jó sötét módban, az injektálás sok helyen törik
(árnyék-DOM, canvas, cross-origin stíluslapok, képek invertálása).
**Javaslat:** **sötétítési technika-létra** (nem egy módszer):
`1` natív jelzés → `2` generált stíluslap (Dark Reader-szerű motor) → `3` invertálás + kép-visszainvertálás →
`4` kényszerített kontraszt (`forced-colors`) → `5` dimmelő réteg (fényerő) → `6` szürke/színtelen „olvasó" mód.
A létra működését **mérni kell, nem hinni**: minden kártya előnézetéből kiszámítjuk az átlagos fényerőt
(luminanciát), és ha a „sötét" mód után az oldal még világos, a böngésző fellép a következő technikára.
Ez önellenőrző, oldalanként tanul, és pontosan azt adja, amit kértél: előre definiált technikák, nem
kompromisszum. A **szürke mód** (a három alapséma egyike) a felületen kívül az oldalakra is alkalmazható
(színtelenítés + enyhe tompítás) — ez a leghatékonyabb „olvasásra hangolt" megjelenés.
Részletek: `02-ARCHITEKTURA.md` 10. fejezet.

### P9 — A széf biztonsága alulspecifikált

**A terv mondja:** DPAPI / KDE Wallet + SQLite.
**Probléma:** (a) a DPAPI a **Windows-felhasználóhoz** köt, nem az alkalmazáshoz — egy másik program is
visszafejtheti ugyanazon a felhasználón; (b) nincs mesterjelszó, tehát a széf nem hordozható és nincs
védelem abban az esetben, ha a felhasználói profil kompromittálódik; (c) nincs kulcsrotáció; (d) a
kitöltő felület és a jelszó útja az oldalig nincs megtervezve.
**Javaslat:** kétszintű kulcs: **DEK** (adatkulcs, véletlen) + **KEK** (kulcsot titkosító kulcs) a rendszer
kulcstartójából, **opcionális mesterjelszóval** (Argon2id, RFC 9106) — ez utóbbi hordozhatóvá és
exportálhatóvá teszi a széfet. A jelszó soha nem kerül a fővilágba: csak izolált világban, zárt Shadow DOM
kitöltőben jelenik meg, és az oldal JS-e nem tudja kiolvasni. Részletek: `02-ARCHITEKTURA.md` 9. fejezet.

### P10 — A „zéró bájtot foglal a lezárt kártya" állítás hamis

A memória valóban felszabadul, de: a lemezre írt pillanatkép, a V8 kód-gyorsítótár, a Chromium saját
gyorsítótárai és a GPU-textúrák egy része megmarad. A helyes állítás: **„a kártya nulla RAM-ot foglal,
lemezen pedig ~1–10 MB-ot"**. Ezt a dokumentációban és a marketingben is így kell megfogalmazni, különben
a felhasználó mérni fog, és nem fog egyezni.

### P11 — Az adatmodell négy táblája kevés

A terv négy entitása (munkaterület, konténer, kártya, hitelesítő adat) nem fedi le a valós működést.
Kell még: **pillanatkép** (verziózott, tartalom szerint címzett), **oldal-recept** (sötétítési technika,
engedélyek, zoom, kivételek), **címke**, **bejelentkezési forgatókönyv**, **napló/esemény**, **média**,
**beállítás**, **séma-verzió**. Részletes DDL: `02-ARCHITEKTURA.md` 7. fejezet.

### P12 — Nincs mérési terv és nincs elfogadási kritérium

A memóriacél egy mérőszám, tehát kell hozzá mérési módszer (privát munkakészlet, nem RSS-összeg — az
RSS a megosztott lapokat többször számolja), automatizált mérőpad, és **CI-kapu**, ami megbuktatja a
buildet, ha a büdzsé túllép. Enélkül a cél három hónap múlva már nem számon kérhető. Részletek:
`03-MEMORIA.md`.

### P13 — A Wayland beágyazás nincs kezelve (Linux-kockázat)

Natív ablak beágyazása (idegen folyamat ablaka a saját ablakunkban) Windows-on és X11-en rutin
(`SetParent`, illetve `XReparentWindow`), **Wayland alatt viszont csak az `xdg-foreign` protokollal**
megy, amit nem minden kompozitor támogat egyformán. Ezért a tervben **két csatolási mód** van:
natív ablak-beágyazás (gyors, Windows/X11) és **OSR** (offscreen renderelés, a képkockákat mi kompozitáljuk
— Wayland-biztos, de drágább CPU-ban). Ez egyben a többmotoros csatolás közös útja is: a motor folyamata
mindig külön, a képet vagy ablakként, vagy képkockacsatornán keresztül kapjuk.

### P14 — Hiányzó asztali böngésző-alapfunkciók

Ezek nélkül a böngésző nem használható napi eszközként, és a terv egyiket sem említi: letöltéskezelő és
letöltési előzmény; keresés az oldalban (Ctrl+F); nagyítás; nyomtatás/PDF-exportálás; PDF-megjelenítés;
kép-a-képben (PiP) videó; popup- és `target=_blank` kezelés; összeomlás utáni helyreállítás; billentyűzetes
navigáció és képernyőolvasó-támogatás; engedélykezelés (kamera, mikrofon, értesítés) konténerenként;
proxy/VPN konténerenként; hirdetés- és követésblokkolás; automatikus frissítés; import/export (könyvjelzők,
munkamenetek, jelszavak); keresőmotor-választás; privát kártya. A terv ezeket a `04-UTEMTERV.md`-ben
fázisokhoz rendeli, hogy ne lehessen „elfelejteni".

### P15 — A tervezett felület (kártyarács) maga is memóriacsapda lehet

500 kártya 400×250-es dekódolt előnézettel = ~200 MB csak a képekből (a naiv, virtualizálatlan
megvalósításnál). Ezt a terv nem veszi figyelembe.
**Javaslat:** virtualizált rács (csak a látható kártyák DOM-ban), kicsi (pl. 260×150) WebP előnézetek,
`loading="lazy"`, dekódolt bittérkép-plafon, és a kezdőlap-renderer **ugyanaz az egy renderer**, mint az
élő oldal — mivel egyszerre úgyis csak egy fut.

---

## 3. Ami a legnagyobb kockázat (és amit figyelni kell)

| # | Kockázat | Hatás | Kezelés |
|---|---|---|---|
| R1 | A CEF + Qt + Rust integráció több hónapot visz el a látható eredményig | Megszakad a lendület | Fázis 0: 2–4 hetes időmérő prototípus, ami már méri a RAM-ot; utána döntés |
| R2 | Egy nehéz oldal (YouTube 1080p) önmagában ~500 MB | Az 1 GB nehezen tartható | Médiaminőség-korlát, büdzsé-vezérlő, „nehéz oldal" jelzés a felületen |
| R3 | A napi (24 órás) adattörlés CAPTCHA-t és újrabejelentkezést hoz | Bosszantó élmény | Kártyánkénti/konténerenkénti felülírás + széf-vezérelt újrabejelentkezés (P3) |
| R4 | Az injektált dark mode és az autofill oldalanként törik | Folyamatos karbantartás | Technika-létra + luminancia-ellenőrzés + regressziós teszt-oldalkészlet (P8) |
| R5 | A lemezhibernálás lassú lehet nagy oldalakon | Lassú kártyaváltás | Aszinkron mentés animáció mögött, inkrementális/gyorsított mentés, gyors visszaállítás előnézetből |
| R6 | Egy személyes projektnél a teljes vízió 2–3 év | Elvérzés | Fázisok, minden fázis önállóan használható; „használható a saját napi böngészőként" már a 2. fázis végén |

---

## 4. Amit nem javaslok, és miért

**„Option 0": ne építs böngészőt.** Őszintén: a víziód 80%-a megkapható Firefox + Multi-Account Containers
+ Auto Tab Discard + egy saját kezdőlap-oldal kombinációjával, nulla fejlesztéssel. Ezt nem azért írom,
hogy lebeszéljelek, hanem hogy tudd: **a saját böngésző akkor nyer, ha olyat tud, amit a Firefox nem** —
nevezetesen a lemezhibernálásos, offline olvasható kártyalét, a beépített többfiókos széfet és a
szerkeszthető kezdőlapot egyben. A terv ezért ezekre a megkülönböztető képességekre optimalizál, és
nem versenyez a Chromium sebességével vagy a weblap-kompatibilitással.

**Ne kezdjük a többmotoros támogatással.** Az adapter-interfész legyen meg az első naptól, de a második
motor csak akkor kerüljön be, ha az első (CEF) már stabil — különben két instabil integrációt kell
egyszerre karbantartani.

**Ne legyen bővítményrendszer v1-ben.** A bővítmények (Chrome extension API) memóriát és karbantartást
hoznak; helyette felhasználói szkriptek + szabályok kártyánként, később eldönthető.
