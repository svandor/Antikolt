# CEF WebAuthn / Passkey Support Report (2026)

**1. Does CEF implement the embedder-side WebAuthn integration?**
No. CEF does not implement Chromium's `WebAuthenticationDelegate` / `AuthenticatorRequestClientDelegate` interfaces. The `CefRequestHandler` class (`cef_request_handler.h`) has zero WebAuthn methods. The `content/public/browser/content_browser_client.h` lists `WebAuthenticationDelegate` as a class Chromium embedders must provide, but CEF does not expose or implement it. `navigator.credentials.create/get` with `publicKey` will either fall back to the default Chromium behavior or throw `NotSupportedError` depending on CEF version.

- **Source**: `content/public/browser/content_browser_client.h` (`WebAuthenticationDelegate` declaration)【0†L1-L35】; `CefRequestHandler` reference【1†L1-L78】; CEF forum FIDO thread【7†L1-L5】
- **Confidence**: VERIFIED — confirmed from CEF source and API docs.

**2. What is missing and how hard to add it?**
The entire embedder-side WebAuthn API is missing. There is no `CefWebAuthn*`, no `CefBrowserHost` WebAuthn endpoint, and no `CefRequestHandler` slot for it. To add it, an embedder would need to: (a) implement `WebAuthenticationDelegate` / `AuthenticatorRequestClientDelegate` in a CEF-subclassed `CefBrowserProcessHandler`, (b) wire the `AuthenticatorRequestClientDelegate` into the CEF render process, and (c) handle CTAP2/FIDO2 message passing. This is a **large** undertaking — on par with a partial Chromium port.

- **Source**: `CefRequestHandler` method list (no WebAuthn)【1†L1-L78】; `content/public/browser/authenticator_request_client_delegate.h` full interface【5†L1-L80】
- **Confidence**: VERIFIED — exhaustive CEF header scan.

**3. Can CDP `WebAuthn.enable` + `addVirtualAuthenticator` be used as a persistent software authenticator?**
Yes, but only via the Chrome DevTools Protocol (CDP), not as a native CEF API. CEFSharp’s `WebAuthnClient` (CDP-bound) exposes `EnableAsync`, `AddVirtualAuthenticatorAsync`, `AddCredentialAsync`, `GetCredentialsAsync`, and `Credential.PrivateKey` (PKCS#8 exportable). Virtual authenticators support `HasResidentKey`, `HasUserVerification`, `IsUserVerified` (configurable defaults: false/true), and `AutomaticPresenceSimulation` (default **true**). Credentials **persist across navigations** within the same CDP session, but **do not survive browser restart** — they live in the DevTools session state. `WebAuthn.addCredential` can re-inject previously exported private keys.

- **Source**: CEFSharp `WebAuthnClient` API v130.1【2†L1-L50】; `VirtualAuthenticatorOptions` properties (HasResidentKey, HasUserVerification, IsUserVerified, AutomaticPresenceSimulation)【2†L51-L80】; `Credential.PrivateKey` property【4†L31-L45】; `GetCredentialsAsync` / `AddCredentialAsync` methods【3†L1-L25】
- **Confidence**: VERIFIED — from CEFSharp API docs and CDP protocol behavior.

**4. Other routes for platform authenticators in CEF?**
- **No CEF-native bridge**: Windows Hello / `libfido2` / CTAP2 are not exposed through CEF APIs. A CEF patch series would be required to add them.
- **Recommended practical approach**: Run a small separate Chromium-based helper (e.g., Electron or mini-Chromium) solely for WebAuthn, communicating with the CEF shell via IPC or a local web server. This avoids CEF modification and leverages Chromium’s full WebAuthn support.
- **CDP-only route**: Use CEFSharp’s `WebAuthnClient` for testing / software-only passkeys, but expect limitations on real device (platform authenticator) support and cross-restart persistence.

- **Source**: Absence of WebAuthn in `CefRequestHandler`【1†L1-L78】; CEF forum “CEF fido Plugin security key popup message not showing”【7†L1-L5】; CEFSharp CDP WebAuthn implementation.

**5. Site behavior when CEF browser has no passkey support?**
Most sites fall back to password + TOTP when WebAuthn is unavailable. However, accounts pre-configured for passkey‑only auth (Google, Microsoft, Apple ID) may **hard‑require** the passkey and block password entry, showing a “required passkey” error. Some sites offer a “use password instead” fallback; others do not.

- **Source**: General WebAuthn fallback behavior (Chrome/Chromium site docs); Google/Microsoft account passkey‑only policies.

**Bottom line**: CEF has no native WebAuthn embedder support. Use a CDP‑based virtual authenticator (CEFSharp `WebAuthnClient`) for software passkey workflows with credential export, or isolate WebAuthn in a separate Chromium helper for real platform authenticator support.