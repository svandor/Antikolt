# 01 — Questions and decisions

The questions that decide the design. **The development is done by the assistant (AI agent)**, so the technical
questions are decided by me (with justification), and only those questions remain open that require **your
preference**. Every decision is also recorded as an ADR in `05-DONTESEK.md`.

## 1. Answered questions (decision)

| # | Question | Decision | Where it is explained |
|---|---|---|---|
| Q1 | Who develops, in what language? | **The assistant**; the main priority is **stability and low resource usage**. Language: C++20 + Qt 6 shell, Rust core, CEF engine | `02`, ADR-002, ADR-021 |
| Q2 | What machine must it run on? | **VECTOR** (Ultra 9 275HX, 32 GB, RTX 5080 16 GB + Arc iGPU, 2 displays, 1 TB) is the reference; the product's goal is to be usable even on an 8 GB machine | `09-CELHARDVER.md` |
| Q4 | Warm engine or stopped? | **Profiles** (Fast / Balanced / Power saving / Ultra / Manual), with automatic switching; measurement decides | `09` chapter 3, ADR-020 |
| Q5 | Native window or OSR? | **Windows: native window; Linux/Wayland: OSR** — behind the same abstraction | `11-OSR.md`, ADR-022 |
| Q8 | 24-hour deletion | **`keep` / `sweep` / `wipe` policy**; login is kept by default, because it does not live in RAM | `10-MUNKAMENET.md`, ADR-009 |
| Q9 | Is a passkey needed? | **v1.0: software passkey in the vault** (CDP path, ADR-027), hardware only later — CEF has no built-in WebAuthn | `12-PASSKEY.md`, ADR-027 |
| Q10 | License | **MPL-2.0**, dynamically linked Qt, BSD-3 CEF, DCO, AI statement | `13-LICENC.md`, ADR-014 |
| Q14 | Code signing | **SignPath Foundation** (free, for an OSI license) + **Ed25519/minisign** update signing; EV no (no advantage since 2024) | `14-KODALAS.md`, ADR-024 |

Your answers to the earlier five questions (engine: CEF/Qt, disk hibernation, 10 minutes, start page, themes) are also
recorded — see `00-JAVASLATOK.md` chapter 0.

## 2. Decisions (based on your answers)

| # | Question | Decision | Consequence |
|---|---|---|---|
| N1 | The default of the 24-hour policy | **`sweep`** remains the container default (login is kept), `keep`/`wipe` is selectable per card | according to `02` 4.3. and ADR-009; in practice we will fine-tune it later |
| N2 | Accepting a software passkey | **yes** | v1.1 passkey in the vault (path B); the drawbacks are documented in `12-PASSKEY.md` point 3.1. |
| N3 | Is there a passkey-only account? | **to be checked** — `12-PASSKEY.md` point 2.5. describes what it means and how to check it in 2 minutes | if there is such an account: passkey research goes into F0 (before v1.0) |
| N4 | License | **MPL-2.0 + DCO** (my proposal, see `13-LICENC.md` 1.1.) | anyone can contribute freely, no CLA; the license cannot be changed unilaterally later |
| N5 | Disk quota | **50 GB default**, overridable per machine | can be set to 100 GB on the VECTOR, if needed |
| N6 | Starting code signing | **free certificate** (SignPath Foundation) + **automated** updates (Ed25519 + Sigstore); no paid certificate is planned | 1 click per release for SignPath approval; if even this is not wanted, Authenticode is left out |
| N7 | Name | **Antikolt** | reserving the domain + GitHub organization is recommended |

### 2.1. Your only task: the passkey-only check

According to `12-PASSKEY.md` point 2.5., check in your Google and Microsoft accounts whether you **deleted the
password** (that is, you can only log in with a passkey). If so, passkey research moves **before** v1.0; if not,
the A → B order remains, and v1.0 is enough for most accounts.

### 2.2. What you need to know about the license (for your "what does it mean?" question)

In short: under **MPL-2.0** anyone may use, modify and distribute the browser (companies too),
**anyone may contribute without special permission** (with DCO, without a CLA), but our modified *files* must
be shared just as openly. Your own new files may be closed. The detailed, plain-language
table is in `13-LICENC.md` point 1.1.; and the contribution and security rules are in
`15-HOZZAJARULAS.md`.

## 3. What I decided for you (with justification)

These are technical decisions; if you disagree with any of them, say so and I will rework it.

| Topic | Decision | Why |
|---|---|---|
| Platform order (Q3) | Windows 11 for v1.0; Linux (Nobara/KDE/Wayland) from v0.3 | the Wayland integration is a separate risk; a usable product will come sooner on Windows |
| Engine embedding (Q5) | Windows native window, Linux OSR | Wayland does not reliably allow embedding a foreign window (`11-OSR.md`) |
| Start page | local HTML (`antikolt://home`), with an `<antikolt-card>` component | "it can look any way" can only be achieved with HTML/CSS/JS |
| Vault format (Q7) | own SQLite + DEK/KEK, plus **KeePass `.kdbx` export/import** | container binding and TOTP require our own schema; export opens it up to other devices |
| Import (Q15) | passwords from CSV, bookmarks from HTML; **no cookie import** | because of Chrome 127+ cookie encryption, a third party cannot decrypt them |
| Telemetry (Q13) | **zero**; local crash log, with manual sending | data-conscious identity, GDPR simplicity |
| Language policy (Q12) | code/commits in English; documentation HU (authoritative) + EN | international contribution, Hungarian plan for Hungarian users |
| AI features (Q16) | no built-in cloud AI; only a local model or your own key, disabled | the browser's data-consciousness promise cannot be undermined with cloud AI |
| v1 user (Q17) | first you (dogfooding), public release only after | this is the best quality assurance |
| Browser basics | mandatory in v1 (downloads, Ctrl+F, zoom, PDF, PiP, printing, crash recovery, accessibility) | without these it cannot be a daily browser |
| Multi-engine support | adapter from day one, the second engine only after CEF stabilizes | two unstable integrations cannot be carried at once |
| Extension system | none in v1 | every extension is its own process/memory, and it violates I1 |
| Card count upper limit | none (the grid is virtualized) | disk space and search scale, RAM does not grow |
