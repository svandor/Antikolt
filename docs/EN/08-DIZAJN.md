# 08 — Design basics (design tokens)

The "modern appearance" is not a matter of taste, but a system: shared tokens that the Qt/QML interface
**and** the start page (HTML/CSS) use the same way. The **grey (graphite) is the default** theme, because it is the
least straining both by day and at night — that is why this is the "no-compromise" base (according to the user's
decision).

## 1. Four interface themes

| Token | `light` | `grey` (base) | `dark` | `amoled` |
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

**Contrast requirement:** WCAG AA (≥ 4,5:1) for text, and ≥ 3:1 for large text and icons — this is
**measured automatically** for every theme (in CI, with a contrast-calculating test), not by eye.

Container colors (identity marking, 8 fixed colors, the same in every theme):
`#E0533D`, `#E3A008`, `#2E9E5B`, `#2F6FEB`, `#8E44AD`, `#D6336C`, `#0FA3A3`, `#6B7280`.

## 2. Typography

- Font: **Inter** (with full support for the Hungarian ő/ű/Ő/Ű characters) — the same on the start page and in the Qt interface
  so that the card and the interface speak one language.
- Sizes: `11` (meta), `13` (base UI), `15` (highlighted), `17` (card title), `22` (section title), `28` (large title).
- Line height: 1,45; letter spacing: 0; `-0.01em` for titles.
- Correct use of **hyphen and dash** in Hungarian texts (interface texts in the i18n file).

## 3. Spacing, form, motion

| Token | Value |
|---|---|
| `--space-1..6` | 4, 8, 12, 16, 24, 32 px |
| `--radius-card` | 10 px |
| `--radius-panel` | 14 px |
| `--radius-control` | 8 px |
| `--shadow-1` | `0 1px 2px rgba(0,0,0,.18)` |
| `--shadow-2` | `0 6px 24px rgba(0,0,0,.28)` |
| `--motion-fast` | 120 ms (`ease-out`) |
| `--motion-base` | 250 ms (card morphing, see `02` 4.4.) |
| `--motion-panel` | 200 ms (`cubic-bezier(.2,.8,.2,1)`) |

With prefers-reduced-motion, every animation switches to 0 ms (and card morphing to a crossfade).

## 4. Card sizes and the grid

| Size | Card | Preview | Use |
|---|---|---|---|
| S | 180 × 110 | 164 × 92 | dense list, "frequently used" |
| M | 260 × 150 | 240 × 130 | default |
| L | 380 × 220 | 360 × 200 | highlighted, "continue reading" |

Grid gap: 16 px; the grid is **virtualized** (only the visible cards live in the DOM — `03` chapter 6).
The preview aspect ratio is **16:10**, with a slight darkening gradient toward the bottom of the card, so the title is always readable
(regardless of the image content).

## 5. The anatomy of the card

```
┌─────────────────────────────┐
│  [előnézeti kép 16:10]      │  ← WebP, 260×150
├─────────────────────────────┤
│ ● Cím (max 2 sor, csonkolva)│  ← konténer-szín pont
│ domain.hu            🔒     │  ← 🔒 = bejelentkezve / ⏳ = hamarosan lejár
└─────────────────────────────┘
```

Status indicators: `🔒` logged in, `⏳` expires within 24 hours, `🔓` logged out, `📌` pinned,
`💤` archived, `⏳→` deletion in progress.

## 6. Control bar (as thin as possible)

From the left: back · forward · reload · **⌂ home** · address/search field · container badge (color + name) ·
theme switcher (light/grey/dark) · vault. From the right: memory indicator (optional), menu.
Height: 40 px; icons 18 px; the address field gets the remaining space.

**Where a button also indicates state, the icon is the interface and the word is the tooltip** (2026-11-18): the four interface
themes are each represented by a generated icon (the theme's background + text color), the name is in the tooltip and the
screen-reader name; the page-darkening crescent-moon icon is selectable from the menu, and its **fill shows
the effective level** (empty = off, black = dark, grey = dark grey, warm = dark warm).

## 7. Accessibility

- Full keyboard navigation; focus ring visible in every theme (2 px, `--accent`).
- Screen-reader labels for every icon (`accessibleName`).
- No information conveyed by color alone (in the container indicator, a letter accompanies the color).
- The card grid's focus order is left to right, top to bottom; `Home`/`End` to the start/end of rows.

## 8. What the interface never does

- **Does not open an unjustified second window, nor keep one alive.** A browser has one visible window
  (the card is embedded in it, not a separate window), the engine and core processes talk over hidden pipes, and the
  taskbar icon is not a window but a signal. If a launch would still create an extra window (Windows gives one to
  console-subsystem programs), the program **discards it immediately** — it does not hide it, does not leave it there
  (`DropOwnConsoleWindow`, `main.cpp`). This is the user's rule: "it is strictly forbidden to open any unjustified
  extra window, and above all to keep one alive" (2026-11-18).
- Does not flicker, does not jump (card morphing is the exception, but even that is 250 ms).
- Does not ask a question whose answer is predictable (e.g. "are you sure you want to open it?" — opening is cheap).
- Does not stuff it full of labels: the card's title + domain + status is enough.
- Does not hide the memory (the indicator shows how much is currently used — this is part of trust).
- Does not lie about the theme: the control-bar choice immediately repaints its **own** surfaces (window, grid), while
  websites are handled either by their own dark theme or by the "Dark pages" toggle requested by the user
  (ADR-044) — `--themetest` measures both, pixel by pixel.

## 9. Dark pages (ADR-044/ADR-045)

`light`/`grey`/`dark`/`amoled` is the **product's** interface; a web page stays light if it has no dark theme of its own
This is what the control-bar **crescent-moon icon** is for (off by default, remembered), whose menu has
four levels:

| Level | What it does to the page | Measured brightness (white test page) |
|---|---|---|
| `off` (Off) | nothing | 251 |
| `invert` (Dark) | inversion + `hue-rotate(180deg)` | 4 |
| `dim` (Dark grey) | the same + `contrast(0.72)` | 39 |
| `warm` (Dark warm) | the same + `contrast(0.78) sepia(0.35)` | 33 |

The engine inserts a **named** style element into the page; a mode change replaces it, switching off removes it —
without a reload. Not to be confused with the "Page theme: auto/dark/light" button, which only *tells* the page what
the system wants.
