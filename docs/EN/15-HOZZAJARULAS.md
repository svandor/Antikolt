# 15 — Contribution and security governance

> The user expectation: **"anyone should be free to contribute, because a browser can carry security
> risks."** This document translates that into working rules: how to join in, what the
> review process is, and how we handle security risks without building bureaucracy.

## 1. Who can contribute, and how?

**Anyone, anytime, without special permission.** The license (MPL-2.0) and the contribution model (DCO)
ensure this:

| Step | What you must do |
|---|---|
| 1. Code | fork → branch → commit. For new files, an `SPDX-License-Identifier: MPL-2.0` header. |
| 2. Contribution | `git commit -s` (Signed-off-by) — one line that certifies you are entitled to submit the code. **No CLA, no signature, no registration.** |
| 3. Submission | a pull request with the template (what changed, how you tested it, whether it touches a security area). |
| 4. Review | the maintainer (currently the project owner + the assistant) reviews it; automated gates run. |
| 5. Merge | after green CI + approval. |

**What we do not ask for:** we do not ask for copyright transfer (CLA), we do not ask for corporate approval, we do not ask for
money. The contributor keeps their copyright, the code stays under MPL-2.0.

## 2. Security governance (because "browser = risk")

In a browser, a bug = risk for the user. Therefore the process is based not on trust, but on **gates**.
These gates are:

### 2.1. Protected areas (CODEOWNERS)

In these folders, changing anything requires **mandatory maintainer approval**, and it cannot be merged by
"fast-tracking":

```
/crates/antikolt-core/src/vault/     # vault, key management, cryptography
/crates/antikolt-core/src/ipc/       # engine-house protocol, message validation
/crates/antikolt-core/src/snapshot/  # snapshot writer/reader (untrusted input!)
/crates/antikolt-core/src/db/migrations/
/apps/desktop-shell/src/platform/    # window embedding, sandbox configuration
/proto/                              # IPC schema
```

### 2.2. Automatic gates (on every PR)

| Gate | What it catches |
|---|---|
| Compilation + full test suite | basic bugs |
| **Fuzz test** on untrusted inputs (MHTML parser, IPC messages, snapshot manifest) | crashes, memory errors, infinite loops |
| **Measured memory regression** (a short scenario from the `perf/` benchmark) | silent erosion of the memory promise |
| Static analysis (clippy, clang-tidy, `cargo-audit`, `cargo-deny`) | known vulnerable dependency, suspicious pattern |
| Secret scanning | a key that got in by accident |
| License header check (SPDX) | legal cleanliness |

### 2.3. What counts as untrusted input (and therefore gets special attention)

1. **Any website** — the engine sandbox protects, but our engine adapter and IPC are the boundary.
2. **The snapshot files** — MHTML/manifest read back from disk: they can be corrupted, modified, or malicious
   (e.g. if someone sneaks an unsigned file into the blob store). → schema validation + integrity hash +
   fuzzing.
3. **The home page** — the user's own HTML: strict CSP, allowlist bridge, sanitization.
4. **Data imported by the user** (password CSV, bookmark HTML) — we never execute code
   from them; we treat them only as data.

### 2.4. Vulnerability disclosure (SECURITY.md)

- **Private channel** for reports (GitHub Security Advisory, e-mail on the website).
- **72-hour acknowledgement**, a 30-day fix target for critical bugs.
- **Coordinated disclosure**: after the fix, a joint announcement, with the reporter's name (if requested).
- **No bounty** — there is no budget for it; instead, thanks in the release notes.
- The release notes list every security fix separately.

### 2.5. Release security

- Releases are protected by an **Ed25519/minisign**-signed manifest (the updater checks it fail-closed) — see
  `14-KODALAS.md`.
- The packages' **SHA-256** hash is on the website and in the GitHub Release.
- **SLSA/Sigstore provenance** for the build (automated, free) — proves that the binary was built from
  the public source, in the public CI.
- Rollback: the updater must be able to "restore the previous version" and to revoke a corrupted
  release (a kill switch in the manifest).

## 3. How do we handle AI contribution?

The project's code is written by an AI agent (ADR-021), and user code may also arrive with AI assistance. That is not a problem,
but it **must be visible**:

- `SPDX-License-Identifier: MPL-2.0` in every file.
- The `NOTICE` states: "part of the code was produced with AI tools, and the author reviewed and tested it."
- The `AUTHORS` names the human contributors and the development tool; we do not list AI as an "author."
- One question in the PR template: "was this made with AI assistance, and which part?" — not a ban, but transparency.
- The headers do not claim exclusive human authorship.

This practice protects the project (provenance) and the contributor (they need not prove their authorship).

## 4. What we deliberately do NOT do

- **No CLA** — contribution is free and immediate; the price is that the license cannot later be changed
  unilaterally (see `13-LICENC.md`).
- **No mandatory two-person review** — in a one-person project that would paralyze development; instead,
  protected areas + automated gates + the maintainer's responsibility.
- **No "security through obscurity"** — the code is open, and the threat model is open too (`02`, chapter 14).
- **No silent change** to security behavior — every such change gets an ADR.
- **No unjustified extra window, and none stays alive.** A browser has one visible window; the
  engine and core processes talk over hidden pipes, the tray icon is an indicator and not a window, and the program discards
  the console window that Windows gives it for shortcut launching (`DropOwnConsoleWindow`). This is the user's
  explicit rule (2026-11-18), and it is a **review question** for every change that creates a process or
  window: why it is needed, and why the same cannot be done without a window. The measurement: `--traytest`,
  `--themetest`, plus counting the windows of a real launch.

## 5. The next step

The `CONTRIBUTING.md`, `SECURITY.md`, `CODEOWNERS`, PR and issue templates, and the fuzz targets
are produced **together with the v0.1 codebase** (at the start of F1). This document is the rule; the files are the implementation.
