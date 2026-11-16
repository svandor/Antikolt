<!-- SPDX-License-Identifier: MPL-2.0 -->
## What does this change?

<!-- One or two sentences. Link the issue or ADR if there is one. -->

## Which area?

- [ ] Rust core (`crates/`)
- [ ] IPC protocol (`crates/antikolt-ipc`, `proto/`)
- [ ] Schema migration (`migrations/`) — *append-only, never edit a shipped migration*
- [ ] Home page (`home/`)
- [ ] Shell / engine host (Qt + CEF)
- [ ] Documentation (`docs/`)
- [ ] Build, CI, packaging

## How did you verify it?

<!-- "cargo test" is expected, not sufficient for timers, session policy or memory behaviour.
     Show the command, the number, or the measurement. -->

- [ ] `cargo test --all`
- [ ] `cargo clippy --all-targets -- -D warnings`
- [ ] `cargo fmt --all`
- [ ] `node --check home/app.js` (if the home page changed)
- [ ] Manual check: …

## Does it touch a security-sensitive path?

<!-- See CODEOWNERS and docs/HU/15-HOZZAJARULAS.md §2.1 -->

- [ ] No
- [ ] Yes — which one, and what did you consider?

## Rules that must not be broken

- [ ] Still **at most one** card surface (`docs/HU/02` invariant I1)
- [ ] No secret reaches the page's main world, a log or a diagnostic bundle (I5)
- [ ] No telemetry, no network access from the home page
- [ ] A new tuning number lives in `profile.rs`, not inline

## AI assistance disclosure

<!-- For provenance only, never a reason to reject. -->

- [ ] No AI tools used
- [ ] AI assistance used (describe briefly which parts)

## Checklist

- [ ] Commit is signed off (`git commit -s`)
- [ ] Docs updated if behaviour or decisions changed (and an ADR added for irreversible choices)
- [ ] SPDX header present on new source files
