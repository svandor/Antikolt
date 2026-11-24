# 06 — Idea list (not required, prioritized)

Opportunities that **follow** from the product beyond your requirements (because of disk hibernation and the
card model they come almost for free), but are not part of the v1 contract. Order = suggested introduction
time.

## A. Follow directly from disk hibernation (after F1–F2, cheap)

1. **Time travel on the card.** Because every freeze writes a snapshot, on a card you can page back:
   „what did I see 3 days ago, 2 weeks ago?" — the snapshot timeline with a slider. This is something that not a single
   browser today can do, and for you it is a *by-product*. Cost: almost zero (the blobs are already there),
   only UI + quota management. **This is the strongest unique-feature candidate.**
2. **Offline reading („can").** From the MHTML the card is also readable on a plane, train, or slow network —
   without a network, in the saved state. Cost: zero (R1 route).
3. **Search in the content of saved pages.** „In which article did I write about data handling?" — on the extracted
   text with FTS5. Cost: low (text extraction happens at save time).
4. **„Continue reading" row** on the home page: the most recently scrolled, half-finished articles, opened from the
   saved position. Cost: zero (the scroll is already saved).
5. **Deleting card versions/«keep this version»** — the user can decide which snapshots
   should remain («save this recipe»). Cost: small, part of quota management.

## B. Follow from the card model (after F3)

6. **Login-state indicator.** A small lock on the card: green = logged in, yellow = expiring soon,
   red = logged out. This makes the privacy model immediately understandable.
   *(In F3 this is no longer an idea, but part of the plan: `10-MUNKAMENET.md`.)*
7. **„Canning" button.** One click on the card: immediate disk hibernation, without the engine — useful
   after work, when „I no longer need it, but don't let it be lost".
8. **Card collection as a shareable package.** A topic's cards can be exported into a file
   (`.antikolt-collection`), and someone else can import it — snackbar: no cloud, no account.
9. **Session templates.** „Monday morning routine": 5 cards in order, opened one after another with one click
   (always only one alive, the rest on disk) — this is the memory-safe
   version of the multi-card workflow.
10. **Memory transparency.** A small panel: „this page uses 430 MB; the card grid uses 90 MB" —
    the user sees what the browser does and why. This is proof of the promise, and it teaches usage.

## C. Later, larger ideas (after F6, separate decision)

11. **Quiet hours.** After sunset the UI automatically switches to gray/dark mode, and pages are also
    dimmed. Cost: small (the theme engine already knows how).
12. **Quick-command palette (`Ctrl+K`)** — card search, container switching, workspace switching, „open
    this card in container X". Cost: medium (bridge + UI).
13. **Per-container proxy/VPN** — work container on the company VPN, private container at home. Already in the v1
    architecture (`container.proxy_json`), only the UI is needed.
14. **Gallery of your own home-page templates** („minimal", „magazine", „dashboard", „reader") — the home page
    is HTML, so the templates are too; one template = one folder.
15. **Encrypted multi-machine transfer** — the entire database (cards, vault, snapshots) can be exported into an
    encrypted archive, transferable on a USB drive. No cloud, but there is portability.

## What I deliberately do NOT recommend

- **Cloud synchronization** — the project's identity is local data; sync would be a separate product.
  (Point 15 is the compromise: file-based, encrypted transfer, with user responsibility.)
- **Live tiles / iframes on the home page** — they would start a second renderer, and would violate the I1
  invariant. The „live" feeling is provided by the fresh preview and instant opening.
- **Built-in AI assistant** — it would assume a cloud by default; if needed later, only with a local model or
  your own key, with a disabled default state.
- **Extension store** — see ADR-016.
