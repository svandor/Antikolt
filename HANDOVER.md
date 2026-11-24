# Handover — picking Antikolt up in a fresh session

**Magyar összefoglaló lentebb („Átadás magyarul"), a lényeg mindkettőben ugyanaz.**

This file exists so that a new workflow, a new agent, or another person can continue without guessing. It states the
state of the repository, what is **measured** versus **not measured**, the exact commands, the plan for the next
slices, and — critically — **what to agree with the maintainer before writing code**.

- Repository: **https://github.com/svandor/Antikolt** (public, MPL-2.0)
- Default branch: `main` · last verified commit: **`68c14d3`** (`feat(slice-164): full HU→EN translation`)
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
stores live in SQLite** (schema `user_version = 11`, WAL) as their only writer. The UI is **fully bilingual HU/EN** —
the chrome, the status bar, the dialogs/tooltips and the grid/card strings all translate (the switch is done; the
Hungarian *documentation* translation is still open, see §4.B). Decisions are recorded as **ADR-001 … ADR-097** in
[`docs/HU/05-DONTESEK.md`](docs/HU/05-DONTESEK.md); the development log runs to slice **164** in
[`docs/HU/16-FEJLESZTESI-NAPLO.md`](docs/HU/16-FEJLESZTESI-NAPLO.md).

What closed in the most recent session (slices 145–164):

| Slice | Feature | Evidence |
|---|---|---|
| 145–158 | Password vault + passkey (M7): AES-256-GCM + Argon2id + DPAPI, credential table, TOTP, CSV import, autofill (isolated world, origin lookup, `fill_credential`), vault panel, passkey store | `cargo test` 190 + `--vaulttest`/`--vaultpaneltest`/`--autofilltest`/`--fillcredentialtest`/`--passkeyflowtest`/`--passkeystoretest` (ADR-083…093) |
| 157, 160, 162 | PiP de-risking: API present, window opens, but **deferred** (windowed shutdown crash `0xC0000005`; CHROME-style views reject custom cache paths) | `--pipchecktest`, `--viewstest` (ADR-095/096/097) |
| 163 | Passkey key export from the virtual authenticator (full credential incl. `privateKey`) | `--passkeyexporttest` (closes the authenticator→vault loop) |
| 164 | Full HU→EN UI translation: ~80 hardcoded strings wired through `uiText()`/`t()` + ~100 new EN entries | `--languagetest`, `--gridlangtest`, `check-repo` |

## 2. How to verify (do this first, in a fresh session)

```powershell
git clone https://github.com/svandor/Antikolt && cd Antikolt

# 1. Fast, needs only Rust + Node: the same checks CI runs.
pwsh -File scripts/check-repo.ps1   # SPDX, links, hygiene, release files, JS parse, cargo fmt, clippy -D warnings, 190 Rust tests

# 2. The full gate: builds both native binaries and runs the 82 checks (needs Qt 6.8 + CEF, GPU disabled).
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

The well-scoped **UI code work is done** (M7 closed except the deferred PiP; the M8 UI translation is also done). What
remains is a small set of items, one of which needs a maintainer decision.

### A. Agree with the maintainer FIRST (this blocks nothing but the milestone label)

1. **Milestone/version.** M6 is done and M7 is closed (PiP deferred by the 2026-11-24 decision; vault + passkey done).
   The one open decision: whether to bump the `v0.8` badge → `v0.9` in `README.md` / `docs/EN/README.md` / `Cargo.toml`
   / `CMakeLists.txt`, which also closes M7.

### B. Concrete work, in order (no decision needed)

1. **Full HU→EN documentation translation (M8).** The UI is bilingual, but the authoritative *docs* (`docs/HU/00`–`21`)
   are still Hungarian; `docs/EN/README.md` is only the condensed summary. This is a large, self-contained translation
   pass — good to fan out with the free subagent chain (survey/draft per file), then wire and review on the main thread.
2. **Linux package (M8).** Nobara/KDE/Wayland with OSR + RPM. Needs a separate Linux environment; the CEF/OSR path is
   not yet exercised off-Windows. This is a separate-session + Pro item.
3. **Release (human steps).** SignPath / Store signing (`docs/HU/20-ALASAS.md`), the GitHub Actions release workflow
   that has never run, and the full NVDA screen-reader pass. The updater (Ed25519) + installer are the remaining *code*
   pieces here — separate-session + Pro.

### C. What is deliberately not measurable (do not "fix" these into fake greens)

- OAuth: the mechanism is built, the sign-in **gesture** cannot be clicked by the harness → `not-measurable` gesture.
- Video **decode**-resolution limit: CEF exposes no per-page decode cap → the cap is a **display** cap (ADR-076).
- The native file **dialogs**: the harness cannot click them → measured through the underlying bridge path instead.
- Full `navigator.credentials.create/get` on a real surface (focus-gated; the harness uses CDP `addCredential`).
- PiP on the windowed path: **deferred** (ADR-095/096/097 — windowed shutdown crashes, views rejects custom cache).

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
| Why a decision was made | [`docs/HU/05-DONTESEK.md`](docs/HU/05-DONTESEK.md) (ADR-001…097) |
| What happened, slice by slice, with outputs | [`docs/HU/16-FEJLESZTESI-NAPLO.md`](docs/HU/16-FEJLESZTESI-NAPLO.md) |
| Toolchain and its traps (Qt, CEF, MSVC) | [`docs/HU/17-ESZKOZLANC.md`](docs/HU/17-ESZKOZLANC.md) |
| The plan and the phases | [`docs/HU/04-UTEMTERV.md`](docs/HU/04-UTEMTERV.md), [`docs/HU/19-FOLYTATAS.md`](docs/HU/19-FOLYTATAS.md) |
| Wire protocol between shell, engine and core | [`proto/README.md`](proto/README.md) |
| What a shell test does and how to run it | [`apps/desktop-shell/README.md`](apps/desktop-shell/README.md) |

---

## Átadás magyarul

**Állapot:** a repó nyilvános (`github.com/svandor/Antikolt`), a `main` ág utolsó commitja `68c14d3` (164. szelet),
MPL-2.0, magyar (tekintélyes) és angol (összefoglaló) dokumentációval; a döntések **ADR-001…097**, a napló **164
szeletig**. A **jól szcopolható felhasználói kódmunka kész**: az M7 lezárva (a széf + passkey kész, a PiP elhalasztva),
az M8 **felületi** HU→EN fordítás is kész (164. szelet).

**Első lépés az új munkamenetben:** `pwsh -File scripts/check-repo.ps1`, majd a teljes kapu **ezen a gépen**,
szoftveres rendereléssel: `$env:ANTIKOLT_ENGINE_DISABLE_GPU='1'; pwsh -File scripts/verify.ps1` — a hardveres GPU-út
az RTX 5080-on továbbra is elhal (`0x80000003`), de ez **nem blokkolja** a termékmunkát. A kapu most **82 ellenőrzés**
(79 önteszt + build + core service + check-repo), `cargo test` **190** zöld (93+69+12+1+15), séma `user_version=11`.

**A maradék — terv, sorrendben:**

1. **Egyetlen nyitott döntés (ez csak a címkét blokkolja):** a **verzióbump** — `v0.8` → `v0.9` a `README.md`-ben /
   `docs/EN/README.md`-ben / `Cargo.toml`-ban / `CMakeLists.txt`-ben, ami lezárja az M7-et.

2. **Konkrétan, döntés nélkül:**
   - **teljes HU→EN dokumentáció-fordítás (M8)** — a felület kétnyelvű, de a tekintélyes *dokumentumok* (`docs/HU/00–21`)
     még magyarok; nagy önálló fordítási pass (free-lánccal feldolgozható, Pro-val ellenőrizve);
   - **Linux-csomag (M8)** — Nobara/KDE/Wayland + OSR + RPM, külön környezet kell (külön munkamenet + Pro);
   - **kiadás (emberi)** — SignPath/Store aláírás, a sosem futott GitHub Actions release, NVDA képernyőolvasó-átnézés;
     a frissítő (Ed25519) + telepítő a maradék *kód* itt (külön munkamenet + Pro).

**Szándékosan nem mérhető (ne „javítsd" hamis zöldre):** az OAuth gesztus-út (`not-measurable`), a videó
**dekódolási** felbontás-korlát (a CEF nem ad laponkénti korlátot — a sapka megjelenítési, ADR-076), a natív
fájlválasztó dialógusok (a harness nem kattinthatja — a mögöttes bridge-út mérve), a teljes
`navigator.credentials.create/get` valós felületen (fókusz-gesztus), és a **PiP a windowed úton** (elhalasztva,
ADR-095/096/097).

**Szabályok, amiket nem szabad újranyitni:** nincs dialógus a lapon (ADR-053); soha nem hazudó zöld (a
`not-measurable` jobb a hamis passznál); az editor előbb ír, aztán állítja vissza a DOM-ot (ADR-062); **minden
store-nak egy írója van** (ADR-069/070: az adatbázis, a `*.json` export); a dokumentumok számait a kapu naplójából
kell venni; **PowerShell `Replace`-csel előállított szöveget soha nem szabad vakon kiírni**; és **egy szelet = egy
mérhető feature + önteszt → build + zöld kapu → naplóbejegyzés a mért számokkal → commit + push**.

**Modell-triage (mért tanulság a 164. szeletből):** a **free-lánc** (Flash) felmérésre/keresésre/draftra jó, de a
terhelést-hordozó következtetést és a user-fájlok szerkesztését a **Pro** végzi — a 164. szeletben egy „csak olvasás"
utasítású free-subagent részlegesen és hiányosan szerkesztett, amit vissza kellett állítani. A natív (CEF/CMake/kripto)
munka és a build/kapu/commit/push a Pro-é; a külön környezetet vagy nagy nem-inkrementális szeletet igénylő munka
(Linux, telepítő/frissítő) **külön munkamenet + Pro**.

**Eszkaláció:** ha apró natív hiba néhány próbálkozásból nem lokalizálható, állj meg és mondd ki: „Pro modell
szükséges" — a bizonyítékkal együtt, nem hurkolva.
