# Security policy

A browser holds everything: sessions, passwords, reading history. A defect here is not a bug, it is
the user's risk. Please report it privately first.

## Reporting a vulnerability

- **Preferred:** GitHub *Security → Report a vulnerability* on this repository (private advisory).
- **Alternative:** the security contact published on the project website.
- Do **not** open a public issue for a vulnerability, and do not include secrets, cookies or
  personal data in a report.

## What to expect

| Step | Target |
|---|---|
| Acknowledgement | within **72 hours** |
| Initial assessment (severity, affected versions) | within **7 days** |
| Fix for a critical issue | within **30 days**, or a documented mitigation |
| Coordinated disclosure | after the fix ships, with credit if you want it |

There is **no bug bounty**; there is also no NDA, no legal threat and credit is given by default.

## In scope

- The Rust core (`crates/`): identifier handling, snapshot manifests, migrations, session policy,
  and later the vault and the IPC implementation.
- The engine host protocol (`crates/antikolt-ipc`, `proto/`): framing, validation, trust boundaries.
- The Qt/CEF shell once it exists: window embedding, sandbox configuration, permission handling.
- The default home page (`home/`): the bridge allowlist, CSP, sanitisation.
- Packaging and update integrity: signatures, provenance, update manifests.

## Out of scope (report upstream)

- Vulnerabilities in Chromium/CEF itself (report to the Chromium project).
- Vulnerabilities in Qt (report to the Qt project).
- Vulnerabilities in a website you visited.

## Engineering rules this policy relies on

1. Untrusted input is named as such and fuzzed: MHTML snapshots read from disk, IPC frames, imported
   bookmarks and password files (`docs/HU/15-HOZZAJARULAS.md` §2.3).
2. Secrets never enter the page's main world, a log file or a crash report (invariant I5,
   `docs/HU/02-ARCHITEKTURA.md` §1).
3. There is no telemetry and no automatic network report (ADR-015).
4. Releases are signed and their provenance is published (ADR-024).
5. Security-relevant behaviour changes require an ADR (`docs/HU/05-DONTESEK.md`).

## Supported versions

Only the latest release receives security fixes. Pre-release builds are unsupported by definition.
