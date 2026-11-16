# Antikolt kezdőlap (`antikolt://home`)

Ez a böngésző **kezdőlapja**: egy helyi HTML-oldal, amely a mentett oldalakat kártyákként jeleníti meg.
Nem könyvjelző-sáv — a felhasználó bármikor lecserélheti a sajátjára (lásd
[`../docs/HU/02-ARCHITEKTURA.md`](../docs/HU/02-ARCHITEKTURA.md) 11. fejezet).

## Kipróbálás böngésző nélkül

Nyisd meg az `index.html`-t egy böngészőben. Ha nincs `window.antikolt` bridge (mert nem az Antikolt
motorjában fut), a lap a beépített **mock bridge**-dzsel dolgozik: 28 minta-kártya, működő keresés,
téma-váltás, billentyűzetes navigáció. Így a felület önállóan is fejleszthető.

## Mi van benne

| Fájl | Tartalom |
|---|---|
| `index.html` | a váz: vezérlősáv, munkaterület-fülek, kereső, kitűzött sor, virtualizált rács |
| `app.css` | a design tokenek és a négy téma (világos / **szürke** / sötét / AMOLED) |
| `app.js` | `<antikolt-card>` web-komponens, virtualizált rács, bridge, billentyűzet |

## Két szabály, amit nem szabad megsérteni

1. **Csak a látható sorok léteznek a DOM-ban.** 5 000 kártya ugyanannyiba kerül, mint 50
   (`../docs/HU/03-MEMORIA.md` 6. fejezet). Ezért a rács virtualizált, nem „legyártjuk és elrejtjük".
2. **A lap sem fájlrendszerhez, sem hálózathoz nem nyúl.** Csak a szűk bridge-en keresztül ér el
   böngésző-funkciókat; a kártya-előnézetek is `antikolt://blob/<digest>` címről jönnek.

## Bridge-szerződés (`window.antikolt`)

A valódi shell ezt injektálja. A mock ugyanezt valósítja meg, hogy a lap önállóan is működjön.

```js
antikolt.cards.list()                       // -> Promise<CardMeta[]>
antikolt.cards.open(id, { mode })           // mode: 'live' | 'snapshot'
antikolt.cards.setLayout(id, layout)        // { x, y, w, h, size }
antikolt.workspaces.list() / .switch(id)
antikolt.containers.list()
antikolt.search(query)                      // -> Promise<CardMeta[]>
antikolt.events.on('open' | 'close' | 'preview-updated', cb)
antikolt.system.memory()                    // { usedBytes, budgetBytes, engineState }
antikolt.system.engine()                    // 'cef 142' | ...
```

`CardMeta`:

```ts
interface CardMeta {
  id: string;              // ULID
  title: string;
  url: string;
  domain: string;
  notes?: string;
  tags?: string[];
  status: 'signed_in' | 'expiring' | 'signed_out' | 'unknown';
  pinned: boolean;
  archived: boolean;
  size: 's' | 'm' | 'l';
  container: { name: string; color: string };
  previewRef: string | null;   // blob digest, vagy null ha még nincs előnézet
  luminance?: number;          // 0..1, a sötét mód ellenőrzéséhez
  lastOpenedAt?: number;
  workspaceId?: string;
}
```

## Billentyűzet

`←` `→` `↑` `↓` mozgás · `Home` / `End` · `Enter` megnyitás · `Ctrl`+`K` keresés · `Esc` keresés törlése.
A fókuszgyűrű minden témában látható, és `prefers-reduced-motion` esetén minden animáció kikapcsol.

## Ismert korlát (szándékos, az első körben)

A **kitűzött** kártyák nagy (L) méretben, nem virtualizált sorban jelennek meg (max. 8), a többi kártya
egységes M méretben, virtualizálva. A tetszőleges méretű elrendezést (masonry) a felhasználó saját
kezdőlapja tudja megvalósítani; a beépített sablon a memória-ígéret miatt marad egységes.
