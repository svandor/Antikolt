# Handover — picking Antikolt up in a fresh session

**Magyar összefoglaló lentebb („Átadás magyarul"), a lényeg mindkettőben ugyanaz.**

This file exists so that a new workflow, a new agent, or another person can continue without guessing. It states the
state of the repository, what is **measured** versus **not measured**, the exact commands, and the one thing that
currently blocks the full test suite on the machine the work was built on.

- Repository: **https://github.com/svandor/Antikolt** (public, MPL-2.0)
- Default branch: `main` · latest verified CI run: **success** (`gh run list --repo svandor/Antikolt --limit 1`)
- Licence: MPL-2.0 · authors: [`AUTHORS`](AUTHORS) · third parties and the AI-assisted notice: [`NOTICE`](NOTICE)
- Documentation: Hungarian is authoritative ([`docs/HU/`](docs/HU/)); the English summary is
  [`docs/EN/README.md`](docs/EN/README.md); the **measured status page** is
  [`docs/HU/21-ALLAPOT.md`](docs/HU/21-ALLAPOT.md).

## 1. What the product is today

A runnable Windows build: a **Qt 6 shell** + a **CEF engine host** + a **Rust core** that owns the data. The grid is a
virtualised card view (503 cards → 13 DOM elements), a card hibernates to disk (MHTML + scroll/anchor/form state) and
restores offline, containers and workspaces are first-class, the profile is portable as one JSON bundle, and the
SQLite store (schema `user_version = 2`, WAL) already serves the grid's card list while the JSON files remain the
writer. Decisions are recorded as **ADR-001 … ADR-068** in [`docs/HU/05-DONTESEK.md`](docs/HU/05-DONTESEK.md); the
development log runs to slice **88** in [`docs/HU/16-FEJLESZTESI-NAPLO.md`](docs/HU/16-FEJLESZTESI-NAPLO.md).

## 2. How to verify (do this first, in a fresh session)

```powershell
git clone https://github.com/svandor/Antikolt && cd Antikolt

# 1. Fast, needs only Rust + Node: the same checks CI runs.
pwsh -File scripts/check-repo.ps1        # SPDX, links, JS parse, cargo fmt, clippy -D warnings, 78+21+2+1+15 Rust tests

# 2. The full gate: builds both native binaries and runs the 37 self tests (needs Qt 6.8 + CEF).
pwsh -File scripts/verify.ps1            # ends with "ALL SELF TESTS PASSED"

# 3. The portable build and a deployed sanity run.
pwsh -File scripts/dist.ps1 -Release
dist\Antikolt\antikolt-shell.exe --addcardtest dist\Antikolt\antikolt-engine-host.exe
```

Expected on a healthy host: `check-repo` ≈ 1 min, `verify.ps1` ≈ 6–8 min, `dist` ≈ 7 min. Numbers in the docs come
from the gate log, not from memory — that rule was learned the hard way (dev log 84).

## 3. The one open blocker (measured)

On the machine this work was built on, **CEF cannot create a GPU channel any more**:

```
ContextResult::kFatalFailure: Failed to create shared context for virtualization
GPU process exited unexpectedly: exit_code=-2147483645
```

The GPU process crash-loops, which eats the CPU: `--coretest` is unaffected (`exit=0`), the engine write path is
exonerated by trace markers (`write` → `wrote` → `flushed`), and an alive-loop tick showed the event loop running for
the entire wait (244 ticks, 25 evals entered, 24 exited) — so **nothing in the shell blocks**; the suite is simply too
slow for its watchdog on this host. So:

- **Not measured here:** the 37 windowed self tests, and therefore the two-step delete (ADR-068).
- **Measured here:** `check-repo` green, `--coretest` green, the storage/parity work (Rust tests), the page's JS.
- The machine restart did **not** help; `ANTIKOLT_ENGINE_DISABLE_GPU=1` (now adding `--use-angle=swiftshader
  --disable-gpu-sandbox`) and `ANTIKOLT_ENGINE_WINDOWLESS=1` did not either.

**Therefore:** the first job in the new session is to run `scripts/verify.ps1` on a host whose CEF GPU initialises
(another PC or a VM with a working GPU), paste the result into `docs/HU/16-FEJLESZTESI-NAPLO.md` as a new slice, and
flip the two-step delete (ADR-068) from `not-measurable` to measured — or fix what actually fails there.

## 4. The next three slices, in order

1. **Close ADR-068 with a measurement** (needs a healthy GPU host): full gate + deployed `--addcardtest`; expect
   `delete_armed=ok`, `delete_card=ok`, `add_card_check=ok`.
2. **Finish the store switch:** the writes still go to the JSON files, and every multi-card op has to mirror by hand
   (`container_delete` with `move_to` needed exactly that fix, dev log 81). Next: `card_upsert`/`card_open`/
   `card_page_text`/`card_delete` write **SQLite first**, JSON becomes an export, and `Db::mirror` disappears.
3. **Container-level permission/proxy (F3)**, then the OAuth popup — the last two structural pieces before a release
   story (SignPath signing is a human step; see `docs/HU/20-ALASAS.md`).

## 5. Rules that are not negotiable in this project

- **Never a dialog in the page** (ADR-053): CEF's `prompt` returns `null` silently, so every editor is inline.
- **Never a green that lies:** a check prints what it measured, and `not-measurable` with a reason beats a fake pass.
- **Write order matters:** an editor writes first and restores the DOM afterwards (ADR-062) — the `blur` handler
  re-enters otherwise and swallows the save.
- **Multi-card writes must mirror explicitly** while two stores exist (ADR-066): equal row counts hide stale rows.
- **Numbers in the docs come from the gate log**, never from memory.
- **Do not build strings with PowerShell `Replace` and write them blindly** — that destroyed `store.rs`, `main.rs`
  (twice) and `main.cpp` in this project; use the `write`/`edit` tools.

## 6. Where to look first

| Question | File |
|---|---|
| What works, with which evidence | [`docs/HU/21-ALLAPOT.md`](docs/HU/21-ALLAPOT.md) |
| Why a decision was made | [`docs/HU/05-DONTESEK.md`](docs/HU/05-DONTESEK.md) (ADR-001…068) |
| What happened, slice by slice, with outputs | [`docs/HU/16-FEJLESZTESI-NAPLO.md`](docs/HU/16-FEJLESZTESI-NAPLO.md) |
| Toolchain and its traps (Qt, CEF, MSVC) | [`docs/HU/17-ESZKOZLANC.md`](docs/HU/17-ESZKOZLANC.md) |
| The plan and the phases | [`docs/HU/04-UTEMTERV.md`](docs/HU/04-UTEMTERV.md), [`docs/HU/19-FOLYTATAS.md`](docs/HU/19-FOLYTATAS.md) |
| Wire protocol between shell, engine and core | [`proto/README.md`](proto/README.md) |
| What a shell test does and how to run it | [`apps/desktop-shell/README.md`](apps/desktop-shell/README.md) |

---

## Átadás magyarul

**Igen, a munka átadható** — de csak akkor, ha a következő munkafolyamat nem találgat. Ez a fájl ezért kimondja:
mi a kész, mi **mért**, mi **nem mért**, milyen parancsokkal lehet ellenőrizni, és mi az az **egyetlen blokker**, ami
ezen a gépen megállítja a teljes kaput.

**Állapot:** a repó nyilvános (`github.com/svandor/Antikolt`), a `main` ág zöld CI-val, MPL-2.0 licenccel, magyar
(tekintélyes) és angol (összefoglaló) dokumentációval; a döntések ADR-001…068, a napló 88 szeletig.

**Első lépés az új munkafolyamatban:** `pwsh -File scripts/check-repo.ps1` (gyors, Rust+Node kell csak), majd
`pwsh -File scripts/verify.ps1` **olyan gépen, ahol a CEF GPU-ja elindul**.

**A blokker (mért):** ezen a gépen a CEF nem tud GPU-kontextust létrehozni (`Failed to create shared context for
virtualization`), a GPU-folyamat crash-loopja lelassítja a futást, ezért a 37 ablakos önteszt nem fut le. Bizonyított,
hogy **nem a mi kódunk blokkol**: az írás (`write`→`wrote`→`flushed`) lefut, az eseményciklus végig él (244 tick),
az `eval`-ek válaszolnak (25 belépés / 24 kilépés). Az újraindítás és a SwiftShader/windowless kapcsolók sem segítettek.

**Ezért nem mérhető itt:** a 37 önteszt és a kétlépéses törlés (ADR-068). **Ez mérhető itt:** `check-repo`, `--coretest`,
a Rust-tesztek (78+21+2+1+15), a lap JS-e.

**Következő három szelet:** (1) ADR-068 lezárása méréssel egészséges GPU-jú gépen; (2) az írás átállítása SQLite-ra
(a JSON legyen export); (3) konténerenkénti engedély/proxy (F3), majd OAuth-popup — utána a kiadás (SignPath, emberi lépés).

**Szabályok, amiket nem szabad újranyitni:** nincs dialógus a lapon (ADR-053); soha nem hazudó zöld (a
`not-measurable` jobb a hamis passznál); az editor előbb ír, aztán állítja vissza a DOM-ot (ADR-062); több-kártyás
írást kézzel kell tükrözni, amíg két store van (ADR-066); a dokumentumok számait a kapu naplójából kell venni; és
**PowerShell `Replace`-csel előállított szöveget soha nem szabad vakon kiírni** — ez a projektben háromszor törölt fájlt.
