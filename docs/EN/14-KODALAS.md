# 14 — Code signing: how much it costs, and what happens without signing (Q14 in detail)

Short answer: **signing is not mandatory, but it strongly affects the user experience on Windows.**
The good news: an open-source project has a **free** route (SignPath Foundation), and the updates'
integrity can also be guaranteed with a free tool (Ed25519/minisign). The bad news: the SmartScreen
warning will still appear in the first months, because Microsoft collects "reputation", not the signature.

## 1. What happens if you release without signing?

| State | What the user sees | Consequence |
|---|---|---|
| **Without signing** | "Windows protected your PC" blue warning → "More info" → "Run anyway" | Many give up here; it erodes trust |
| **With a self-signed certificate** | The same warning, plus an unknown publisher | Improves nothing, only complicates |
| **With an OV certificate** | The publisher name is shown, but the warning **remains until reputation builds up** | Improves, but not immediately |
| **With an EV certificate** | The same as with OV | **Since 2024 even EV does not bypass SmartScreen** — the premium price is not for this |
| **Via the Microsoft Store** | No warning (Microsoft re-signs it) | This is the cleanest route, but Store dependency |

Source: Microsoft Learn, *SmartScreen reputation for developers*.

## 2. Certificate types and real 2026 prices

| Type | Cost | For individuals? | Hardware key needed? | SmartScreen |
|---|---|---|---|---|
| **OV code signing** | $150–300 / year | yes (in the EU too) | **yes** (USB token or cloud HSM; mandatory since June 2023) | reputation builds |
| **EV code signing** | $400+ / year | yes | yes | the same as OV |
| **Azure Artifact Signing** (formerly Trusted Signing) | ~$10 / month | **individual: USA/Canada only**; organization: USA/Canada/EU/UK | no (signs from CI) | reputation builds |
| **SignPath Foundation** | **free** (for an OSI-licensed open-source project) | yes | no (in their pipeline) | reputation builds, the publisher is SignPath Foundation |
| **Sigstore** | free | yes | no | no SmartScreen effect, but cryptographically verifiable |
| **Self-signed** | free | — | — | improves nothing |

**Critical detail for an individual in the EU:** Azure Artifact Signing **is not available to individuals**
(USA/Canada only), but it is available to organizations. So if it later becomes a company, this is the cheapest route;
as an individual, the two realistic options are: SignPath Foundation (free) or an OV certificate (~$150–300/year + token).

## 3. The recommended route (cost-optimized)

### v0.x — internal/dogfooding stage
- **No signing.** You install the installer yourself; the SmartScreen warning is not a problem.
- **BUT**: the updater should already verify the package with an **Ed25519/minisign-signed manifest**
  (the public key baked into the application, "fail-closed": if it doesn't match, it doesn't update).
- This part is free, and it remains the backbone of the update chain later on.

### v1.0 — public release, **on a free basis, with automated updates**

The goal: **zero certificate cost, and updates going out without human hands.** This can be solved with three separate layers
— and it is important to see that the three layers are **independent** of each other:

| Layer | What it protects | Automatable? | Cost |
|---|---|---|---|
| **1. Update integrity** (the most important) | the authenticity of the downloaded package | **fully** (CI-generated Ed25519 signature on the manifest) | 0 |
| **2. Provenance** | the binary was built from the public source in the public CI | **fully** (Sigstore/cosign + GitHub build attestation) | 0 |
| **3. Authenticode** (Windows "publisher" name) | the SmartScreen/trust chain | **partly**: the SignPath signature requires one manual approval per release | 0 |

**Layer 1 — the updater chain (fully automated, free):**

1. CI builds → produces the `latest.json` manifest (version, platform, SHA-256, release note).
2. CI **signs the manifest with an Ed25519 key** (the private key is an encrypted secret in CI, the public
   key baked into the application).
3. The application at update time: manifest download over HTTPS → **signature verification** → SHA-256 check
   → download → verify again → **atomic swap** → on error, **rollback to the previous version**.
   "Fail-closed": if anything is wrong, it does not update.
4. **Two keys** (current + next) in the application, so that key rotation does not break installations.
5. Gradual release: "channel" and "minimum version" fields in the manifest; a broken release
   can be **recalled** (kill switch).

This layer **never requires a human**, and this is what gives updates their security. Compared to this, Authenticode
only improves the Windows *user experience* (fewer warnings), not the actual protection.

**Layer 2 — Sigstore/provenance (automated, free):** the Sigstore signing of release artifacts and the
GitHub build attestation are free and fully automated; anyone can verify that the binary comes from the public
repo and the public CI. This is the proof that "you could not have downloaded it from elsewhere".

**Layer 3 — Authenticode (SignPath Foundation, free, 1 click per release):**

- Requirement: **OSI license** (MPL-2.0 qualifies), public repo, published "code signing policy" on the
  project website.
- The GitHub Action submits the artifact; **approving the release is one click** on the SignPath interface —
  this is the price of being free (it cannot be fully automatic).
- The certificate is issued in the name of *SignPath Foundation* → SmartScreen shows this as the publisher; this needs to be
  explained on the website.
- **If even this click does not fit:** we skip Authenticode, and layers 1+2 remain (updates
  remain automated and protected), only SmartScreen keeps warning. This is also entirely
  acceptable — we will make this decision before v1.0.

### What I do NOT recommend
- **Any paid certificate** (OV/EV): your request is clear, and the added value since the 2024
  SmartScreen change is small.
- **Self-signed certificate**: for the user it is just as bad as no signing, only more confusing.
- **Store-only distribution**: the Store would exclude our own updater and our own installer.

## 4. Linux (Nobara/Fedora) and Flatpak

| Channel | Requirement | Note |
|---|---|---|
| **RPM (own repo)** | GPG-signed repo metadata (`repo-gpgkey`), verified by `dnf` | The least friction; the key must be published on the website |
| **RPM (Copr / Fedora repo)** | Copr signs it itself | simple, but a dependency |
| **Flatpak / Flathub** | **no code-signing requirement**; the review checks sandbox compliance, the license, and the "no trademark infringement" condition | a browser-like app is acceptable if it meets the inclusion policy |

## 5. What to prepare for (communication)

- In the first **weeks/months SmartScreen will warn**, even if signed. This is not a bug.
  A short explanation should go on the website and alongside the installer ("new publisher, Windows is collecting reputation").
- The **publisher name** (SignPath Foundation) differs from the product name — this too must be explained, otherwise
  it looks suspicious.
- The update **never installs without signature verification**, and there should be a plan for key rotation
  (two keys: current + next, so that rotation does not break every installation).

## 6. Cost summary

| Release | v0.x | v1.0 (free) | If it is ever needed anyway |
|---|---|---|---|
| Certificate | 0 | **0** (SignPath Foundation) | OV ~$150–300/year (not recommended) |
| Hardware token | 0 | **0** | for OV ~$50–150 one-time |
| Update signing (Ed25519) | 0 | **0** | — |
| Provenance (Sigstore) | 0 | **0** | — |
| CI integration | 0 | GitHub Action (free) | — |
| Human work | 0 | **1 click** per release (SignPath approval) | — |
| SmartScreen warning | yes | in the first period yes, later it fades | — |

**Decision (per your request):** **zero cost**, free certificate (SignPath Foundation) +
**a fully automated update chain** (Ed25519 + Sigstore). No paid certificate is in the plan; if the
SignPath approval is still bothersome, Authenticode is left out, and updates remain automated and
protected anyway.
