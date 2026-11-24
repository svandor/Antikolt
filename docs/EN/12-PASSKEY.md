# 12 — Passkey / WebAuthn: why it's hard, and what we can do (Q9 in detail)

This document discusses the most painful technical limitation. Short summary:

> **CEF has no built-in WebAuthn support.** A passkey requires the browser's *built-in* authenticator,
> which in Chromium must be implemented by the embedding party (the browser). CEF does not do this —
> `CefRequestHandler` has no WebAuthn hook, and there is no `CefWebAuthn*` API. Therefore, without
> passkey support, some modern sites let you in "with passkey only," and the browser cannot sign in.

## 1. What a passkey is, in one sentence

A passkey is a **key pair**: the private key is on the user's device (in a hardware-protected chip or in the
system authenticator), the public key is with the service provider. At sign-in, the browser signs a
challenge — without a password, protected against phishing (because the signature is bound to the **origin**).

The browser's job in this is: **invoking the platform authenticator** (Windows Hello, Touch ID, FIDO2 key) and
carrying out the cryptographic protocol. In Chromium this is implemented by the *embedder* — as a browser, we
would have to. CEF does not provide this layer.

## 2. What exactly is missing

| Layer | Who implements it in Chromium | In CEF |
|---|---|---|
| `navigator.credentials.create/get` (JS API) | Chromium renderer | ✅ present |
| `WebAuthenticationDelegate` (embedder decisions: which authenticator, what UI) | the browser (Chrome/Edge) | ❌ **missing** |
| `AuthenticatorRequestClientDelegate` (the user interface and the CTAP2 call) | the browser | ❌ **missing** |
| CTAP2 / Windows Hello / libfido2 call | the browser | ❌ **missing** |

The missing part is not a switch, but a slice of Chromium's embedder integration. Its size, according to
research, is **on the order of a partial Chromium port** — not one week of work.

## 2.5. What a "passkey-only" account is, and how to check it

**Plain (password) account:** you have a password, and the passkey is only a *convenience* alternative. If the
browser doesn't support passkeys, you sign in with password + TOTP — no problem.

**Passkey-only account:** at the service provider you **deleted the password**, and you can only sign in with a
passkey. In that case there is nothing to fall back on: if the browser doesn't support passkeys, **you cannot
sign in at all** — neither with a password, nor with "forgot password," because there is no password. This is the hard wall.

**Where it occurs most often:**

| Service | What it means in practice |
|---|---|
| **Google** | in account settings the password **can be deleted** if a passkey exists ("passwordless sign-in") |
| **Microsoft** | "Passwordless account" turned on → only Microsoft Authenticator / passkey / Windows Hello |
| **Apple ID** | a passkey or security key can be made mandatory |
| **GitHub / X / others** | passkey is *supported*, but the password usually remains → **not** passkey-only |
| **Enterprise/school SSO** | sometimes FIDO2/passkey is mandatory (this may include workplace sign-in) |

**How to check in 2 minutes:**

1. **Google:** `myaccount.google.com` → *Security* → "How you sign in to Google" — if only the
   passkey/fingerprint is listed, and there is no "Password" row (or there is an "Add password" button), then
   you are **passkey-only**. If the password is there too, then not.
2. **Microsoft:** `account.microsoft.com` → *Security* → *Advanced security options* → if the
   "Passwordless account" **is turned on**, you are passkey-only.
3. **Any site:** try signing in "incognito" (there is no passkey in the system there): if it only asks for a
   passkey and does not offer a password, then it is passkey-only.
4. The browser will also signal this on its own: if, when opening a card, the `navigator.credentials.get()`
   call fails, and there is no password field on the page, the UI writes: "this account requires a passkey".

**If it turns out you have such an account:** v1.0 (without passkey) will not be your daily browser, so the
passkey research (path B) must be moved **ahead of** v1.0. If you don't have one, the A → B order stays.

## 3. The options (honestly, with cost)

| # | Solution | What it can do | Cost | Risk |
|---|---|---|---|---|
| **A** | We start without passkey (password + TOTP + runbook) | most sites work; passkey-only accounts don't | 0 | a few accounts (e.g. Google "passkey-only") excluded; the user should see this |
| **B** | **Software passkey in the vault** (CDP virtual authenticator + key export) | real passkey experience, cross-platform, the vault carries the key | 3–5 weeks research + 4–6 weeks development | CDP internal API (version-dependent); not hardware-protected; `userVerification` is simulated |
| **C** | CEF patch: implementing `WebAuthenticationDelegate` → Windows Hello | real, hardware passkey | months, CEF maintenance forever | not realistic for a solo project |
| **D** | Separate Chromium helper just for passkey | hardware passkey, in isolation | +150–300 MB RAM + complex IPC | violates the memory target; two engines to maintain |
| **E** | JS shim in the main world (overriding `navigator.credentials`) | appears to work | days | **rejected**: the shim is visible/modifiable to the page's JS → security level drops, phishing protection is broken |

### 3.1. The drawbacks of path **B** — precisely

B = software passkey in the vault (CDP virtual authenticator, key export/reload).

| # | Drawback | What it means in practice |
|---|---|---|
| **B1** | **No hardware protection** | the private key is in the vault (AES-256-GCM, DEK behind DPAPI/Argon2id). Whoever unlocks the vault (or takes over your Windows session) gets access. A TPM/Windows Hello key **cannot be exported**, so it can't even be taken by theft. |
| **B2** | **No real user-presence verification** | a real passkey asks for fingerprint/face/PIN on every use. The virtual authenticator "simulates" with `isUserVerified=true`, so we must supply the protection ourselves: vault locking + an approval window for every passkey operation. This is our code, not a platform guarantee. |
| **B3** | **No attestation** | the virtual authenticator gives "none" attestation. Where the provider demands **real** attestation (some banks, enterprise systems, government sign-in), path B **gets rejected** — there you still cannot sign in. |
| **B4** | **Built on an internal/debug CDP API** | `WebAuthn.addVirtualAuthenticator` is a developer test feature, not a product API. A CEF/Chromium update can break it, and then **all passkey users stall at once**. Handling: F0 spike + contract tests + automatic fallback to password. |
| **B5** | **Does not survive restart** | the virtual authenticator lives in memory; at every startup it must be re-injected from the vault. If the vault is locked, sign-in does not work until then — this is an extra step for the user. |
| **B6** | **Per-site compatibility is uncertain** | if the site decides via `isUserVerifyingPlatformAuthenticatorAvailable()`, it may see the virtual authenticator as "not real," and **hide the passkey button** or reject it. It must be tested site by site. |
| **B7** | **Pretending to be "device-bound"** | the virtual authenticator reports itself as device-bound while the key is portable. This is an **advantage** for the user (it can be transferred), but can be misleading to the provider — so the UI must state: "software authenticator, not hardware". |
| **B8** | **Sync model differs** | the Apple/Google passkey syncs on the platform; with us only if you export/transfer the vault. |

**Overall:** B's security level is **roughly equal to that of the vault** — not more. Against phishing,
however, it **does protect** (the key is bound to the origin, the fake page cannot request it), and it works on
most consumer sites. This is the realistic compromise.

### 3.2. The drawbacks of path **D** — precisely

D = separate Chromium/Electron helper process, just so it can do hardware passkey.

| # | Drawback | What it means in practice |
|---|---|---|
| **D1** | **Memory** | +150–300 MB, whether permanently or only at sign-in (in the latter case 1–2 s startup). This **goes against the project's entire memory promise** (I1 and the 1 GB budget). |
| **D2** | **Handing over the session is the real problem** | WebAuthn is bound to the origin, so the helper must open the same page. If sign-in happens there, the **cookie must be transferred** into the main engine — which means two separate engines, two partitions, and a secret-transfer channel. Or: you finish sign-in in the helper, and the card opens there — which is not our engine. |
| **D3** | **Two engines to maintain** | two Chromium versions, two security-patch cycles, +100–150 MB installer, two packaging paths. |
| **D4** | **Worse user experience** | the Windows Hello window appears in the helper; on Linux the libfido2 path is separate work. The "everything in one window" principle is broken. |
| **D5** | **Larger attack surface** | a second engine, a new IPC channel, and a secret-transfer protocol — more code, more bugs, more security risk. |
| **D6** | **Wayland** | the helper also needs a window/OSR, so work doubles on the hardest platform. |

**What D provides:** real hardware passkey (Windows Hello/TPM), attestation too, accepted everywhere.
**Its price:** giving up one of the project's main promises (small memory) and eternal maintenance of a second engine.

### 3.3. Why not C (CEF patch)

C would be the "correct" solution, but implementing Chromium's embedder WebAuthn layer in CEF is
**months** and **eternal** maintenance (it must be re-applied at every CEF update). One person + one
agent cannot take this on. If it's ever really needed, this will be the path — but not now.

## 4. My proposal: A now, B in v1.1, C/D excluded

**v1.0:** no passkey. The vault's data model is ready (`credential.kind = 'passkey'`, `passkey_ct`),
and the UI honestly indicates: "this site asks for a passkey — you must use password sign-in for this"
(or the runbook tries the password path).

**v1.1 (path B), but only after a research spike.** The essence of B is that the browser operates its **own
software authenticator**:

1. The engine gets a **virtual authenticator** via CDP (`WebAuthn.enable` + `addVirtualAuthenticator`),
   `hasResidentKey = true`, `hasUserVerification = true`, `isUserVerified = true` (the user-presence check is
   replaced by **vault locking**: a passkey operation only runs with an unlocked vault).
2. At registration (`create`) the resulting credential data (`credentialId`, **private key PKCS#8**,
   `userHandle`, RP identifier) is **exported and saved encrypted into the vault** — so the passkey becomes
   *portable* (even across machines), which is better than a hardware passkey.
3. At sign-in (`get`) the credential is **reloaded** from the vault into the virtual authenticator, and the
   operation runs.
4. Most sites accept it, because from the protocol's perspective it is a real (origin-bound) passkey.

**What this solution does NOT provide (must be stated):**
- No hardware protection: the key is in the vault, not in the TPM/Windows Hello chip.
- No "user presence" (fingerprint/face) on every use — this is replaced by vault locking.
- The CDP virtual authenticator is an **internal/debug API**: a CEF version change can break it. Therefore:
  - the spike's first question: does it work, and does it survive a CEF update;
  - if not: we fall back to A, and communicate this to the user.

**v2 (if it's ever really needed):** path C (CEF patch) or path D (separate helper) — but only if users
actually stall because of passkey-only accounts.

## 5. What this means in daily use (honestly)

| Service | v1.0 (without passkey) | v1.1 (path B) |
|---|---|---|
| Google (password + 2FA enabled) | ✅ works | ✅ |
| Google (passkey-only, password disabled) | ❌ cannot sign in | ✅ |
| Microsoft (password + Authenticator) | ✅ | ✅ |
| Microsoft (passkey-only) | ❌ | ✅ |
| Bank (password + SMS/TOTP) | ✅ | ✅ |
| Bank (passkey-only) | ❌ | ✅ |
| GitHub, X, Discord (password + TOTP) | ✅ | ✅ |
| Registering new accounts with a passkey | ❌ (must choose a password) | ✅ |

**Conclusion:** v1.0 is enough for most people, but **not for everyone** — and the project must state this
on the website too, before anyone makes it their daily browser.

## 6. Decisions (recorded) and the remaining to-do

1. ✅ **A → B order accepted** (v1.0 without passkey, v1.1 software passkey in the vault).
2. ✅ **The software compromise accepted** (key stored in the vault; the presence check is replaced by vault
   locking + an approval window). B's drawbacks are in section 3.1, documented.
3. ⬜ **To-do (project owner):** per section `2.5.`, check in 2 minutes whether you have a **passkey-only**
   account (Google / Microsoft). If yes: B research moves **ahead of** v1.0 (into F0), if not: the order stays.
4. **Hardware** passkey (C patch / D helper process) is **excluded** — D's reasoning is in section 3.2,
   so that it doesn't have to be re-debated later.

## 7. Sources

- CEF `CefRequestHandler` reference (no WebAuthn hook): <https://cef-builds.spotifycdn.com/docs/stable/classCefRequestHandler.html>
- Chromium embedder WebAuthn interface: `content/public/browser/authenticator_request_client_delegate.h`
- CDP `WebAuthn` domain (virtual authenticator, `addCredential`, `getCredentials`):
  <https://chromedevtools.github.io/devtools-protocol/tot/WebAuthn/>
- CEFSharp `WebAuthnClient` (example of implementing the CDP path):
  <https://cefsharp.github.io/api/130.1.x/html/T_CefSharp_DevTools_WebAuthn_WebAuthnClient.htm>
