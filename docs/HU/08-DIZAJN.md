# 08 — Dizájn alapok (design tokenek)

A „modern megjelenés" nem ízlés kérdése, hanem rendszer: közös tokenek, amelyeket a Qt/QML felület
**és** a kezdőlap (HTML/CSS) ugyanúgy használ. A **szürke (grafit) az alapértelmezett** téma, mert ez a
legkevésbé megerőltető nappal és este is — ezért ez a „kompromisszum nélküli" alap (a felhasználói döntés
szerint).

## 1. Négy felületi téma

| Token | `light` | `grey` (alap) | `dark` | `amoled` |
|---|---|---|---|---|
| `--bg` | `#F7F7F5` | `#2E3134` | `#1C1D1F` | `#000000` |
| `--surface` | `#FFFFFF` | `#35383C` | `#232527` | `#0A0A0A` |
| `--surface-raised` | `#FFFFFF` | `#3C4045` | `#2A2C2F` | `#121212` |
| `--text` | `#1A1A18` | `#EDEDEA` | `#E9E9E7` | `#EDEDED` |
| `--text-muted` | `#5F5F5A` | `#A8AAAD` | `#9A9C9F` | `#9A9A9A` |
| `--border` | `#E1E1DC` | `#44484D` | `#33363A` | `#1E1E1E` |
| `--accent` | `#2F6FEB` | `#7FB0FF` | `#6EA8FF` | `#8AB8FF` |
| `--danger` | `#C23B32` | `#FF8A80` | `#F27A72` | `#FF8A80` |
| `--ok` | `#1E7D4F` | `#7BD8A5` | `#6FD3A0` | `#7BD8A5` |

**Kontraszt-követelmény:** a szövegre WCAG AA (≥ 4,5:1), a nagy szövegre és az ikonokra ≥ 3:1 — ezt
minden témára **automatikusan mérjük** (a CI-ban, egy kontraszt-számító teszttel), nem szemre.

Konténer-színek (identitás jelölése, 8 fix szín, minden témában ugyanaz):
`#E0533D`, `#E3A008`, `#2E9E5B`, `#2F6FEB`, `#8E44AD`, `#D6336C`, `#0FA3A3`, `#6B7280`.

## 2. Tipográfia

- Betűtípus: **Inter** (a magyar ő/ű/Ő/Ű karakterek teljes támogatásával) — a kezdőlapon és a Qt felületen
  ugyanaz, hogy a kártya és a felület egy nyelvet beszéljen.
- Méretek: `11` (meta), `13` (alap UI), `15` (kiemelt), `17` (kártyacím), `22` (szakaszcím), `28` (nagy cím).
- Sormagasság: 1,45; betűköz: 0; a címeknél `-0.01em`.
- A magyar szövegekben **kötőjel és gondolatjel** helyes használata (a felületi szövegek i18n-fájlban).

## 3. Térköz, forma, mozgás

| Token | Érték |
|---|---|
| `--space-1..6` | 4, 8, 12, 16, 24, 32 px |
| `--radius-card` | 10 px |
| `--radius-panel` | 14 px |
| `--radius-control` | 8 px |
| `--shadow-1` | `0 1px 2px rgba(0,0,0,.18)` |
| `--shadow-2` | `0 6px 24px rgba(0,0,0,.28)` |
| `--motion-fast` | 120 ms (`ease-out`) |
| `--motion-base` | 250 ms (kártya-morfolás, lásd `02` 4.4.) |
| `--motion-panel` | 200 ms (`cubic-bezier(.2,.8,.2,1)`) |

`prefers-reduced-motion` esetén minden animáció 0 ms-ra vált (és a kártya-morfolás áttűnésre).

## 4. Kártya-méretek és a rács

| Méret | Kártya | Előnézet | Használat |
|---|---|---|---|
| S | 180 × 110 | 164 × 92 | sűrű lista, „gyakran használt" |
| M | 260 × 150 | 240 × 130 | alapértelmezett |
| L | 380 × 220 | 360 × 200 | kiemelt, „folytasd az olvasást" |

Rácsköz: 16 px; a rács **virtualizált** (csak a látható kártyák élnek a DOM-ban — `03` 6. fejezet).
Az előnézet képaránya **16:10**, a kártya alja felé enyhe sötétedő átmenet, hogy a cím mindig olvasható
legyen (a kép tartalmától függetlenül).

## 5. A kártya anatómiája

```
┌─────────────────────────────┐
│  [előnézeti kép 16:10]      │  ← WebP, 260×150
├─────────────────────────────┤
│ ● Cím (max 2 sor, csonkolva)│  ← konténer-szín pont
│ domain.hu            🔒     │  ← 🔒 = bejelentkezve / ⏳ = hamarosan lejár
└─────────────────────────────┘
```

Állapot-jelzések: `🔒` bejelentkezve, `⏳` 24 órán belül lejár, `🔓` kijelentkezve, `📌` kitűzve,
`💤` archivált, `⏳→` törlés folyamatban.

## 6. Vezérlősáv (a lehető legvékonyabb)

Balról: vissza · előre · újratöltés · **⌂ kezdőlap** · cím/kereső mező · konténer-jelvény (szín + név) ·
téma-váltó (világos/szürke/sötét) · széf. Jobbról: memória-jelző (opcionális), menü.
Magasság: 40 px; ikonok 18 px; a címmező kapja a maradék helyet.

**Ahol egy gomb állapotot is jelöl, ott az ikon a felület, a szó a tooltip** (2026-11-18): a négy felületi
témát egy-egy generált ikon képviseli (a téma háttere + szövegszíne), a megnevezés a tooltipben és a
képernyőolvasó-névben van; az oldal-sötétítés holdsarló-ikonja a menüből választható, és a **kitöltése mutatja
az érvényes fokozatot** (üres = kikapcsolva, fekete = sötét, szürke = sötét szürke, meleg = sötét meleg).

## 7. Akadálymentesség

- Teljes billentyűzetes navigáció; fókuszgyűrű minden témában látható (2 px, `--accent`).
- Képernyőolvasó-címkék minden ikonra (`accessibleName`).
- Ne legyen információ kizárólag színben (a konténer-jelzésnél a szín mellé betű is kerül).
- A kártyarács fókuszsorrendje balról jobbra, fentről lefelé; `Home`/`End` a sorok elejére/végére.

## 8. Amit a felület soha nem tesz

- **Nem nyit indokolatlan második ablakot, és nem is tart életben egyet.** Egy böngészőnek egy látható ablaka
  van (a kártya ebbe van ágyazva, nem külön ablak), a motor- és magfolyamatok rejtett csöveken beszélnek, és a
  tálcaikon nem ablak, hanem jelzés. Ha egy indítás mégis plusz ablakot hozna létre (a Windows
  konzol-subsystem programoknak ad egyet), a program **azonnal eldobja** — nem elrejti, nem hagyja ott
  (`DropOwnConsoleWindow`, `main.cpp`). Ez a felhasználói szabály: „szigorúan tilos bármilyen indokolatlan
  plusz ablakot megnyitni és főleg életben tartani" (2026-11-18).
- Nem villog, nem ugrál (a kártya-morfolás a kivétel, de az is 250 ms).
- Nem tesz fel kérdést, amire a válasz megjósolható (pl. „biztosan megnyitod?" — a megnyitás olcsó).
- Nem töm tele feliratot: a kártya címe + domain + állapot elég.
- Nem rejti el a memóriát (a jelző megmutatja, mennyi az éppen használt érték — ez a bizalom része).
- Nem hazudik a témáról: a vezérlősáv választása a **saját** felületeket (ablak, rács) azonnal átfestit, a
  weboldalakat pedig vagy a saját sötét témájuk, vagy a felhasználó által kért „Sötét oldalak" kapcsoló
  (ADR-044) — a `--themetest` mindkettőt méri, pixelenként.

## 9. A sötét oldalak (ADR-044/ADR-045)

A `light`/`grey`/`dark`/`amoled` a **termék** felülete; egy weboldal világos marad, ha nincs saját sötét
témája. Erre való a vezérlősáv **holdsarló-ikonja** (alapból kikapcsolva, megjegyzett), aminek a menüjében
négy fokozat van:

| Fokozat | Mit tesz a lappal | Mért fényesség (fehér próbaoldal) |
|---|---|---|
| `off` (Kikapcsolva) | semmit | 251 |
| `invert` (Sötét) | invertálás + `hue-rotate(180deg)` | 4 |
| `dim` (Sötét szürke) | ugyanaz + `contrast(0.72)` | 39 |
| `warm` (Sötét meleg) | ugyanaz + `contrast(0.78) sepia(0.35)` | 33 |

A motor egy **nevesített** stíluselemet szúr a lapba, a módváltás lecseréli, a kikapcsolás eltávolítja —
újratöltés nélkül. Nem keverendő az „Oldal-téma: auto/dark/light" gombbal, ami csak *megmondja* a lapnak, mit
szeretne a rendszer.
