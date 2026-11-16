# `perf/` — mérési mérőpad

> A büdzsé számait (`docs/HU/03-MEMORIA.md`) **mérni kell**, nem hinni. Ez a mappa a mérőpad helye.

## Miért van ez a mappa, ha még üres

A mérőpad a shell (Qt + CEF) megjelenésével válik futtathatóvá, mert a memória nagy része a motor
folyamatfájában van. Addig is itt vannak a **specifikációk és a kapuk**, hogy a mérés ne maradjon el:
a `03-MEMORIA.md` 5. fejezete pontosan megmondja, mit, hogyan és milyen küszöbökkel kell mérni.

## Mit fog mérni

| Szcenárió | Mit bizonyít |
|---|---|
| S0 | üres böngésző, üres kártyarács — alapköltség |
| S1 | 50 kártya, 50 előnézet — virtualizáció |
| S2 | 500 kártya — nagy könyvtár |
| S3 | egy statikus cikk — M1 mód |
| S4 | YouTube 1080p — nehéz oldal, büdzsé-vezérlő |
| S5 | Gmail-szerű SPA — nehéz SPA |
| S6 | 10 kártya váltogatva — szivárgás-ellenőrzés |
| S7 | 1 óra szimulált használat — hosszú távú stabilitás |
| S8 | ultra-low (T2) tétlen — M0-light |
| S9 | 25 életben tartott bejelentkezés, hideg motor — a login 0 RAM |
| S10 | ugyanaz a szcenárió mind az öt profillal — a profilok valós különbsége |

## Hogyan mérünk

- **Windows:** a folyamatfa **privát munkakészlete** (`Get-Counter` vagy `K32GetProcessMemoryInfo`),
  folyamatonként és összesen. **Nem** RSS-összeg (a megosztott lapokat többször számolná).
- **Linux:** `PSS` a `/proc/<pid>/smaps_rollup`-ból.
- 5 futás, **medián**; meleg indítás; rögzített viewport (1440×900, DPR 1.25); a gépen semmi más nem fut.
- Minden futás eredménye `perf/baseline/<dátum>.json` + emberi riport (`perf/reports/<dátum>.md`).
- A CI `memory-budget` jobja **elutasítja** a kiadást, ha a `03-MEMORIA.md` 5.4. táblázat küszöbeit
  átlépi (a mérőpad elkészülte után).

## Elfogadási küszöbök (kivonat)

| Mérőszám | Cél | Kemény maximum |
|---|---|---|
| S0 privát memória | ≤ 380 MB | 450 MB |
| S3 (egy cikk) | ≤ 620 MB | 700 MB |
| S4 (nehéz videó) | ≤ 1000 MB | 1100 MB |
| S8 / S9 (tétlen) | ≤ 180 MB | 250 MB |
| Renderelő folyamatok (I1) | 1 | 1 |
| Kártyanyitás érzékelt idő | ≤ 600 ms | 900 ms |
| Fagyasztás | ≤ 800 ms | 1500 ms |

## Állapot

- [ ] Mérőpad váz (folyamatfa-mérés) — *a következő szelet része*
- [ ] Shell szcenáriók (S3–S5)
- [ ] Baseline + CI-kapu
- [ ] Profil-összehasonlítás (S10) riportja

A naplót lásd: `docs/HU/16-FEJLESZTESI-NAPLO.md`.
