# 13 — License and legal framework (Q10 in detail)

> This document is **not legal advice**, but engineering decision preparation based on public sources.
> The final license file and copyright notices will go into the repo with the first commit of the code.

Short answer: **MPL-2.0**, with dynamically linked Qt, BSD-3 CEF, DCO-based contribution,
SPDX headers, and an honest `NOTICE` about AI assistance.

## 1. License choice: MPL-2.0 vs Apache-2.0 vs GPL-3.0

| | **MPL-2.0** *(recommended)* | **Apache-2.0** | **GPL-3.0** |
|---|---|---|---|
| What the user may do | use, modify, distribute | the same + explicit patent grant | the same, but strong copyleft |
| What must be shared | the **modified files** (file-level copyleft); new files may be licensed freely | nothing | **the entire derivative work** |
| Later closed extension | ✅ possible (the extension is not "infected") | ✅ | ❌ practically not |
| Qt (LGPL-3) | ✅ compatible (MPL 3.3 "Larger Work") | ✅ | ❌ conflicts, forces the project into GPL |
| CEF (BSD-3) | ✅ | ✅ | ✅ |
| The norm among browsers | **Firefox, Brave, Floorp, Zen** | Chromium (BSD) | not typical |

**Why MPL-2.0:**
1. This is the proven license of browsers — familiar to a user and a contributor alike.
2. **File-level** copyleft: modified files must be shared, but your own new code remains freely
   licensable (e.g. a later commercial extension).
3. No CLA needed: the contribution automatically falls under MPL-2.0.
4. Compatible with Qt LGPL-3 and CEF BSD-3.

**Why not GPL-3:** because of the combination with Qt LGPL-3 and the possibility of a later closed extension
it is practically ruled out. **Why not Apache-2.0:** it would work, but it does not protect the project from
someone keeping fixes closed — for browsers, MPL is the better balance.

### 1.1. What this means in practice (in your case)

| Question | Answer under MPL-2.0 |
|---|---|
| Can anyone use it for free, even a company? | **Yes.** |
| Can anyone modify it? | **Yes.** |
| Can anyone contribute? | **Yes**, without special permission (DCO, not CLA) — see `15-HOZZAJARULAS.md`. |
| What must someone who modifies share? | The **modified version of the files we wrote** (under MPL-2.0). Their **new** files may even be closed. |
| Can someone take the project and close it off? | **Not** our files; but they may make a closed extension. |
| Who owns the copyright? | Everyone owns their own code. The project does not ask for a rights assignment. |
| Can we change the license later? | Only with the consent of **every** contributor — this is the price of "irreversibility". |
| A later paid/closed version? | Possible (the core stays open, the extension may be closed). |
| Are Qt/CEF/distros compatible? | **Yes** (LGPL-3 "Larger Work", BSD-3). |
| Do contributors know it? | **Yes** — Firefox, Brave, Floorp, Zen use the same. |

**So my recommendation is: MPL-2.0 + DCO.** This **fully** meets your expectation of "anyone can contribute freely"
(no CLA, no signature, no asking for permission), while the project's code stays open. If the
freedom to change the license later were more important, that would only be achievable with a CLA — and that is precisely what would hinder free
contribution, so I **do not recommend it**.

### 1.2. Security is a matter of process, not of license

An important clarification: **the license does not affect security.** Apache-2.0 would not be more secure, and
MPL-2.0 would not be less so. The browser's risks are handled by the **process**: protected code sections, mandatory review,
fuzz tests, memory-regression gates, a vulnerability-reporting channel, signed releases. This is what
`15-HOZZAJARULAS.md` describes in detail — this is the answer to your point about "there may be security risks".

## 2. Qt 6 (LGPL-3) — obligations that must be met

With dynamic linking, this list is mandatory and cannot be neglected:

| Obligation | What it means in practice |
|---|---|
| **Dynamic linking** | Qt is shipped in DLL/SO form, not baked in. |
| **Relinkability** | The user must be given the opportunity to replace Qt. On Windows this means replacing the Qt DLLs (must be documented); on Linux it is natural. |
| **Source / written offer** | The Qt source must be made available (a link is sufficient, an offer valid for 3 years). |
| **License texts** | In the "About / Licenses" window, the Qt LGPL-3 text and the fact of use. |
| **Modified Qt** | If we modified Qt, that would have to be published — we do not forbid it. |
| **Static linking** | ❌ to be avoided: it would only be allowed if the user receives relinkable object files — in practice this would mean GPL-izing the entire work. |

**MPL-2.0 + Qt LGPL-3 together:** section 3.3 of MPL 2.0 ("Larger Work") allows the combination;
the Qt parts must be distributed under LGPL-3, the MPL parts under MPL-2.0. This is the established practice
(e.g. Qt-based open-source applications).

## 3. CEF / Chromium — license and trademark

| Item | Obligation |
|---|---|
| CEF license | **BSD-3-clause** → the copyright notice and the license text must be preserved |
| Chromium third parties | Chromium contains hundreds of components; the `THIRD-PARTY-NOTICES` / CREDITS file must be included |
| Trademark | We may call ourselves a "**Chromium-based** browser"; we may **not** use the "Chrome" name or logo without permission |

The practice: the installer contains a `THIRD-PARTY-NOTICES.txt`, and the About window has an
"Open-source licenses" button.

## 4. The name: "Antikolt"

- Based on a public trademark search, there is **no conflicting software trademark** (the word's meaning is "antikolt" =
  archaic/antique — a Hungarian word, not registered for software).
- Flathub and winget listing is unhindered from a trademark standpoint, as long as it does not suggest an official relationship.
- Recommendation: keep the name; it is worth reserving the domain and the GitHub organization in advance.
- **Note:** the name is Hungarian, the code is English — this is fine per ADR-017, but international
  readers need a one-line explanation in the README ("antikolt ≈ antique-ish, archaic").

## 5. Contribution: DCO, not CLA

- MPL-2.0 **does not require a CLA**: the contribution automatically falls under MPL-2.0 with the license, and the author
  keeps their copyright.
- Therefore the **DCO** (`git commit -s`, `Signed-off-by`) is the right mechanism: it is easy, and most
  contributors know it.
- **A consequence to be aware of:** code already released under MPL-2.0 **irreversibly** stays MPL-2.0;
  a later license change would require the consent of every contributor. Therefore the license must be chosen well now
  — and now MPL-2.0 is the decision.
- `CONTRIBUTING.md` will be prepared together with v0.1, including the DCO and the code style.

## 6. Hungarian/EU specifics

| Topic | Practical note |
|---|---|
| Copyright | As a private individual, the code is automatically protected by copyright (Szjt., Act LXXVI of 1999); no registration is needed. |
| Donations (VAT) | Donations to a hobby project are typically not VAT-liable as long as no product/service is sold; in the case of a counter-benefit (perk) it is no longer this clear-cut → an accountant. |
| License headers | Per file `SPDX-License-Identifier: MPL-2.0` (accepted instead of a 3-line header, MPL FAQ Q22). |
| Consumer protection | If you release the browser to consumers, questions of liability and warranty arise — MPL excludes warranty, but this cannot always be enforced against a consumer. |
| **EU CRA (cyber resilience regulation)** | For products containing digital elements, obligations take effect from the end of 2027; purely non-commercial, open-source projects may fall under a lighter assessment. **To be checked** before release — not blocking today, but it could be in 2027. |

## 7. AI assistance in the code (a mandatory topic in this project's case)

Since development is done with an **AI agent** (the user recorded this, ADR-021), the repo must handle this
**honestly and properly**:

1. **Do not claim exclusive human authorship** in the headers. Instead:
   `Portions copyright © 2026 Antikolt Project. Portions generated with AI assistance.`
2. **`NOTICE` file**: a short statement that part of the code was produced with AI tools, and
   that the author reviewed/tested it.
3. **`AUTHORS` file**: naming the human contributors (currently the project owner) and the development tool.
4. **`SPDX-License-Identifier`** in every file — so authorship does not need to be claimed in the header.
5. Recording the code's **origin** (which file comes from which source/ML) in the commit messages
   and in `docs/`; this protects you later in the event of a legal dispute.
6. There is no universal, mandatory rule (2026), but the Linux kernel and the SPDX community recommend this direction —
   the project follows it.

## 8. What must be put into the repo (checklist)

| File | Content |
|---|---|
| `LICENSE` | full text of MPL-2.0 |
| `NOTICE` | CEF/Chromium/Qt third parties + AI statement |
| `AUTHORS` | human contributors + development tool |
| `THIRD-PARTY-NOTICES.txt` | alongside the binary (Chromium components, Qt, zstd, SQLite, etc.) |
| `CONTRIBUTING.md` | DCO, code style, how to indicate AI assistance |
| About window | "Licenses" button → the above texts |
| Header of every source file | `SPDX-License-Identifier: MPL-2.0` |

## 9. Decision (to be confirmed)

- License: **MPL-2.0** ✅
- Qt: **dynamic linking**, relinkability documented ✅
- CEF: BSD-3, `THIRD-PARTY-NOTICES` included ✅
- Contribution: **DCO**, no CLA ✅
- AI: **statement + SPDX + AUTHORS/NOTICE** ✅
- Name: **Antikolt** stays ✅

If you disagree with any of these points (e.g. prefer Apache-2.0, or prefer a CLA for the possibility of a later
license change), you must speak up now — later it will be difficult or impossible to reverse.
