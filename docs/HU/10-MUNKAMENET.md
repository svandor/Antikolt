# 10 — Munkamenet-gazdálkodás és a bejelentkezés költsége

> **A követelmény pontosítása (a te megfogalmazásod alapján).** Nem az a cél, hogy a böngésző
> „mindenhol automatikusan bejelentkeztessen". Az a cél, hogy **a fontos bejelentkezések (Google,
> Facebook, saját oldal, amit használsz) életben maradjanak, lehetőleg kis memóriaigénnyel — és legyen
> mérhető, hogy ez mennyibe kerül, illetve mit veszítünk, ha nem tartjuk életben.**

Ez a dokumentum ezt a követelményt bontja ki: mi kerül memóriába, mi kerül lemezre, mit lehet mérni, és
melyik döntés mit veszít el.

## 1. A legfontosabb tény: a bejelentkezés nem RAM-ban él

A bejelentkezés lényegében **süti + tárolt token** (cookie, localStorage, ritkábban IndexedDB). Ezek a
motor **lemezes** partíciójában vannak (cookie-adatbázis, LevelDB-k). Amikor a motor nem fut:

| Mi | Hol van | RAM-költség |
|---|---|---|
| Sütik (login token) | motor partíció, lemez (SQLite) | **0 bájt** |
| localStorage / IndexedDB | motor partíció, lemez | **0 bájt** |
| Jelszó, TOTP | széf (SQLite, titkosítva) | **0 bájt** (zárva) |
| Előnézet, pillanatkép | blob-tár | **0 bájt** |

**Következmény:** 50–200 életben tartott bejelentkezés memóriája **gyakorlatilag nulla**, és néhány MB
lemez. Nem a bejelentkezések viszik el a RAM-ot — hanem a **futó oldalak**. A Chrome 3-4 GB-ját nem a
bejelentkezett fiókok okozzák, hanem a 30 nyitott fül renderelője.

Ezért a helyes ígéret: **„a bejelentkezéseid megmaradnak, mert nem kell őket a memóriában tartani"**.

## 2. Akkor mi visz el memóriát? (a valódi trade-off-ok)

| Tényező | Költség | Megtartható? |
|---|---|---|
| **Egy megnyitott, bejelentkezett oldal** (Gmail, Facebook) | 250–600 MB | csak addig, amíg nézed; utána lemezre |
| **Meleg motorház (T1)** — hogy a kártya azonnal nyíljon | 180–280 MB fix | profil kérdése (Takarékos: kikapcsolva) |
| **Service Workerek a hibernált oldalakon** | néhány MB / origin + hálózat | **alapból kikapcsolva** hibernált kártyáknál |
| **Háttérben életben tartott kapcsolat** (chat, WebSocket, video) | 150–400 MB | csak „kitűzött" kártyánál (dokumentált kivétel) |
| **Süti-/tárolóadat növekedése** | ~1–50 KB / origin, néhány MB összesen | igen, gyakorlatilag ingyen |
| **Előnézet-frissítés** | ideiglenesen +80–200 MB, amíg fut | ütemezve, kikapcsolható |

**Amit tehát meg kell mérni, nem a „bejelentkezések száma", hanem:** hány oldal fut, meleg-e a motor,
futnak-e Service Workerek, és van-e kitűzött háttérkapcsolat.

## 3. A 24 órás szabály újraértelmezése (fontos változás)

A korábbi válaszod („24 óra után teljes cookie-/adattörlés = kijelentkezés") **ellentmond** a mostani
kérésnek (a fontos bejelentkezések maradjanak életben). A feloldás: a 24 óra **nem a bejelentkezést**
törli, hanem a **melléktermékeket**. Három szint van, kártyánként/konténerenként választhatóan:

| Politika | Mit töröl 24 óra után | Mit tart meg | Mikor való |
|---|---|---|---|
| **`keep`** *(új alapértelmezés)* | semmit (csak a gyorsítótár LRU-t) | mindent, a bejelentkezést is | Google, Facebook, saját oldal, fórumok |
| **`sweep`** | gyorsítótár, `sessionStorage`, Service Worker regisztrációk, code cache | sütik, localStorage, IndexedDB → **bejelentkezve marad** | a legtöbb oldal |
| **`wipe`** | mindent az originra (kijelentkezés) | csak a széf jelszava/TOTP-je | bank, egészségügy, munkahelyi rendszer |

- Alapból `sweep` a konténer-szintű alapérték, a gyakran használt kártyák `keep`-pel jelölhetők.
- A `wipe` a korábbi választásod — **megmarad, mint opció**, de már nem alapértelmezés.
- A politikát a felület méri is: minden kártyán látszik, mi a szabály és mi volt az utolsó törlés.

## 4. Munkamenet-ledger: „legyen mérhető, mit veszítünk"

Minden munkamenetről (konténer × origin) a mag nyilvántart egy rekordot, és a felület **Munkamenetek**
panelén ez látszik:

```
┌──────────────────────────────────────────────────────────────────────┐
│ Munkamenetek                                       Összesen: 23 aktív │
│ ─────────────────────────────────────────────────────────────────────│
│ ● Google — munka            ✔ bejelentkezve   ma 08:12   1,4 MB lemez │
│   RAM most: 0 MB · nyitva 41× · politika: keep · „ha törlöm: új login" │
│ ● Facebook — magán         ✔ bejelentkezve   2 napja     3,1 MB lemez │
│   RAM most: 0 MB · 8 cookie lejár 3 nap múlva                          │
│ ● sajatoldal.hu            ✔ bejelentkezve   tegnap      0,2 MB lemez │
│ ● bank (konténer: pénz)    ⏳ lejárt         —            0,4 MB lemez │
└──────────────────────────────────────────────────────────────────────┘
   Motor: hideg (0 MB) · meleg motor ehhez: +210 MB · előnézet: 12 friss
   [ Hideg motor ]  [ Meleg motor ]  [ Munkamenet törlése… ]
```

Rögzített adatok munkamenetenként:

| Mező | Jelentés |
|---|---|
| `state` | `logged_in` / `logged_out` / `expired` / `unknown` (az utolsó betöltés alapján) |
| `cookie_count`, `cookie_bytes` | a sütik száma és mérete (a partíció lemezén) |
| `storage_bytes` | localStorage + IndexedDB + Cache Storage mérete |
| `sw_count` | regisztrált Service Werkerek száma |
| `last_seen`, `last_verified` | utolsó megnyitás / utolsó igazolt bejelentkezés |
| `live_ram_bytes` | a mért RAM, ha éppen fut (0, ha dormant) |
| `policy` | `keep` / `sweep` / `wipe` / `wipe_on_lock` |
| `loss_if_wipe` | mit veszítesz (új jelszó + TOTP, esetleg CAPTCHA, 2FA-újrakötés) |

**Az „elveszítjük" tábla** — ezt a felület is mutatja, nem csak a dokumentáció:

| Ha ezt tesszük | RAM | Lemez | Elveszítjük |
|---|---|---|---|
| 25 bejelentkezés, mind dormant, hideg motor | ~0 MB | ~20 MB | semmit (a nyitás 0,6–1,2 s) |
| Ugyanaz, meleg motorral (T1) | +180–280 MB | ~20 MB | semmit, csak RAM-ot (nyitás 0,2–0,4 s) |
| 1 chat-kártya háttérben életben | +150–400 MB | ~5 MB | nem veszítünk, de *szabályt sért* (I1 kivétel) |
| SW engedélyezve minden bejelentkezett originnek | +5–20 MB/origin + hálózat | — | nem veszítünk, de a háttérforgalom nő |
| 24 órás `wipe` mindenhol | 0 MB | 0 MB | **napi újrabejelentkezés, CAPTCHA, fiókzárolás kockázata** |
| 24 órás `sweep` mindenhol | 0 MB | ~20 MB | semmit, csak a gyorsítótár ürül |

## 5. „Tartsd életben" (opt-in, mérhető)

Két külön dolog, amit a felhasználó kérhet kártyánként:

1. **Kitűzés (pin)** — a kártya nem hibernál automatikusan. Költség: az adott oldal RAM-ja, folyamatosan.
   Csak akkor ajánljuk, ha tényleg kell (zene, chat, hosszú űrlap).
2. **Munkamenet-éltetés (keep-alive)** — időnkénti könnyű kérés az adott konténer sütijeivel, hogy a
   szerveroldali munkamenet ne járjon le (bankok, rövid lejáratú tokenek). Költség: ~0 RAM, 1 kérés /
   15–60 perc, kis hálózat. **Alapból kikapcsolva**, mert (a) felesleges forgalom, (b) egyes oldalak
   „bot"-nak nézhetik, (c) adatvédelmi kérdés. Csak a felhasználó által megjelölt kártyákon fut, és
   naplózva van („ma 14:00-kor éltettem, 200 OK").

## 6. Ami továbbra sem megy (dokumentált veszteség)

- **Háttérben futó chat/értesítés** csak kitűzéssel — és akkor az a kártya RAM-ot foglal (I1 kivétel).
- **A szerveroldali munkamenet lejáratát nem tudjuk megakadályozni**, ha az oldal nem ad frissítő tokent;
  ilyenkor a runbook újra bejelentkezik (ha a széf tudja), vagy a felhasználó.
- **Passkey nélkül** egyes oldalak a jelszavas utat kínálják fel — ez a `12-PASSKEY.md` témája.
- **A „be van jelentkezve?" állapot heurisztika**: a böngésző nem tudja biztosan, csak abból következtet,
  hogy az oldal betöltése után van-e belépési űrlap vagy „kijelentkezés" link. Ezért `unknown` állapot is
  létezik, és a felület ezt őszintén mutatja.

## 7. Következmények a tervre (röviden)

- A széf és a konténerek maradnak, de a hangsúly a **munkamenet-nyilvántartáson** van (új tábla:
  `session`), nem az „automatikus bejelentkezésen".
- A `wipe` politika opció lesz, a `keep`/`sweep` az alap — ez a 24 órás szabály új értelme.
- A Munkamenetek panel a széf mellett a fő „identitás" felület.
- A mérés része: RAM (dormant vs live), lemez, és a „mit veszítünk" lista — a `03-MEMORIA.md` és a
  `perf/` riportok kiegészítéseként.
