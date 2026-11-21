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

## 0. Model handover (read this first)

The remaining work is handed to **DeepSeek V4.1 Flash**. The deep native debugging is already **closed** on
DeepSeek V4 Pro (dev logs 90–94: the CEF-GPU flag, the stdio handle, the use-after-free, and the text-mode
frame corruption). Flash takes the rest: the OAuth work — the container proxy/permission engine half landed in ADR-071 (dev log 98), the remaining product features,
and release engineering. The **whole store switch is done**: all four stores are SQLite-first with JSON exports
(ADR-069 for the cards, ADR-070 for the rest, dev logs 96–97).

**Escalation rule — do not loop.** If Flash hits a subtle native failure it cannot localise in a few attempts
(a crash with a varying stack, a use-after-free, a frame/pipe corruption, an AddressSanitizer report it cannot
explain), it must **stop and say clearly „Pro modell szükséges" (Pro model needed)**, together with the evidence
collected so far — instead of burning more attempts on the same dead end. Flash is strong at coding and
reasoning but weak at fully autonomous multi-file bug-hunting, so a localised-but-unfixed bug should be handed
back up, not looped on. This rule is the whole point of the handover.

## 1. What the product is today

A runnable Windows build: a **Qt 6 shell** + a **CEF engine host** + a **Rust core** that owns the data. The grid is a
virtualised card view (503 cards → 13 DOM elements), a card hibernates to disk (MHTML + scroll/anchor/form state) and
restores offline, containers and workspaces are first-class, the profile is portable as one JSON bundle, and **all four
stores live in SQLite** (schema `user_version = 4`, WAL) as their only writer — the four `*.json` documents are derived
exports, and a pre-switch profile is adopted from them exactly once, per store, before the marker is written
(ADR-069/070). Decisions are recorded as **ADR-001 … ADR-070** in
[`docs/HU/05-DONTESEK.md`](docs/HU/05-DONTESEK.md); the development log runs to slice **97** in
[`docs/HU/16-FEJLESZTESI-NAPLO.md`](docs/HU/16-FEJLESZTESI-NAPLO.md).

## 2. How to verify (do this first, in a fresh session)

```powershell
git clone https://github.com/svandor/Antikolt && cd Antikolt

# 1. Fast, needs only Rust + Node: the same checks CI runs.
pwsh -File scripts/check-repo.ps1        # SPDX, links, JS parse, cargo fmt, clippy -D warnings, 78+45+5+1+15 Rust tests

# 2. The full gate: builds both native binaries and runs the 37 self tests (needs Qt 6.8 + CEF).
pwsh -File scripts/verify.ps1            # ends with "ALL SELF TESTS PASSED"

# 3. The portable build and a deployed sanity run.
pwsh -File scripts/dist.ps1 -Release
dist\Antikolt\antikolt-shell.exe --addcardtest dist\Antikolt\antikolt-engine-host.exe
```

Expected on a healthy host: `check-repo` ≈ 1 min, `verify.ps1` ≈ 6–8 min, `dist` ≈ 7 min. Numbers in the docs come
from the gate log, not from memory — that rule was learned the hard way (dev log 84).

## 3. The GPU blocker: resolved (measured), and the bug it was hiding

The `ContextResult::kFatalFailure: Failed to create shared context for virtualization` was **not** the GPU driver —
it was the escape hatch fighting itself. `ANTIKOLT_ENGINE_DISABLE_GPU=1` appended `--use-angle=swiftshader` and then
`--disable-software-rasterizer`, which switches SwiftShader straight back off; with `--disable-gpu
--disable-gpu-compositing` also in force, nothing could create the shared context. `apps/engine-host/src/host.cpp`
no longer appends `--disable-software-rasterizer` (nor the Linux-only `--disable-dev-shm-usage`).

VECTOR's **hardware** GPU path still aborts on its own (RTX 5080 + Intel iGPU, Chromium 158 beta; the GPU process
exits with `0x80000003`, and `--use-angle=d3d11` does not help), so this machine runs the windowed tests through
software rendering:

```powershell
$env:ANTIKOLT_ENGINE_DISABLE_GPU = '1'
pwsh -File scripts/verify.ps1
```

Measured after the fix: engine `--selftest` → `exit=0, gpu_exited=0, shared_context_fail=0, load=ok`;
`--embedtest` → `exit=0` in **2.6 s** with `embed=ok paint=ok focus=ok capture_size_match=ok`.

### The stdio lifecycle bug is also fixed; two smaller ones remain

The engine host reads the protocol from `stdin`, and Chromium closes the browser process's inherited stdin handle
while launching its child processes — so the host saw a premature `stdin closed` and exited mid-test. `protocol.cpp`
now reads from a private `DuplicateHandle` copy instead, so a CloseHandle inside Chromium cannot tear the link down.
Measured: `--notetest` → `exit=0` in 8.4 s, `--searchtest` → `exit=0` in 6.9 s, `--addcardtest` → `exit=0` in 11.9 s
(`delete_armed=ok delete_card=ok add_card_check=ok`).

The full gate is now **green**: `scripts/verify.ps1` reaches **`ALL SELF TESTS PASSED`** (with
`$env:ANTIKOLT_ENGINE_DISABLE_GPU='1'`, software rendering). The last windowed failure was not a renderer
stall but a frame-corruption bug: the engine wrote its protocol frames to a **text-mode `stdout`**, so a `0x0A`
byte inside a length header (the 4362-byte `cards.upsert` event is `0x110A`) was CRLF-translated, a 4-byte
header became 5 bytes, and the shell decoded a frame length of `1116685` — so the page's `cards.upsert` never
got a reply and its JS blocked the renderer. Fixed with `_setmode(_fileno(stdout), _O_BINARY)` in
`protocol.cpp` (dev log 94). Measured: `--containertest` → `exit=0` in 12 s, all `container_*` checks ok.

## 4. The next slices, in order

1. **Unblock the hardware GPU path** (the one real environment blocker left). Bisected (dev log 93): the default
   `d3d11` ANGLE backend crashes the GPU process on the RTX 5080, but `--use-angle=gl` keeps it alive — run on VECTOR
   with `$env:ANTIKOLT_ENGINE_EXTRA_SWITCHES='--use-angle=gl'`. The full hardware-GL gate is now **36 of 37**: every
   windowed check (including `containertest`) plus `coretest`, `sessionpanel`, the preview suite and `check-repo` pass,
   and the single miss is `capturetest`, which passes in isolation in 2.3 s (`capture=ok capture_size=ok`) — so it is a
   load-related transient timeout under the full gate, not a defect. A newer NVIDIA driver or forcing the Intel iGPU is
   still worth trying; the software-rendering gate is already fully green.
2. **Container-level permission/proxy (F3) — the engine half is done** (ADR-071, dev log 98): per-partition `CefRequestContext`, container proxy, permission policy denied by default, measured by `--partitiontest`. The settings editor on the grid is still missing, and so is the OAuth popup — the last two structural pieces before a release
   story (SignPath signing is a human step; see `docs/HU/20-ALASAS.md`). The store swap that used to be item 2 is
   **done** (ADR-070, dev log 97): all four stores are SQLite-first with JSON exports and a per-store adoption marker.

## 5. Rules that are not negotiable in this project

- **Never a dialog in the page** (ADR-053): CEF's `prompt` returns `null` silently, so every editor is inline.
- **Never a green that lies:** a check prints what it measured, and `not-measurable` with a reason beats a fake pass.
- **Write order matters:** an editor writes first and restores the DOM afterwards (ADR-062) — the `blur` handler
  re-enters otherwise and swallows the save.
- **Every store has one writer** (ADR-069/070): SQLite. The four `*.json` documents are exports, never imported once the
  file's `meta` table says the store was adopted; a write that closed the database and then filled the JSON would be the
  old mirror bug again.
  The other three stores are still JSON and write through their own store — do not mix the two directions.
- **Numbers in the docs come from the gate log**, never from memory.
- **Escalate instead of looping** (handover §0): the remaining work runs on DeepSeek V4.1 Flash; if a subtle native
  bug resists a few localisation attempts, stop and say „Pro modell szükséges" with the evidence — never burn
  attempts re-trying the same dead end.
- **Do not build strings with PowerShell `Replace` and write them blindly** — that destroyed `store.rs`, `main.rs`
  (twice) and `main.cpp` in this project; use the `write`/`edit` tools.

## 6. Where to look first

| Question | File |
|---|---|
| What works, with which evidence | [`docs/HU/21-ALLAPOT.md`](docs/HU/21-ALLAPOT.md) |
| Why a decision was made | [`docs/HU/05-DONTESEK.md`](docs/HU/05-DONTESEK.md) (ADR-001…070) |
| What happened, slice by slice, with outputs | [`docs/HU/16-FEJLESZTESI-NAPLO.md`](docs/HU/16-FEJLESZTESI-NAPLO.md) |
| Toolchain and its traps (Qt, CEF, MSVC) | [`docs/HU/17-ESZKOZLANC.md`](docs/HU/17-ESZKOZLANC.md) |
| The plan and the phases | [`docs/HU/04-UTEMTERV.md`](docs/HU/04-UTEMTERV.md), [`docs/HU/19-FOLYTATAS.md`](docs/HU/19-FOLYTATAS.md) |
| Wire protocol between shell, engine and core | [`proto/README.md`](proto/README.md) |
| What a shell test does and how to run it | [`apps/desktop-shell/README.md`](apps/desktop-shell/README.md) |

---

## Átadás magyarul

**Modell-átadás (ezt olvasd először):** a hátralevő munka a **DeepSeek V4.1 Flash**-re van bízva. A mély natív
hibakeresést a **DeepSeek V4 Pro** már lezárta (napló 90–94: a CEF-GPU kapcsoló, a stdio-handle, a use-after-free és a
szöveges-mód keret-torzulás). A Flash viszi a többit: a konténer-engedély/proxy + OAuth, a maradék termékfunkciók és a
kiadás. A **teljes store-csere kész** — mind a négy store SQLite-first, JSON-exporttal (ADR-069/070, napló 96–97).

**Eszkalációs szabály — ne ismételj fölöslegesen.** Ha a Flash olyan apró natív hibába ütközik, amit néhány
próbálkozásból nem tud lokalizálni (változó stackű krach, use-after-free, keret/pipe-torzulás, megmagyarázhatatlan
AddressSanitizer-jelentés), akkor **álljon meg, és mondja ki egyértelműen: „Pro modell szükséges"** — az addig
összegyűjtött bizonyítékkal együtt, ahelyett hogy újra és újra ugyanazt a zsákutcát próbálná. A Flash kódírásban és
következtetésben erős, de az önálló, többfájlos hibavadászatban gyenge — a lokalizált, de meg nem oldott hibát vissza
kell adni, nem hurkolni. Ez a szabály az átadás lényege.

**Igen, a munka átadható** — de csak akkor, ha a következő munkafolyamat nem találgat. Ez a fájl ezért kimondja:
mi a kész, mi **mért**, mi **nem mért**, milyen parancsokkal lehet ellenőrizni, és mi az az **egyetlen blokker**, ami
ezen a gépen megállítja a teljes kaput.

**Állapot:** a repó nyilvános (`github.com/svandor/Antikolt`), a `main` ág zöld CI-val, MPL-2.0 licenccel, magyar
(tekintélyes) és angol (összefoglaló) dokumentációval; a döntések ADR-001…070, a napló 97 szeletig.

**Első lépés az új munkafolyamatban:** `pwsh -File scripts/check-repo.ps1` (gyors, Rust+Node kell csak), majd a
teljes kapu **ezen a gépen**, szoftveres rendereléssel: `$env:ANTIKOLT_ENGINE_DISABLE_GPU='1'; pwsh -File
scripts/verify.ps1` (a hardveres GPU-út a VECTOR-on továbbra is elhal, lásd fent).

**A blokker (feloldva, mért):** a `Failed to create shared context for virtualization` nem a GPU-meghajtó volt,
hanem a saját kapcsolónk: a `ANTIKOLT_ENGINE_DISABLE_GPU=1` `--use-angle=swiftshader`-t kért, majd
`--disable-software-rasterizer`-rel ki is kapcsolta a SwiftShader-t. Javítva (`apps/engine-host/src/host.cpp`): a
motor `--selftest` → `exit=0, gpu_exited=0, shared_context_fail=0`; a `--embedtest` → `exit=0` 2,6 s alatt
(`embed=ok paint=ok focus=ok`). A VECTOR hardveres GPU-útja továbbra is elhal (RTX 5080 + Intel, Chromium 158
beta), ezért ezen a gépen szoftveres rendereléssel fut a kapu: `$env:ANTIKOLT_ENGINE_DISABLE_GPU='1'`.

**A stdio-hiba is feloldva (mért):** a motor a protokollt a `stdin`-jéből olvasta, a Chromium pedig a
gyerekfolyamatai indításakor bezárja a böngészőfolyamat örökölt stdin-handle-jét — ezért a motor a teszt közepén
`stdin closed`-ot látott és kilépett. A `protocol.cpp` most egy privát `DuplicateHandle`-másolatból olvas. Mérve:
`--notetest` → `exit=0` 8,4 s, `--searchtest` → `exit=0` 6,9 s, `--addcardtest` → `exit=0` 11,9 s
(`delete_armed=ok delete_card=ok add_card_check=ok`).

**A `containertest` krachja is feloldva (mért):** a Qt6Core access violation forrását **AddressSanitizerrel**
lokalizáltam (`pwsh -File scripts/build.ps1 -Asan -BuildDir build-asan`, új `ANTIKOLT_ASAN` opció): az `evaluate_json`
teszt-segéd a saját `QEventLoop`-ját referenciaként (`[&]`) fogta el, és a 20 s-os timeout lefutása után egy halott
loopon hívott `quit()`-ot. Javítva (`apps/desktop-shell/src/main.cpp`, `std::make_shared<State>`), a többi
`evaluate_json`/`evaluate_text` segédben is.

**A keret-torzulás is feloldva (mért) — ez volt a `containertest` utolsó akadálya:** a motor a protokoll-kereteit
**szöveges módú `stdout`-ra** írta, ezért a kerethosszban lévő `0x0A` bájtot a Windows CRLF-fé fordította (`4362 =
0x110A`, a 4 bájtos fejlécből 5 lett, a shell `1116685`-öt olvasott `4362` helyett). Javítva
(`protocol.cpp`: `_setmode(_fileno(stdout), _O_BINARY)`). **A teljes kapu most zöld: `ALL SELF TESTS PASSED`**
(`$env:ANTIKOLT_ENGINE_DISABLE_GPU='1'`), a `--containertest` → `exit=0` 12 s, minden `container_*` ok.

**A tároló kész (mért) — mind a négy store SQLite-first:** a kártyák, konténerek, munkaterületek és munkamenetek
**egyetlen írója** a `antikolt.sqlite3` (séma `user_version=4`, WAL), a négy `*.json` **export** (átvételkor,
leálláskor, `db_status(sync)`-ra), a tükör és a `sync_reference_tables` megszűnt. Az átvétel **store-onként** történik,
a `meta` tábla jelölőivel (`adopted:cards`, `adopted:containers`, …), így nem függ a megnyitás sorrendjétől; a `0004`
migráció egy v3 fájlban rögzíti, hogy a kártyák már átvételre kerültek (különben a `cards.json` visszaíródna az újabb
sorok fölé). A munkamenet-tábla a ledger teljes sorát hordozza, a házirendek egy egy-soros `session_rules` táblában.
Mérve: `cargo test` **129 → 144** teszt zöld (78+45+5+1+15), köztük a store-onkénti átvétel, az „üres dokumentum üres
marad", a korrupt dokumentum, az export-ellenőrzés, a v2/v3 fájl átvétel-jelölői és a négy store e2e útja; a kapu
`db_status=ok schema=4 … verified=yes` (mind a négy exportot soronként ellenőrzi). ADR-070, napló 97. szelet.

**Következő szeletek:** (1) a hardveres GPU-út a VECTOR-on — bisectelve (93. szelet): a d3d11 ANGLE-háttér a hibás
a 5080-on, a `--use-angle=gl` megy (`$env:ANTIKOLT_ENGINE_EXTRA_SWITCHES='--use-angle=gl'`); a maradék driver-szintű
(frissebb NVIDIA-driver vagy az Intel iGPU kényszerítése); (2) konténerenkénti engedély/proxy (F3), majd OAuth-popup —
utána a kiadás (SignPath, emberi lépés).

**Szabályok, amiket nem szabad újranyitni:** nincs dialógus a lapon (ADR-053); soha nem hazudó zöld (a
`not-measurable` jobb a hamis passznál); az editor előbb ír, aztán állítja vissza a DOM-ot (ADR-062); **minden
store-nak egy írója van** (ADR-069/070: az adatbázis; a négy `*.json` export, és a `meta` jelölő után soha nem
importálunk vissza belőlük); a dokumentumok számait a kapu naplójából kell venni; és
**PowerShell `Replace`-csel előállított szöveget soha nem szabad vakon kiírni** — ez a projektben háromszor törölt fájlt.

