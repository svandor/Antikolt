# Handover — picking Antikolt up in a fresh session

**Magyar összefoglaló lentebb („Átadás magyarul"), a lényeg mindkettőben ugyanaz.**

This file exists so that a new workflow, a new agent, or another person can continue without guessing. It states the
state of the repository, what is **measured** versus **not measured**, the exact commands, the plan for the next
slices, and — critically — **what to agree with the maintainer before writing code**.

- Repository: **https://github.com/svandor/Antikolt** (public, MPL-2.0)
- Default branch: `main` · last verified commit: **`d131c4f`** (`docs(slice-144): release files …`)
- Licence: MPL-2.0 · authors: [`AUTHORS`](AUTHORS) · third parties: [`NOTICE`](NOTICE) + [`THIRD-PARTY-NOTICES.txt`](THIRD-PARTY-NOTICES.txt)
- Documentation: Hungarian is authoritative ([`docs/HU/`](docs/HU/)); the English summary is
  [`docs/EN/README.md`](docs/EN/README.md); the **measured status page** is
  [`docs/HU/21-ALLAPOT.md`](docs/HU/21-ALLAPOT.md).

## 0. Escalation rule — do not loop

If a subtle native failure resists a few localisation attempts (a crash with a varying stack, a use-after-free, a
frame/pipe corruption, an AddressSanitizer report that cannot be explained), **stop and say clearly „Pro modell
szükséges" (Pro model needed)** together with the evidence collected so far — instead of burning more attempts on the
same dead end. A localised-but-unfixed bug is handed back up, not looped on. This rule is the whole point of the
handover.

## 1. What the product is today

A runnable Windows build: a **Qt 6.8 shell** + a **CEF 158 engine host** + a **Rust core** that owns the data. The grid
is a virtualised card view (503 cards → ~25 DOM elements), a card hibernates to disk (MHTML + scroll/anchor/form state)
and restores offline, containers and workspaces are first-class, the profile is portable as one JSON bundle, and **all
stores live in SQLite** (schema `user_version = 10`, WAL) as their only writer. Decisions are recorded as **ADR-001 …
ADR-081** in [`docs/HU/05-DONTESEK.md`](docs/HU/05-DONTESEK.md); the development log runs to slice **144** in
[`docs/HU/16-FEJLESZTESI-NAPLO.md`](docs/HU/16-FEJLESZTESI-NAPLO.md).

What closed in the most recent session (slices 136–144):

| Slice | Feature | Evidence |
|---|---|---|
| 136 | `video_max_height` display cap (F6) | `--videocaptest` (ADR-076) |
| 137 | sha256 content-addressed previews (F2) | `--previewhashtest`, `preview_content_addressed` (ADR-077) |
| 138 | shell chrome HU/EN switch | `--languagetest` (ADR-078) |
| 139 | grid HU/EN switch | `--gridlangtest` (ADR-079) |
| 140 | card inline + static labels HU/EN | same `t()` mechanism (ADR-079) |
| 141 | shell status messages HU/EN | `setStatus`→`t()` (i18n complete) |
| 142 | automatic resource profile (power + pressure, M6) | `--autoprofiltest` (ADR-080) |
| 143 | bookmark import from Netscape HTML (M7) | `--bookmarkimporttest` (ADR-081) |
| 144 | release files + gate check (M8) | `THIRD-PARTY-NOTICES.txt`, `check-repo` "Release files" |

## 2. How to verify (do this first, in a fresh session)

```powershell
git clone https://github.com/svandor/Antikolt && cd Antikolt

# 1. Fast, needs only Rust + Node: the same checks CI runs.
pwsh -File scripts/check-repo.ps1   # SPDX, links, hygiene, release files, JS parse, cargo fmt, clippy -D warnings, 156 Rust tests

# 2. The full gate: builds both native binaries and runs the 68 self tests (needs Qt 6.8 + CEF, GPU disabled).
$env:ANTIKOLT_ENGINE_DISABLE_GPU='1'
pwsh -File scripts/verify.ps1       # ends with "ALL CHECKS PASSED"

# 3. The portable build and a deployed sanity run.
pwsh -File scripts/dist.ps1 -Release
dist\Antikolt\antikolt-shell.exe --addcardtest dist\Antikolt\antikolt-engine-host.exe
```

Expected on a healthy host: `check-repo` ≈ 1 min, `verify.ps1` ≈ 6–8 min, `dist` ≈ 7 min. **Numbers in the docs come
from the gate log, not from memory** — that rule was learned the hard way.

## 3. The one environment blocker: the hardware GPU path

The **software-rendering** gate is fully green. The **hardware** GPU path still aborts on the RTX 5080 (GPU process
exits `0x80000003`; the `d3d11` ANGLE backend is the suspect, `--use-angle=gl` keeps it alive). So run the windowed
gate with `$env:ANTIKOLT_ENGINE_DISABLE_GPU='1'`. A newer NVIDIA driver or forcing the Intel iGPU is still worth
trying, but it is **not** a blocker for product work.

## 4. The remaining work — plan, in order

The well-scoped code work is **done**. What remains is a small set of large items, several of which need a maintainer
decision first. Work through them in this order.

### A. Agree with the maintainer FIRST (these block the code)

1. **Password vault scope** (the last M7 code item). Antikolt has **no password store** today, so "password import"
   cannot land without one. Decide: (a) what the vault holds — passwords only, or passwords + TOTP + passkeys
   (`docs/HU/12-PASSKEY.md` already explored passkeys); (b) encryption — Windows DPAPI per-user, or a master
   password; (c) import sources — Chrome/Firefox CSV, or skip import and do the vault alone first.
2. **PiP** (M7). CEF 158's PiP is **views-based** (`CefBrowserViewDelegate`), while the shell uses the windowed (not
   views) API — so PiP likely needs an architecture switch. Decide: accept the switch, or **drop PiP** and mark it
   `not-measurable` for now. The honest current note is in `docs/HU/19-FOLYTATAS.md` (PiP row).
3. **Milestone/version.** M6 is done and M7 is nearly done (PiP, password import and full screen-reader remain). Agree
   whether to bump the `v0.8` badge → `v0.9` in `README.md` / `docs/EN/README.md`, and which open items gate that.

### B. Concrete work once the decisions are made

1. **Vault + password import (M7)** — the biggest remaining code slice. Rust vault crate (ADR-worthy: encryption,
   one-writer store like ADR-069/070), shell settings UI, `--vaulttest` self test, then CSV import behind the same
   "native file dialog + measured path" pattern as `--bookmarkimporttest` (slice 143).
2. **PiP** (only if the maintainer approves the architecture switch).
3. **Full HU→EN documentation (M8)** — the authoritative plan is Hungarian; `docs/EN/README.md` is only the condensed
   summary. This is a large, self-contained translation pass (docs `00`–`21`).
4. **Linux package (M8)** — Nobara/KDE/Wayland with OSR + RPM. Needs a separate Linux environment; the CEF/OSR path
   is not yet exercised off-Windows.
5. **Release (human steps)** — SignPath / Store signing (`docs/HU/20-ALASAS.md`), and the GitHub Actions release
   workflow that has never run (`docs/HU/21-ALLAPOT.md`, "Amihez emberi lépés kell"). Also the full NVDA screen-reader
   pass is a human step, not code.

### C. What is deliberately not measurable (do not "fix" these into fake greens)

- OAuth: the mechanism is built, the sign-in **gesture** cannot be clicked by the harness → `not-measurable` gesture.
- Video **decode**-resolution limit: CEF exposes no per-page decode cap → the cap is a **display** cap (ADR-076).
- The native file **dialogs**: the harness cannot click them → measured through the underlying bridge path instead.

## 5. Rules that are not negotiable in this project

- **Never a dialog in the page** (ADR-053): CEF's `prompt` returns `null` silently, so every editor is inline.
- **Never a green that lies:** a check prints what it measured, and `not-measurable` with a reason beats a fake pass.
- **Write order matters:** an editor writes first and restores the DOM afterwards (ADR-062).
- **Every store has one writer** (ADR-069/070): SQLite. The `*.json` documents are exports, never imported once the
  `meta` table says the store was adopted.
- **Numbers in the docs come from the gate log**, never from memory.
- **Escalate instead of looping** (handover §0).
- **Do not build strings with PowerShell `Replace` and write them blindly** — that destroyed `store.rs`, `main.rs`
  (twice) and `main.cpp` in this project; use the `write`/`edit` tools.
- **One slice = one measurable feature + self test** → build + green gate → dev-log entry (with measured numbers) →
  commit `feat/test/docs(slice-NNN): …` → push. Docs to update: `16`, `21`, `04`, `19` (and `05` for ADRs).

## 6. Where to look first

| Question | File |
|---|---|
| What works, with which evidence | [`docs/HU/21-ALLAPOT.md`](docs/HU/21-ALLAPOT.md) |
| Why a decision was made | [`docs/HU/05-DONTESEK.md`](docs/HU/05-DONTESEK.md) (ADR-001…081) |
| What happened, slice by slice, with outputs | [`docs/HU/16-FEJLESZTESI-NAPLO.md`](docs/HU/16-FEJLESZTESI-NAPLO.md) |
| Toolchain and its traps (Qt, CEF, MSVC) | [`docs/HU/17-ESZKOZLANC.md`](docs/HU/17-ESZKOZLANC.md) |
| The plan and the phases | [`docs/HU/04-UTEMTERV.md`](docs/HU/04-UTEMTERV.md), [`docs/HU/19-FOLYTATAS.md`](docs/HU/19-FOLYTATAS.md) |
| Wire protocol between shell, engine and core | [`proto/README.md`](proto/README.md) |
| What a shell test does and how to run it | [`apps/desktop-shell/README.md`](apps/desktop-shell/README.md) |

---

## Átadás magyarul

**Állapot:** a repó nyilvános (`github.com/svandor/Antikolt`), a `main` ág utolsó commitja `d131c4f` (144. szelet),
MPL-2.0, magyar (tekintélyes) és angol (összefoglaló) dokumentációval; a döntések **ADR-001…081**, a napló **144
szeletig**. A jól szcopolható kódmunka **kész** — a legutóbbi munkamenet 9 szeletet zárt (136–144: videósapka,
sha256-preview, HU/EN teljes, automatikus profil, könyvjelző-import, kiadási fájlok).

**Első lépés az új munkamenetben:** `pwsh -File scripts/check-repo.ps1`, majd a teljes kapu **ezen a gépen**,
szoftveres rendereléssel: `$env:ANTIKOLT_ENGINE_DISABLE_GPU='1'; pwsh -File scripts/verify.ps1` — a hardveres GPU-út
a VECTOR-on továbbra is elhal (RTX 5080), de ez **nem blokkolja** a termékmunkát. A kapu most **71 ellenőrzés**
(68 önteszt + build + core service + check-repo), `cargo test` **156** zöld (78+53+9+1+15), séma `user_version=10`.

**A maradék — terv, sorrendben:**

1. **Előbb egyeztess a karbantartóval** (ezek blokkolják a kódot):
   - **Jelszó-boltozat köre** (az M7 utolsó kód-tétele): a termékben **nincs** jelszótároló, így a jelszó-import nem
     mehet anélkül. Döntés kell: mit tárol (csak jelszavak / + TOTP / + passkey), milyen titkosítással (Windows DPAPI
     vagy mesterjelszó), és honnan importál (Chrome/Firefox CSV, vagy előbb csak a vault).
   - **PiP** (M7): a CEF 158 PiP-je views-alapú, a shell ablakos API-t használ — valószínűleg architektúra-váltás
     kell. Döntés: vállaljuk a váltást, vagy **engedjük el** a PiP-t és jelöljük `not-measurable`-nek.
   - **Verzió/mérföldkő:** az M6 kész, az M7 majdnem — egyeztetni kell, hogy a `v0.8` → `v0.9` bump megtörténhet-e,
     és mi zárja az M7-et.

2. **A döntések után, konkrétan:**
   - vault + jelszó-import (a legnagyobb maradék kód-szelet; `--vaulttest`, majd CSV-import a `--bookmarkimporttest`
     mintájára);
   - PiP (csak jóváhagyás után);
   - teljes HU→EN dokumentáció (M8, nagy önálló fordítási pass);
   - Linux-csomag (M8, Nobara/KDE/Wayland + OSR + RPM, külön környezet kell);
   - kiadás (emberi: SignPath/Store, GitHub Actions release, NVDA képernyőolvasó-átnézés).

**Szándékosan nem mérhető (ne „javítsd" hamis zöldre):** az OAuth gesztus-út (`not-measurable`), a videó
**dekódolási** felbontás-korlát (a CEF nem ad laponkénti korlátot — a sapka megjelenítési, ADR-076), és a natív
fájlválasztó dialógusok (a harness nem kattinthatja — a mögöttes bridge-út mérve).

**Szabályok, amiket nem szabad újranyitni:** nincs dialógus a lapon (ADR-053); soha nem hazudó zöld (a
`not-measurable` jobb a hamis passznál); az editor előbb ír, aztán állítja vissza a DOM-ot (ADR-062); **minden
store-nak egy írója van** (ADR-069/070: az adatbázis, a `*.json` export); a dokumentumok számait a kapu naplójából
kell venni; **PowerShell `Replace`-csel előállított szöveget soha nem szabad vakon kiírni**; és **egy szelet = egy
mérhető feature + önteszt → build + zöld kapu → naplóbejegyzés a mért számokkal → commit + push**.

**Eszkaláció:** ha apró natív hiba néhány próbálkozásból nem lokalizálható, állj meg és mondd ki: „Pro modell
szükséges" — a bizonyítékkal együtt, nem hurkolva.
