# 06 — Ötletlista (nem kötelező, priorizálva)

Olyan lehetőségek, amelyek a követelményeiden túl a termékből **következnek** (a lemezhibernálás és a
kártyamodell miatt szinte ingyen vannak), de nem részei a v1 szerződésnek. Sorrend = javasolt bevezetési
időpont.

## A. Közvetlenül a lemezhibernálásból következnek (F1–F2 után, olcsók)

1. **Időutazás a kártyán.** Mivel minden fagyasztás pillanatképet ír, egy kártyánál visszalapozhatsz:
   „mit láttam 3 napja, 2 hete?" — a snapshot-idővonal egy csúszkával. Ez olyan dolog, amit egyetlen
   mai böngésző sem tud, és nálad *melléktermék*. Költség: szinte nulla (a blobok már ott vannak),
   csak felület + kvóta-kezelés. **Ez a legerősebb egyedi funkció-jelölt.**
2. **Offline olvasás („konzerv").** Az MHTML-ből repülőn, vonaton, lassú hálón is olvasható a kártya —
   hálózat nélkül, a mentett állapotban. Költség: nulla (R1 útvonal).
3. **Keresés a mentett oldalak tartalmában.** „Melyik cikkben írtam az Adatkezelésről?" — a kivont
   szövegen FTS5-tel. Költség: alacsony (a szövegkinyerés a mentéskor történik).
4. **„Folytasd az olvasást" sor** a kezdőlapon: a legutóbb görgetett, félbehagyott cikkek, a mentett
   pozícióból nyitva. Költség: nulla (a görgetés már mentve van).
5. **Kártya-verziók törlése/«ez a verzió maradjon»** — a felhasználó eldöntheti, mely pillanatképek
   maradjanak meg («őrizd meg ezt a receptet»). Költség: kicsi, a kvóta-kezelés része.

## B. A kártyamodellből következnek (F3 után)

6. **Bejelentkezés-állapot jelző.** A kártyán egy kis lakat: zöld = bejelentkezve, sárga = hamarosan
   lejár, piros = kijelentkezve. Ez azonnal érthetővé teszi az adatvédelmi modellt.
   *(Ez az F3-ban már nem ötlet, hanem része a tervnek: `10-MUNKAMENET.md`.)*
7. **„Konzerválás" gomb.** A kártyán egy kattintás: azonnali lemezhibernálás, motor nélkül — hasznos
   munka után, amikor „már nem kell, de ne vesszen el".
8. **Kártya-gyűjtemény mint megosztható csomag.** Egy téma kártyái exportálhatók egy fájlba
   (`.antikolt-collection`), és más importálhatja — snackbar: nincs felhő, nincs fiók.
9. **Munkamenet-sablonok.** „Hétfő reggeli rutin": 5 kártya sorrendben, egy kattintással egymás után
   megnyitva (mindig csak egy élve, a többi lemezen) — ez a több-kártyás munkafolyamat memóriabiztos
   változata.
10. **Memória-átláthatóság.** Egy kis panel: „ez az oldal 430 MB-ot használ; a kártyarács 90 MB" —
    a felhasználó látja, mit miért tesz a böngésző. Ez az ígéret bizonyítéka, és tanítja a használatot.

## C. Későbbi, nagyobb ötletek (F6 után, külön döntés)

11. **Csendes órák.** Naplemente után a felület automatikusan szürke/sötét módba vált, az oldalak is
    tompulnak. Költség: kicsi (a téma-motor már tudja).
12. **Gyorsparancs-paletta (`Ctrl+K`)** — kártyakeresés, konténerváltás, munkaterület-váltás, „nyisd meg
    ezt a kártyát X konténerben". Költség: közepes (bridge + felület).
13. **Konténerenkénti proxy/VPN** — munka konténer céges VPN-en, magán konténer otthon. Már a v1
    architektúrájában benne van (`container.proxy_json`), csak felület kell.
14. **Saját kezdőlap-sablonok galériája** („minimal", „magazin", „műszerfal", „olvasó") — a kezdőlap
    HTML, tehát a sablonok is azok; egy sablon = egy mappa.
15. **Titkosított többgépes átvitel** — a teljes adatbázis (kártyák, széf, snapshotok) exportálható egy
    titkosított archívumba, pendrive-on átvihető. Nincs felhő, de van hordozhatóság.

## Amit tudatosan NEM javaslok

- **Felhő-szinkronizáció** — a projekt identitása a helyi adat; a szinkron egy külön termék lenne.
  (A 15. pont a kompromisszum: fájlos, titkosított átvitel, felhasználói felelősséggel.)
- **Élő csempék / iframe-ek a kezdőlapon** — második renderelőt indítanának, és megsértenék az I1
  invariánst. A „élő" érzést a friss előnézet és az azonnali nyitás adja.
- **Beépített AI-asszisztens** — alapból felhőt feltételezne; ha később kell, csak helyi modellel vagy
  saját kulccsal, kikapcsolt alapállapottal.
- **Bővítménybolt** — lásd ADR-016.
