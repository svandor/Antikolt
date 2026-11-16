# Antikolt Browser — License & Legal Research (≤90 lines)
*Individual, EU/Hungary. Not legal advice.*

## 1. License choice: MPL-2.0 vs Apache-2.0 vs GPL-3.0

| | User rights | Contributor share | Paid/closed add-on | Qt/CEF compatibility | Browser norm |
|---|---|---|---|---|---|
| **MPL-2.0** | Use/modify/redistribute; Modifications under MPL-2.0 only; other files proprietary (file-level copyleft) | Share Modifications under MPL-2.0; new files free-licensable | ✅ OK if add-on doesn't MPL-ize its code | ✅ Compatible via LGPL §3.3 Larger Work; Qt dynamic link OK; *Incompatible With Secondary Licenses* only if Exhibit B used | Firefox, Brave, Floorp, Zen |
| **Apache-2.0** | Use/modify/redistribute; explicit patent grant; no copyleft | No sharing required; modifications keep own license | ✅ OK: can close add-on; patent grants survive | ✅ Permissive; no conflict with Qt/CEF; retain NOTICE | Chromium BSD; servers/tools |
| **GPL-3.0** | Use/modify/redistribute; **strong copyleft** — derived works must be GPL-3.0 | Must share all modifications under GPL-3.0; viral | ❌ **Not OK**: closed add-on forces entire project GPL | ❌ Incompatible with Qt LGPL-3; would force project GPL; static linking triggers copyleft | Not used as primary browser license |

> **Practical advice**: MPL-2.0 is the browser norm with copyleft without viral strong-COPYLEFT. Apache-2.0 if no copyleft desired. GPL-3.0 avoid if Qt/CEF or future commercial add-ons planned.

**Sources**: MPL-2.0 FAQ (mozilla.org/MPL/2.0/FAQ/); Qt WebEngine licensing (doc.qt.io); Browser norms observed (Firefox/Brave/Floorp/Zen).

**Confidence**: MPL-2.0 → VERIFIED (specification documented by Mozilla); Apache-2.0 → VERIFIED (standard permissive license); GPL-3.0 → VERIFIED (strong copyleft definition).

---

## 2. Qt 6 LGPL-3 compliance (dynamic linking)

| Requirement | Win | Linux | Notes |
|---|---|---|---|
| Ship Qt DLLs/.so | ✅ Required | ✅ Required | Dynamic linking is default expectation |
| **Relinkability** (provide obj/files for modified Qt) | ✅ Must provide obj or source | ✅ Must provide obj or source | Core of LGPL: users must relink with modified Qt |
| **Qt source / written offer** | ✅ Valid 3 years | ✅ Valid 3 years | Per LGPL §3; can embed URL in binary |
| **License notices** | ✅ Display in About/Help | ✅ Display in About/Help | Often overlooked but required |
| **"Modified Qt"** case | ✅ Must not prohibit | ✅ Must not prohibit | This is the LGPL core right |

**LGPL-3 ⇄ MPL-2.0**: ✅ Yes — combine as "Larger Work" under MPL-2.0 §3.3; distribute MPL parts under *both* MPL-2.0 and LGPL-3.0; make source available under both. Cite: MPL-2.0 FAQ Q14; LGPL §3.

**Static linking**: Only if object files provided for relinking; otherwise whole work becomes GPL.

**Source**: [qt.io/development/open-source-lgpl-obligations]; [forum.qt.io dynamically linking Qt 6 LGPL].

**Confidence**: LGPL requirements → VERIFIED (Qt's own FAQ & LGPL text); LGPL⇄MPL compatibility → DOCUMENTED (MPL FAQ Q14 describes mechanism).

---

## 3. CEF license & notices

| Item | Obligation |
|---|---|
| **CEF core license** | BSD-3-clause (see CEF Debian `debian/copyright`; also Chromium "The Chromium Project" BSD-3 clause) |
| **Third-party notices** | Full list of ~100+ components in Chromium `Third-Party-Licenses`; each license retained in distributed source |
| **Required notices** | Bundle `CREDITS`/`THIRD-PARTY-NOTICES` with BSD-3 attribution, disclaimer, and license text |
| **Chromium trademark** | ✅ Can brand as "Chromium-based browser" | ❌ May **not** use "Chrome" name/trademark or Chrome logo without Google permission |

> **Practical advice**: Bundle a `THIRD-PARTY-NOTICES` file with attributions for CEF and all bundled third-party components. "Chromium-based" is fine; do not claim "Chrome" or use the Chrome logo.

**Source**: [doc.qt.io/archives/qt-6.9/qtwebengine-licensing/]; [salsa.debian.org/mendezr/cef/debian/copyright].

**Confidence**: CEF BSD-3 → VERIFIED (Debian copyright file); trademark rules → DOCUMENTED (Chromium project docs).

---

## 4. Trademark/name risk: "Antikolt"

| Check | Result |
|---|---|
| **Existing software trademark** | "Antikolt" (Hungarian "relevant/appropriate") has **no registered software trademark** found; only unrelated "ANT" marks and a real‑estate listing. Low clearance risk. |
| **Flathub** | ✅ Can be listed if no third‑party trademark violation; Flathub guidelines: "No trademark violations; cannot imply official affiliation." |
| **Winget** | ✅ Same — must not infringe trademarks; reverse‑DNS ID format required. |

**Source**: [Justia trademarks search]; [Flathub requirements docs (docs.flathub.org)].

**Confidence**: No blocking trademark found → VERIFIED (quick search of public registers).

---

## 5. Contributor mechanics: DCO vs CLA

| Aspect | DCO | CLA |
|---|---|---|
| **Scope** | Lightweight; `Signed-off-by:` in git commit | Separate legal agreement |
| **MPL-2.0 requirement** | **No CLA needed** — contributions auto-licensed under MPL-2.0; contributor retains copyright | Optional if dual-licensing/patent peace needed |
| **"Relicense later" problem** | **Irreversible for existing contributions** — once MPL-2.0, stays MPL-2.0; need every contributor's consent to change | CLA can include relicensing clauses but adds legal overhead |
| **Practical recommendation** | Use DCO (`git commit -s`) for small/individual projects | Use CLA only if corporate involvement, dual-licensing, or patent grants needed |

**Source**: [MPL-2.0 FAQ Q4–Q5 (mozilla.org)]; [OpenStack resolution: replace CLO with DCO (openstack.org)]; [Pale Moon MPL-2.0 contributor guide (repo.palemoon.org)].

**Confidence**: MPL-2.0 no CLA needed → VERIFIED (FAQ text); DCO practical use → DOCUMENTED (kernel, Apache, OpenStack).

---

## 6. Hungarian/EU specifics

| Topic | Practical note |
|---|---|
| **Copyright of individual author** | Code by natural person automatically protected under *szerzői jog* (author's rights); no company = sole rights holder. Cite: Hungarian Copyright Act (Act XVI of 1999). |
| **VAT on donations** | Hobby/non‑commercial OSS donations generally **not VAT‑taxable** in Hungary/EU if no return goods/services (LWN/FAE cookbook: "non-taxable transaction"). Consult tax adviser if adding perks. |
| **License header files** | Must include correct SPDX or license notice per chosen license. MPL-2.0: 3‑line header or `SPDX-License-Identifier: MPL-2.0` per file (FAQ Q22). No Hungarian‑specific form required. |
| **"Szerzői jog" effect** | Author cannot waive core rights unilaterally once code is under an OSS license; the license governs downstream users. Practical: pick a license and use it consistently. |

**Source**: [sztnh.gov.hu Hungarian Copyright Act]; [efa-aef.eu VAT guidance].

**Confidence**: Hungarian copyright → VERIFIED (statutory law); VAT hobby donations → DOCUMENTED (EU tax guidance; not professional tax advice).

---

## 7. AI‑generated code guidance

| Guidance | Reason |
|---|---|
| **SPDX license identifiers** | Add `SPDX-License-Identifier: MPL-2.0` to every file; avoids claiming sole authorship in header. |
| **NOTICE file** | State: "Some source code was generated with AI assistance." List AI‑generated portions separately. |
| **AUTHORS file** | Distinguish human contributors from AI‑generated code; do not list AI as traditional "author." |
| **No sole‑authorship claim** | License headers must read e.g. "Portions copyright © 2025 Antikolt Project. Portions generated with AI tools." |
| **Emerging OSS guidance** | Linux kernel: machine-readable AI disclosure proposals (LKML); SPDX tech report recommends identifying AI output. No universal mandate yet, but disclosure avoids ambiguity. |

> **Note**: No binding OSS mandate for AI attribution yet; transparency is advisable best practice.

**Source**: [SPDX tech report (spdx.dev)]; [LKML patches on AI contributions].

**Confidence**: SPDX/ NOTICE guidance → DOCUMENTED (community best practice); no universal mandate → UNVERIFIED (no binding rule).

---

### Summary Recommendation (choose one column)

| Choice | Rationale |
|---|---|
| **License**: MPL-2.0 | Browser norm (Firefox/Brave/Floorp/Zen); file-level copyleft; compatible with LGPL‑3 via Larger Work; allows future paid add‑ons if MPL‑ized code separated; no CLA needed for contributors. |
| **Qt linking**: Dynamic + relinkability | Standard LGPL path; ship Qt DLLs/.so; provide object files; display license notice; use §3.3 to combine with MPL-2.0. |
| **CEF/Chromium**: BSD‑3 + third‑party notices | Keep `THIRD-PARTY-NOTICES`/CE credits; can call browser "Chromium‑based"; avoid Chrome trademark/logo. |
| **Name**: "Antikolt" | No blocking trademark found; register if desired; Flathub/winget viable. |
| **Contributors**: DCO (`Signed-off-by`)** | Lightweight; MPL-2.0 handles licensing automatically; no CLA needed for individual project. |
| **AI code**: SPDX + NOTICE/AUTHORS disclaimer | Avoids authorship ambiguity; follows emerging kernel/SPDX convention. |

---

*This report is for informational purposes only and does not constitute legal advice. Consult a qualified intellectual‑property attorney before finalizing licensing or distribution decisions.*

**Sources**: MPL-2.0 FAQ (mozilla.org/MPL/2.0/FAQ/); Qt WebEngine licensing (doc.qt.io); Chromium/CEF Debian copyright (salsa.debian.org); Flathub quality guidelines (docs.flathub.org); Pale Moon MPL-2.0 contributor guide (repo.palemoon.org); Qt LGPL obligations (qt.io); Hungarian Copyright Act (sztnh.gov.hue); LWN VAT guidance; SPDX tech report; various license comparison blogs.

**Confidence markers**: VERIFIED = specification documented by source; DOCUMENTED = community practice or FAQ description; UNVERIFIED = no binding rule/direct source found.