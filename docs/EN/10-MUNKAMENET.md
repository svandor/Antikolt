# 10 — Session management and the cost of logging in

> **Refinement of the requirement (based on your wording).** The goal is not for the browser to
> "log you in automatically everywhere". The goal is that **the important logins (Google,
> Facebook, your own site that you use) stay alive, preferably with low memory usage — and that it
> be measurable how much this costs, and what we lose if we do not keep them alive.**

This document breaks this requirement down: what goes into memory, what goes to disk, what can be
measured, and which decision loses what.

## 1. The most important fact: the login does not live in RAM

A login is essentially **a cookie + a stored token** (cookie, localStorage, more rarely IndexedDB).
These are in the engine's **on-disk** partition (cookie database, LevelDBs). When the engine is not
running:

| What | Where it is | RAM cost |
|---|---|---|
| Cookies (login token) | engine partition, disk (SQLite) | **0 bytes** |
| localStorage / IndexedDB | engine partition, disk | **0 bytes** |
| Password, TOTP | vault (SQLite, encrypted) | **0 bytes** (locked) |
| Preview, snapshot | blob store | **0 bytes** |

**Consequence:** the memory of 50–200 kept-alive logins is **practically zero**, plus a few MB of
disk. It is not the logins that consume the RAM — but the **running pages**. Chrome's 3-4 GB is not
caused by the logged-in accounts, but by the renderer of 30 open tabs.

Therefore the right promise is: **"your logins stay, because they do not need to be kept in memory"**.

## 2. Then what consumes memory? (the real trade-offs)

| Factor | Cost | Can it be kept? |
|---|---|---|
| **One open, logged-in page** (Gmail, Facebook) | 250–600 MB | only while you are viewing it; then to disk |
| **Warm engine housing (T1)** — so the card opens instantly | 180–280 MB fixed | a profile question (Frugal: off) |
| **Service Workers on hibernated pages** | a few MB / origin + network | **off by default** for hibernated cards |
| **Connection kept alive in the background** (chat, WebSocket, video) | 150–400 MB | only on a "pinned" card (documented exception) |
| **Cookie/storage data growth** | ~1–50 KB / origin, a few MB in total | yes, practically free |
| **Preview refresh** | temporarily +80–200 MB while running | scheduled, can be disabled |

**So what must be measured is not the "number of logins", but:** how many pages are running,
whether the engine is warm, whether Service Workers are running, and whether there is a pinned
background connection.

## 3. Reinterpreting the 24-hour rule (an important change)

Your earlier answer ("full cookie/data deletion after 24 hours = logout") **contradicts** the current
request (that the important logins stay alive). The resolution: the 24 hours **does not delete the
login**, but the **by-products**. There are three levels, selectable per card/container:

| Policy | What it deletes after 24 hours | What it keeps | When it is right |
|---|---|---|---|
| **`keep`** *(new default)* | nothing (only the cache LRU) | everything, including the login | Google, Facebook, your own site, forums |
| **`sweep`** | cache, `sessionStorage`, Service Worker registrations, code cache | cookies, localStorage, IndexedDB → **stays logged in** | most sites |
| **`wipe`** | everything for the origin (logout) | only the vault's password/TOTP | banking, healthcare, work system |

- By default `sweep` is the container-level default; frequently used cards can be marked with `keep`.
- `wipe` was your earlier choice — **it stays as an option**, but is no longer the default.
- The UI also measures the policy: each card shows what the rule is and what the last deletion was.

## 4. Session ledger: "let it be measurable what we lose"

For every session (container × origin) the core keeps a record, and this is what appears on the UI's
**Sessions** panel:

```
┌──────────────────────────────────────────────────────────────────────┐
│ Sessions                                           Total: 23 active │
│ ─────────────────────────────────────────────────────────────────────│
│ ● Google — work            ✔ logged in    today 08:12   1.4 MB disk │
│   RAM now: 0 MB · opened 41× · policy: keep · "if I delete it: new login" │
│ ● Facebook — personal      ✔ logged in    2 days ago    3.1 MB disk │
│   RAM now: 0 MB · 8 cookies expire in 3 days                          │
│ ● sajatoldal.hu            ✔ logged in    yesterday     0.2 MB disk │
│ ● bank (container: money)  ⏳ expired      —             0.4 MB disk │
└──────────────────────────────────────────────────────────────────────┘
   Engine: cold (0 MB) · warm engine for this: +210 MB · previews: 12 fresh
   [ Cold engine ]  [ Warm engine ]  [ Delete session… ]
```

Recorded data per session:

| Field | Meaning |
|---|---|
| `state` | `logged_in` / `logged_out` / `expired` / `unknown` (based on the last load) |
| `cookie_count`, `cookie_bytes` | the number and size of the cookies (on the partition's disk) |
| `storage_bytes` | size of localStorage + IndexedDB + Cache Storage |
| `sw_count` | number of registered Service Workers |
| `last_seen`, `last_verified` | last open / last verified login |
| `live_ram_bytes` | the measured RAM if it is currently running (0 if dormant) |
| `policy` | `keep` / `sweep` / `wipe` / `wipe_on_lock` |
| `loss_if_wipe` | what you lose (new password + TOTP, possibly CAPTCHA, 2FA re-binding) |

**The "what we lose" table** — the UI also shows this, not just the documentation:

| If we do this | RAM | Disk | We lose |
|---|---|---|---|
| 25 logins, all dormant, cold engine | ~0 MB | ~20 MB | nothing (opening takes 0.6–1.2 s) |
| The same, with a warm engine (T1) | +180–280 MB | ~20 MB | nothing, only RAM (opening takes 0.2–0.4 s) |
| 1 chat card alive in the background | +150–400 MB | ~5 MB | we lose nothing, but it *violates the rule* (I1 exception) |
| SW enabled for every logged-in origin | +5–20 MB/origin + network | — | we lose nothing, but background traffic grows |
| 24-hour `wipe` everywhere | 0 MB | 0 MB | **daily re-login, CAPTCHA, account-lockout risk** |
| 24-hour `sweep` everywhere | 0 MB | ~20 MB | nothing, only the cache empties |

## 5. "Keep it alive" (opt-in, measurable)

Two different things that the user can request per card:

1. **Pinning (pin)** — the card does not hibernate automatically. Cost: the given page's RAM,
   continuously. We only recommend it when it is truly needed (music, chat, long form).
2. **Session keep-alive** — an occasional light request with the given container's cookies, so that
   the server-side session does not expire (banks, short-lived tokens). Cost: ~0 RAM, 1 request /
   15–60 min, a little network. **Off by default**, because (a) it is unnecessary traffic, (b) some
   sites may see it as a "bot", (c) it is a privacy question. It only runs on user-marked cards, and
   is logged ("I kept it alive at 14:00 today, 200 OK").

## 6. What still does not work (documented loss)

- **Chat/notifications running in the background** only with pinning — and then that card occupies
  RAM (I1 exception).
- **We cannot prevent the server-side session from expiring** if the site does not provide a refresh
  token; in that case the runbook logs in again (if the vault can), or the user does.
- **Without a passkey** some sites offer the password path — this is the topic of `12-PASSKEY.md`.
- **The "is it logged in?" state is a heuristic**: the browser cannot know for sure, it only infers
  from whether, after the page loads, there is a login form or a "log out" link. That is why an
  `unknown` state exists, and the UI shows this honestly.

## 7. Consequences for the plan (briefly)

- The vault and the containers stay, but the emphasis is on the **session registry** (new table:
  `session`), not on "automatic login".
- The `wipe` policy becomes an option, `keep`/`sweep` are the default — this is the new meaning of
  the 24-hour rule.
- The Sessions panel is the main "identity" surface alongside the vault.
- Measurement is part of it: RAM (dormant vs live), disk, and the "what we lose" list — as a
  complement to `03-MEMORIA.md` and the `perf/` reports.
