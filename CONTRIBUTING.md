# Contributing to Antikolt

Thank you for helping. This project handles a browser's attack surface, so the process is short but
strict on a few points.

**The short version:** anyone can contribute, no CLA, no permission needed. Sign off your commits
(`git commit -s`), keep the CI green, and expect review on the security-sensitive paths.

## 1. Getting set up

| Tool | Why |
|---|---|
| Rust (stable, 1.80+) | the core (`crates/`) |
| MSVC Build Tools (Windows) or a C toolchain (Linux) | linking Rust binaries |
| Node 22+ | only to syntax-check the home page |
| CMake + Qt 6 + CEF | the native shell and engine host (`scripts/build.ps1`) |

```sh
git clone https://github.com/antikolt/antikolt
cd antikolt
cargo test --all          # 166 tests
cargo clippy --all-targets -- -D warnings
cargo fmt --all
node --check home/app.js
```

Or run everything the CI runs, in one command:

```powershell
pwsh -File scripts/check-repo.ps1        # SPDX, links, hygiene, JS, Rust
pwsh -File scripts/check-repo.ps1 -SkipRust
```

To build the native targets (Qt shell + CEF engine host):

```powershell
pwsh -File scripts/build.ps1             # configure + build + both self tests
```

The toolchain (Qt 6, CEF) is installed under `D:\Tools`; see `docs/HU/17-ESZKOZLANC.md`.

If your machine cannot reach crates.io, `cargo test --offline` works when the needed crates are in the
local registry cache (this is how the project was first built).

## 2. Sign-off (DCO)

Every commit needs a `Signed-off-by` line:

```sh
git commit -s -m "core: explain why the freeze deadline is 2.5s"
```

That line means: you wrote the change, or have the right to submit it, and it may be distributed
under MPL-2.0. You keep your copyright. There is no CLA and no copyright assignment — which also means
the licence cannot be changed unilaterally later (`docs/HU/13-LICENC.md`).

## 3. What gets reviewed strictly

Changes under these paths need maintainer review before merge, always
(`docs/HU/15-HOZZAJARULAS.md` §2.1):

```
crates/antikolt-core/src/vault/      (when it lands)
crates/antikolt-core/src/ipc/
crates/antikolt-core/src/snapshot/
crates/antikolt-core/migrations/
crates/antikolt-ipc/
apps/desktop-shell/src/platform/
proto/
```

## 4. Code rules

- **Comments in English, in the code.** Documentation is Hungarian (authoritative) and English.
- **No `unsafe`** in the core crates (`#![deny(unsafe_code)]`); if it becomes unavoidable, it needs an
  ADR and a safety comment.
- **No panics on untrusted input.** Return a typed error; the parsers are fuzzed.
- **Every behavioural rule gets a test**, especially the timers and the session policy — those are
  pure functions and have no excuse.
- **Numbers live in one place.** Tuning values belong to `profile.rs`, not scattered through the code.
- **Don't invent APIs.** If an engine capability is missing, report a missing `Capability` and degrade
  gracefully.

## 5. Using AI tools

Allowed and welcome. You do not have to avoid them. Please answer the AI question in the pull request
template (yes/no/partly) — it is for provenance, not for judgement.

## 6. Commit messages

`area: imperative summary` — e.g. `core: reject snapshots with control characters in the url`,
`home: virtualise the card grid`, `docs: record the quota decision`. Body explains *why*, not *what*.

## 7. Pull requests

- One concern per PR; a PR that mixes a refactor with a behaviour change will be asked to split.
- Fill in the template, including "how did you verify this?" — "tests pass" is expected, not
  sufficient for timer or memory behaviour.
- Add an ADR (`docs/HU/05-DONTESEK.md`) for anything irreversible or user-visible in a surprising way.
- Keep the CI green; do not merge with skipped tests.
