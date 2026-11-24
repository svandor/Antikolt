# Signing — how to obtain a certificate, and what to do

> **The decision is recorded:** [ADR-040](05-DONTESEK.md) — two channels: **Store (MSIX, signed by Microsoft)** and **SignPath (our own portable release)**.

> This page describes **what route can be used to produce a signed Antikolt exe today (2026)**, how much it costs, and what
> needs to be done in the repo. The short answer: **a free, publicly accepted code-signing certificate is available on only one
> route: the SignPath Foundation** (for open-source projects), and there the key is not on our machine.

## Why you cannot just "request a free certificate"

Under the CA/Browser Forum rule (since 2023), the **private key** of a publicly accepted code-signing certificate must reside
on **hardware protection** (HSM or USB token, FIPS 140-2 Level 2 / Common Criteria). Therefore:

- there is no free certificate that you could download and use on your own machine;
- the earlier "free" offers (e.g. Certum Open Source) also require a **paid card**;
- the certificate is usually issued to a **person**, not to the project.

Because of this, open-source projects have two realistic routes left: **someone else signs for you** (SignPath, Microsoft
Store), or **you pay** for a service (Azure Trusted Signing, Certum, SSL.com).

## 1. Free route: SignPath Foundation (this is the recommended one)

- <https://signpath.org/> — "Free Code Signing for Open Source software".
- The certificate is issued in the name of **SignPath**; they attest that the binary was built from your repo; the private key is in
  **their HSM** and never reaches you.
- **No identity verification** and no card; the eligibility condition is that the project is **open source**, lives in a
  public repo, is active, and the build goes through their (or CI) process.
- Antikolt is **MPL-2.0**, so it is a suitable candidate.

**Steps (in order):**

1. **Public repo** (GitHub) — SignPath sees the repo URL and the build, so the code must be publicly
   available (the license already exists: `LICENSE`).
2. **Application**: <https://signpath.org/apply> — the project, the license, the main maintainer's contact details, and the
   release location (Releases) are required. After review you receive a SignPath.io account.
3. **CI process** that builds the release (GitHub Actions), because SignPath hooks into it:
   - the workflow runs `scripts/dist.ps1 -Release` on a Windows runner;
   - it uploads the resulting exes to SignPath (SignPath GitHub Action or PowerShell),
   - **SignPath signs them**, then the signed files go into the release;
   - the workflow records the version and build information (SignPath expects this as provenance).
4. **Verification after the release**: `pwsh -File scripts/sign.ps1` is not needed (the key is with them), but
   `signtool verify /pa /v <exe>` locally shows that the downloaded binary is signed and who signed it.
5. **SmartScreen**: the signature shows the publisher, but *reputation* builds over time. In the first weeks it may still
   warn; this cannot be bought, only waited out (and it strengthens with more downloads).

## When should you apply? (the order worth following)

SignPath **signs releases in its own process**, so applying is most useful once there is already a **releasable, reproducible
build** and (ideally) a public repo + CI. Until then, the correct path is:

1. **now** — development and testing **without signing or with a self-signed** certificate (on your own machine), because
   self-signed gives no trust on a foreign machine anyway;
2. **when there is a release** — SignPath application (free, OSS), integrating the SignPath signing into CI;
3. **in parallel** — if the MSIX/Store route is also on the table, the Store account (currently **free for individual
   developers**), because there Microsoft performs the signing (see point 4).

Applying earlier is not forbidden, but SignPath asks about the build process and the releases — with empty hands the
review is slower.

## 2. Free, but not public: your own (self-signed) certificate

This does **not** provide SmartScreen trust; it only works on your own machines (and even there only if the
certificate was installed into the "Trusted Root Certification Authorities" store). It is good for development,
internal distribution, and trying out the process:

```powershell
pwsh -File scripts/sign.ps1 -SelfSigned
```

The script creates a `CN=Antikolt test (NOT TRUSTED)` certificate in the user store, signs the
`dist\Antikolt\*.exe` files, and **verifies** the signature (`signtool verify /pa`). Its name is also a warning:
this is not a solution for release.

## 3. Cheap, real: Azure Artifact Signing (formerly Trusted Signing, about 10 USD/month)

- <https://learn.microsoft.com/azure/trusted-signing/> — cloud HSM, no USB token, callable from CI.
- **Important limitation:** for organizations, USA / Canada / EU / UK; **for individual developers, only USA and Canada**. As a Hungarian individual developer this route is therefore *not* available (a company would be needed), leaving the OV certificate or the Store.
- Contrary to the old situation: **an individual developer can also apply** (with identity verification; for a company,
  at least 3 years of verifiable company data are required, and the service is tied to regions).
- This is the cheapest **real** (publicly accepted) route, if we do not want to wait for SignPath's process.

## 4. Microsoft Store (MSIX) — "signed" even without signing

This is the **truly free route to signing**: for an MSIX package, after certification the Store **automatically re-signs** the package, no certificate needs to be purchased; since 2025 the developer account is **free for individual developers** (`storedeveloper.microsoft.com`). If, however, we submit an **MSI/EXE installer** to the Store, we must sign it ourselves (with a certificate chained to the Microsoft Trusted Root CA). This does **not** sign our portable folder. If we release an MSIX package through the Store, **Microsoft signs** the package. This requires a developer account
(one-time fee), and a package that meets the Store requirements. It is an alternative to the portable folder, not a
supplement: an MSIX installed from the Store is a different experience than the "folder + shortcut".

## SignPath is not just Windows — and why it does not replace the Store

According to SignPath's documentation, the supported formats are **platform-independent** (<https://docs.signpath.io/artifact-configuration/reference>):

| Format | Windows | Linux | Other |
|---|---|---|---|
| Authenticode (`.exe`, `.dll`, `.msi`, `.msix` …) | ✅ | — | — |
| NuGet, OPC/VSIX, Office macros | ✅ | — | — |
| Java (`jar`), Android (`apk`) | — | — | ✅ |
| **RPM** | — | ✅ | — |
| DEB (`debsigs`) | — | ✅ | — |
| Container (Notation/Cosign), XML/JSON, downloaded GPG/CMS/S/MIME | ✅ | ✅ | ✅ |

**The tier limit matters:** the documentation indicates, for each directive, which package it is available in. The
`<rpm-sign>`, `<jar-sign>`, `<create-apk-signature>`, `<jsf-sign>` appear there with the **"Open Source Code Signing"** label
(that is our route), while `<debsigs-sign>` appears only with the "Semantic Code Signing" label — so signing a
Linux `.deb` is **not certain** to be in the free OSS tier. This must be confirmed with SignPath,
not assumed.

### Why does it make sense alongside the Store?

Because they sign **different artifacts**:

- **Store (MSIX)**: Microsoft signs the package that is installed **from the Store**. It does **not** sign our portable
  exe, nor the release downloadable from our own site. In return it is free and
  warning-free — but the Store rules and the submission/review also come with it.
- **SignPath**: signs **our own** releases (portable `.exe`, later `.msi`/`.exe` installer, Linux
  RPM). This is the only free route that signs what is **the product today** (the folder + shortcut).

So not "instead of", but **two channels**: Store for those installing from the Store, SignPath for our own
download. Neither covers the other.

## Summary table (Microsoft's own comparison, 2026)

| Route | Cost | SmartScreen | Available to us (HU)? |
|---|---|---|---|
| **Store, MSIX** (signed by Microsoft) | **free** (account too, for individual developers) | ✅ no warning | yes, an MSIX package is needed |
| Store, MSI/EXE installer | requires your own certificate | ✅ (at install) | only with a certificate |
| **SignPath Foundation** (OSS) | **free** | ⚠️ reputation builds | yes (public repo + CI) |
| Azure Artifact Signing | ~9.99 USD/month | ⚠️ reputation builds | **not for individuals** (only US/CA); as a company in the EU, yes |
| OV certificate (DigiCert, Sectigo…) | 150–300 USD/year | ⚠️ reputation builds | yes (hardware token) |
| EV certificate | 400+ USD/year | ⚠️ **since 2024 does not bypass** SmartScreen | yes, but not worth it for this |
| Self-signed | free | ❌ blocks installation on others' machines | only for development |

## What to do now in the Antikolt repo

| Step | Status |
|---|---|
| `scripts/sign.ps1` — signing and **verification**, in PFX / store / self-signed mode; rejects without a certificate | done |
| `scripts/dist.ps1 -Release` — the package that must be signed | done |
| Public repo + SignPath application | **human step** (account, e-mail, review) |
| GitHub Actions workflow (build → SignPath → release) | not yet; makes sense after SignPath approval |
| Post-release verification (`signtool verify /pa`) | `sign.ps1` shows how to do it |

**What requires a certificate and not code**: the application, the contract (SignPath Terms), and, for a paid route, the
verification of company/individual data. These cannot be "solved in the repo", so here the task is to follow the order
above.

## References

- SignPath Foundation: <https://signpath.org/> (application: <https://signpath.org/apply>)
- Certum code-signing certificates (paid, with card): <https://certum.eu/>
- Azure Trusted Signing: <https://learn.microsoft.com/azure/trusted-signing/>
- MSIX signing: <https://learn.microsoft.com/windows/msix/package/signing-package-overview>
- Windows SmartScreen and reputation: <https://learn.microsoft.com/windows/security/operating-system-security/virus-and-threat-protection/microsoft-defender-smartscreen/>

## What we measured in this repo (actual output)

```
pwsh -File scripts/sign.ps1
  signtool : C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe
  file     : dist\Antikolt\antikolt-shell.exe  (and the other two)
  → no certificate: pass -Pfx …, -Thumbprint …, -SelfSigned, or sign through SignPath in CI
  exit 1

pwsh -File scripts/sign.ps1 -SelfSigned
  → New-SelfSignedCertificate: CertEnroll::CX509Enrollment::_CreateRequest:
    A hozzáférés megtagadva. 0x80070005 (WIN32: 5 ERROR_ACCESS_DENIED)
```

The first run is the point: the script **rejects without a certificate**, it does not invent a signature. The second shows
that in this **restricted developer environment** writing to the user certificate store is forbidden, so the
self-signed route was **not testable** here — it should work in a normal Windows account (the code calls
`New-SelfSignedCertificate` in the documented way). This is a limitation, not a bug in the script.

## CI (the place of SignPath signing) — prepared, but not yet run

`.github/workflows/release.yml`: builds on a tag (`v*`), packages with `scripts/dist.ps1 -Release`, uploads
as an artifact, then — once the SignPath approval is in place — SignPath's PowerShell commands (`Submit-SigningRequest`
→ `Get-SignedArtifact`) sign the exes, and finally `scripts/sign.ps1 -VerifyOnly` **proves** that the
signature is present. The signing step is skipped until the `SIGNPATH_ORGANIZATION_ID` variable is set.

**What is proven and what is not:** `sign.ps1 -VerifyOnly` was **tried** locally (on an unsigned binary
`NOT SIGNED`, exit 1); the workflow itself **has never run** — there is no public repo and no SignPath approval.
So it is a scaffold, not a finished feature; the first real release will confirm it.
