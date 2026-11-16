# Windows Code Signing & SmartScreen Report (2026)

## 1. SmartScreen behavior on Windows 11

| Certificate status | First-download user prompt | Reputation accumulation |
|---|---|---|
| **Unsigned** | "Windows protected your PC" — must click "Run anyway" | None; each new binary starts at zero |
| **OV/EV signed** | Verified publisher name shown; still shows warning until reputation accumulates | Reputation builds over time via download volume; same process for OV and EV |
| **EV only** | Same as OV — no "instant" bypass | EV behavior removed 2024; premium no longer justified for SmartScreen alone |
| **Microsoft Store** | ✅ No warning — Microsoft re-signs | N/A |
| **Self-signed / No signature** | Same as unsigned | — |

**Source**: Microsoft Learn — [SmartScreen reputation for developers](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation) (updated 2026-11-10). **VERIFIED**.

> Note: EV no longer bypasses SmartScreen. Paying a premium for EV solely to avoid SmartScreen warnings is no longer justified.

## 2. Certificate options & real prices 2026

| Type | Cost | Individual-friendly? | HSM requirement | SmartScreen impact |
|---|---|---|---|---|
| **OV code signing** | $150–300/year | Yes (outside USA/Canada needs OV) | ✅ Required since June 2023 (USB token or cloud HSM) | Warning until reputation accumulates |
| **EV code signing** | $400+/year | Yes | ✅ Required | Same as OV; premium not justified for SmartScreen |
| **Azure Artifact Signing** | ~$9.99/mo | ✅ Individuals: USA/Canada only; Organizations: USA/Canada/EU/UK | ❌ None — CI-integrated | Same reputation build as OV; lower cost |
| **Self-signed** | Free | No (blocks public installs) | N/A | Same as unsigned |

**Sources**: MS Learn code signing options (2026-11-10) **VERIFIED**; SignPath terms **VERIFIED**; Azure Q&A on country restrictions **VERIFIED** (France individual onboarding paused; only USA/Canada).

> **HSM operational cost**: USB token purchase (~$50–$150 one-time) or cloud HSM per-use fees. CI must provision the token/secret at runtime.

## 3. Azure Artifact Signing (formerly Trusted Signing)

| Aspect | Detail |
|---|---|
| **Pricing** | ~$9.99/month (estimates; actual varies by agreement/currency) |
| **Eligibility** | Organizations: USA, Canada, EU, UK. Individuals: USA and Canada only. Individual onboarding **paused** as of March 2026. |
| **File types** | Authenticode: PE/EXE, MSI, MSIX |
| **Hardware token** | ❌ Not required — signs directly from CI/CD (GitHub Actions, Azure DevOps) |
| **SmartScreen** | Does **not** give instant trust. Reputation builds organically over time with consistent publisher identity. |
| **Docs** | [learn.microsoft.com/en-us/azure/trusted-signing/](https://learn.microsoft.com/en-us/azure/trusted-signing/) |

**Source**: Azure pricing page + Microsoft Q&A **VERIFIED**.

## 4. SignPath.io / SignPath Foundation (free OSS)

| Condition | Requirement |
|---|---|
| **License** | OSI-approved license, no commercial dual-licensing |
| **Code signing policy** | Must be published on project home page (specific format required) |
| **Eligible projects** | Actively maintained, released, documented, no malware |
| **Certificate** | Issued to "SignPath Foundation"; project bins signed via their pipeline |
| **Catch** | Every release requires manual approval; technical constraints on artifact configuration; SignPath may pause/terminate for policy violations |
| **Cost** | Free |

**Source**: signpath.org/terms.html **VERIFIED**.

> **Build environment**: SignPath provides a GitHub Action that submits your release artifacts to SignPath for signing; your CI does not need to run in SignPath's environment, but you must use their GitHub Action.

## 5. Other OSS-friendly options

| Option | Cost | SmartScreen / Reputation |
|---|---|---|
| **Sigstore** | Free | Proves file came from GitHub account via `--cert-identity`. No Windows SmartScreen bypass, but users can verify cryptographically. |
| **Microsoft Store (MSIX)** | Free (MSIX only) | ✅ No warnings — Microsoft re-signs automatically |
| **winget-pkgs** | Free | ❌ Installer need not be signed, but manifest must have correct `InstallerSha256` and `InstallerUrl`. Unsigned executables won't establish SmartScreen reputation. |
| **Let's Encrypt equivalent** | ❌ None | No free PKI equivalent for code signing exists. |

**Sources**: Sigstore dev.to tutorial **DOCUMENTED**; winget ADR from sylin-org/koi **DOCUMENTED**.

## 6. Linux side (RPM/Flatpak)

| Ecosystem | Signing requirement | Review / Distribution |
|---|---|---|
| **RPM/Fedora** | GPG key on repo metadata (`repo-gpgkey`); `dnf` verifies signatures | Standard distro practice; no "SmartScreen" equivalent |
| **Flatpak/Flathub** | ❌ No code signing requirement for binaries | Review focuses on sandbox compliance, open source license, no trademarks, sufficient development history, distributed verifiably. Browser-like apps accepted if they meet inclusion policy. |
| **Flatpak verification** | Optional metadata provenance; not a gate | [docs.flathub.org](https://docs.flathub.org/docs/for-app-authors/requirements) |

**Source**: Flathub requirements doc **DOCUMENTED**.

## 7. Auto-update signing (no paid cert)

| Approach | What's signed | How verification works | OSS typical usage |
|---|---|---|---|
| **minisign / Ed25519** | Release artifacts (.exe, .tar.gz, .AppImage, .msi) via `.minisig` detaches | Client has embedded public key; `minisign -V -p key.pub -m file` | [paneflow runbook](https://github.com/arthurdev44/paneflow/blob/main/docs/self-update-signing.md) **DOCUMENTED** |
| **Sigstore** | .exe via `sigstore sign` + `sigstore-bundle.json` | `sigstore verify github file --bundle bundle.json --cert-identity https://github.com/User` | [dev.to Huckler tutorial](https://dev.to/huckler/how-to-sign-your-python-exe-with-sigstore-no-certificate-required-m5l) **DOCUMENTED** |
| **Authenticode + manifest** | Installer + update .yml/.json manifest | WinVerifyTrust chains to trusted root; manifest hash checked by updater | Paid cert required for trusted publisher chain |
| **Tauri Ed25519** | Artifacts in CI; dual-key rotation for zero-downtime key rotation | Client embeds current+next public keys; fail-closed on mismatch | [Tauri ADR](https://github.com/koedame/chordsketch/blob/main/docs/adr/0005-tauri-updater-key-management.md) **DOCUMENTED** |

**Typical OSS workflow**: Sign release artifacts with minisign/Ed25519 + publish Sigstore bundle for GitHub provenance. Users verify either or both. OS-native checks (WinVerifyTrust) still apply on top.

---
*Confidence markers: VERIFIED = official Microsoft doc or primary source confirmed; DOCUMENTED = reputable third-party source (SignPath, Sigstore, Flathub, community ADRs).*

**Bottom line for EU individual on a budget**: Start with **SignPath Foundation** (free, if OSI-licensed project) or **Sigstore** (free, no license restriction). If you need Authenticode for Windows-native trust, **Azure Artifact Signing** at ~$9.99/mo is the lowest-cost paid option — but only if you're in USA/Canada (individual) or an org in EU/UK. Otherwise, **OV certificate** ($150–300/yr) with HSM token is the traditional path.