# A motorház–mag protokoll, v2

This is the written form of `crates/antikolt-ipc`. The Rust types are authoritative; this document
explains the contract to the C++ side (and to a future engine adapter) without requiring them to read
Rust. Changing anything here is a protocol change: bump `PROTOCOL_VERSION`, and record it in
`docs/HU/02-ARCHITEKTURA.md` §3.

**Version 2** adds the page's request channel: `Message::BridgeReply` and the `bridge_reply` op. A page
call marked `request` is a *persistent* `cefQuery` — the engine holds it and forwards the question as a
`bridge_call` event carrying a request number; the shell answers with `bridge_reply` and the page's
promise settles with that payload (ADR-034). A v1 shell and a v2 engine refuse each other instead of
half-working: the envelope version is checked on both sides.

## Transport

For this milestone the transport is the **child process's standard input/output**, because the engine
host is a child process with exactly one client: that is simpler and sturdier than a socket, and it
makes the lifecycle obvious (closing stdin ends the session). A local socket becomes worth its cost
only when a second client appears (a debugger, an extension host).

Rules that make stdio safe here:

1. **stdout carries only frames.** Every human-readable diagnostic goes to stderr.
2. CEF's helper processes inherit the handles, so nothing in the engine host writes to stdout except
   the frame writer.
3. A frame above **16 MiB** kills the link; it is never buffered "just in case".

The switch to a local socket (named pipe / `AF_UNIX`) is a transport swap behind the same frames and
the same [`antikolt_ipc::EngineLink`](../crates/antikolt-ipc/src/transport.rs) API.

## Framing

```
+----------------+---------------------------+
| u32 LE length  | JSON payload (Envelope)   |
+----------------+---------------------------+
```

Rules:

1. A payload above **16 MiB** is a protocol violation and the connection is closed. Do not "buffer
   more" — the limit exists so a broken peer cannot exhaust memory.
2. A truncated frame is not an error: read more and retry.
3. An unknown `op`, an unknown enum value, or a wrong `version` is an error, never a guess.
4. Snapshots and pixels do **not** travel in frames (pixels go through the frame channel, snapshots
   through a length-prefixed blob transfer that lands in F1).

## Envelope

```json
{ "version": 1, "id": 42, "message": { "op": "snapshot", "surface": 7 } }
```

- `id`: monotonic per peer. A request expects exactly one `reply` whose `to` equals this `id`.
- `version`: the sender's `PROTOCOL_VERSION`.

## Handshake

1. Client → `hello { protocol, engine }`
2. Server → `welcome { protocol, engine: EngineInfo }`

`EngineInfo.capabilities` is how the core learns what an engine can do. The core must degrade
gracefully when a capability is missing, and must never assume one:

`snapshot`, `capture`, `media_emulation`, `isolated_world`, `clear_origin_data`,
`virtual_authenticator`, `offscreen_surface`, `surface_proxy`, `frame_channel`.

## Messages

| Op | Direction | Payload | Reply |
|---|---|---|---|
| `hello` | core → engine | `protocol`, `engine` | `welcome` |
| `ping` | both | — | `reply` |
| `create_surface` | core → engine | `config` (`partition`, `viewport`, `proxy`, `proxy_bypass`, `container`, `permissions`, `is_primary`, `parent_window`, `hidden`) | `reply` (`{surface}`) |
| `destroy_surface` | core → engine | `surface` | `reply` |
| `navigate` | core → engine | `surface`, `url`, `opts` | `reply` |
| `set_bounds` | core → engine | `surface`, `bounds` (`x`, `y`, `width`, `height`) | `reply` (`{applied}`) |
| `focus` | core → engine | `surface`, `focused` (default `true`) | `reply` |
| `install_bridge` | core → engine | `surface` | `reply` |
| `eval` | core → engine | `surface`, `script`, `world` | `reply` (`{json}`) |
| `scroll` | core → engine | `surface`, `x`, `y`, `animate` | `reply` (`{ok: true, x, y}`) |
| `set_zoom` | core → engine | `surface`, `factor_percent` (25..500) | `reply` (`{factor_percent}`) |
| `set_media` | core → engine | `surface`, `media` (`auto`\|`dark`\|`light`) | `reply` (`{media: "dark"}`) |
| `set_forced_dark` | core → engine | `surface`, `mode` (`off`\|`invert`\|`dim`\|`warm`; the old `enabled` boolean still works) | `reply` (`{ok: true, mode}`) |
| `capture` | core → engine | `surface`, `format`, `quality` (default 80) | `reply` (`{bytes, width, height}` plus `path`, `format`, `surface`) |
| `snapshot` | core → engine | `surface` | `reply` (`{mhtml_bytes, integrity}`) |
| `metrics` | core → engine | `surface` | `reply` (`{dom_nodes, js_heap_bytes, engine_working_set_bytes}`) |
| `clear_origin_data` | core → engine | `surface`, `origin`, `storages[]` | `reply` (`{cleared: true, origin}`) |
| `bridge_reply` | core → engine | `request`, `json` **or** `error` | `reply` (`{}`; the page's callback is what carries the answer) |
| `shutdown` | core → engine | — | `reply`, then the process exits |
| `reply` | engine → core | `to`, `outcome` (`kind`: `ok`/`err`) | — |
| `event` | engine → core | `event` | — |

`World`: `isolated` (where the password watcher and theming live) or `main` (the page's own world —
never receives a secret).

`storages[]` for `clear_origin_data` are the identifiers produced by
`session::CleanupPlan::storage_types()`: `session_storage`, `cache_storage`, `service_workers`,
`code_cache`, `local_storage`, `indexed_db`, `cookies`. CDP spells IndexedDB `indexeddb`, so the engine
maps that one and passes the rest through (`session_storage` and `code_cache` have no identifier of their
own in `Storage.clearDataForOrigin`; the engine logs what it cannot express rather than pretending).

## The page's side of the bridge (protocol 2)

The engine injects `window.antikoltNative` on surfaces the shell enables with `install_bridge`:

```js
// One-way, answered immediately by the engine: the shell acts on the event.
await window.antikoltNative.openCard({ id, url, title, pinned });
await window.antikoltNative.freezeCard();

// A question: the engine holds the `cefQuery` open until the shell answers `bridge_reply`.
const answer = await window.antikoltNative.request('cards.search', { q: 'wiki' });
const cards = answer.cards;

// The same question through the typed helper:
const { cards: matches } = await window.antikoltNative.cards.search('wiki');

// Writing goes the same way: the whole record travels, so a pin, a note and a new card are one call.
const { card: saved } = await window.antikoltNative.cards.upsert({ ...card, pinned: true });
```

Rules:

- every question is answered **exactly once** — with `json` on success or `error` on failure; a page
  that awaits is never left hanging, because the engine drops held callbacks when the page goes away;
- an unknown method is a rejection (`ismeretlen híd-kérdés: …`), not silence;
- the shell answers from the core, so the page reads the same data the grid does — a page never keeps a
  second copy of the truth;
- only surfaces the shell enabled can call at all, and the main world is the only world that has the
  bridge (secrets do not live there, I5).

## Card previews (`antikolt://blob/…`)

A card may carry a preview image, and the grid refers to it as `antikolt://blob/<name>` (ADR-035). That
URL never reaches the network: the engine registers the `antikolt` scheme (standard, secure, CORS- and
fetch-enabled) and answers `blob/<name>` from `<user data>/previews/<name>` through
`CefStreamResourceHandler`. The shell is what fills that directory — a preview is *produced*, not
downloaded.

Rules:

- the name is resolved **inside** the directory: an empty name, a `..` or a path separator is refused, and
  a name that does not exist answers 404 — card data must never read another file through the browser;
- the content type follows the extension (`png`, `jpg`/`jpeg`, `webp`), anything else is
  `application/octet-stream`;
- the scheme is installed before the first browser exists, so a preview request from the first load works.

## Embedding the live card

`config.parent_window` is a native window handle as an integer, or `0` for "the engine host owns a
top-level window". The shell passes its card widget's `winId()`; the engine calls
`CefWindowInfo::SetAsChild` on it, so Chromium creates its window *inside the shell's window tree* and
pixels and input travel through the operating system instead of this protocol. That is what keeps the
UI and the engine in separate processes without a frame channel.

Rules that come with it:

- **Units are device independent pixels** (the page's CSS pixels), for `viewport` and `bounds` alike. On
  a 175 % monitor a 900×640 card is a 1575×1120 window, and that conversion belongs to the engine — it
  is the only side that knows the monitor's scale factor. Sending physical pixels makes Chromium scale
  a second time: measured, the page then saw a 1575 px viewport and the window grew past the screen,
  where the window manager clamped it.
- `set_bounds` may arrive **before** Chromium has created its window, because `create_surface` replies
  as soon as the browser is requested. The engine remembers the rectangle and applies it as soon as the
  child window exists; `applied: false` means "remembered, not yet placed", not "failed".
- The parent window belongs to the shell. `destroy_surface` closes the browser and forgets the handle,
  it never destroys the parent.
- `parent_window` is additive and optional: an omitted key means `0`, so a peer from before embedding
  keeps working without a protocol bump. That is the compatibility rule for this file as a whole.
- `partition` is the surface's **storage**: the engine creates one `CefRequestContext` (and one profile
  directory beside the default profile) per key, so two partitions never share cookies or a cache
  (ADR-009/ADR-071). `container`, `proxy`, `proxy_bypass` and `permissions` are the surface's **policy**:
  the container's proxy (`""` = system, `direct`, or `scheme://host:port`) becomes the context's `proxy`
  preference, and `permissions` (`{"geolocation":"allow"}`) is what the container grants — anything not
  named is denied, because this host has no permission prompt. All four keys are additive and optional,
  so an older shell sends none of them and keeps the previous behaviour.

## The JS bridge (home page → shell)

`install_bridge` turns on a narrow channel from the page to the shell for **one surface**:
the engine evaluates a small script that defines `window.antikoltNative` on top of CEF's
`window.cefQuery`. Calls arrive as `bridge_call` events with an opaque `args` object:

```
{"type":"bridge_call","surface":1,"method":"open_card","args":{"url":"https://…","title":"…"}}
```

Rules:

- The script lives in the engine, not in the shell: the shell cannot inject arbitrary JavaScript, and
  the page only ever gets the two methods the engine defines (`openCard`, `freezeCard`).
- A surface that the shell did not enable answers `Failure` to every query, so a random web page
  cannot reach the shell through the same door as the home page.
- The bridge is re-installed after every main-frame load, because a navigation creates a new JS
  context.
- Calls are answered immediately; the shell acts asynchronously (for example
  `open_card` hibernates the current surface and opens the new one). Waiting for the shell's answer
  (persistent queries) is left for the freeze milestone.

## Events

`load_started`, `load_finished` (`http_status`), `load_failed` (`reason`), `title_changed`,
`favicon_changed`, `audible_changed` (`audible`, `playing`), `form_submitted`, `popup_requested`
(`disposition`: `authenticated_window` | `plain_window` | `blocked`), `permission_requested` (`kind`),
`download_started`, `surface_created`, `surface_closed` (`reason`), `renderer_gone` (`reason`),
`shutting_down` (`reason`).

`surface_created` exists because CEF creates the browser **asynchronously**: the reply to
`create_surface` can arrive before the browser itself exists, so a client that needs to act on the
live surface waits for this event.

## What is implemented today

| Message | Engine host status |
|---|---|
| `hello` / `welcome`, `ping` | implemented |
| `create_surface` (with `parent_window`), `navigate`, `destroy_surface`, `shutdown` | implemented |
| `set_bounds`, `focus` | implemented (Windows windowed embedding) |
| `snapshot`, `eval` | implemented through DevTools (MHTML + runtime state, `Runtime.evaluate`) |
| `capture` | implemented through DevTools (`Page.captureScreenshot`; PNG/JPEG/WebP, `quality`) |
| `clear_origin_data` | implemented through DevTools (`Storage.clearDataForOrigin`; the plan's `indexed_db` is sent as CDP's `indexeddb`) |
| `scroll`, `set_zoom` | **implemented** (ADR-038): `window.scrollBy` and a device-metrics override that reflows |
| `set_media` | **implemented** (ADR-037): `Emulation.setEmulatedMedia`, pages that support the query follow it |
| `set_forced_dark` | **implemented** (ADR-044/ADR-045): one named style element; the mode is the filter applied, `off` removes it again |

`capture`'s reply carries more than the minimum `{bytes, width, height}` the table above promises: it also
names the written file (`path`), the encoding (`format`) and the surface. `width`/`height` are read from
the encoded image's own header (PNG, JPEG, WebP), so they are the compositor's pixels — not the viewport
multiplied by a device scale. Both the request and the reply are additive: `quality` defaults to 80 and a
client that ignores the extra reply fields still works.

`crates/antikolt-probe` exercises the implemented set end to end; `docs/HU/16-FEJLESZTESI-NAPLO.md`
records the verified output.

## Errors

`timeout`, `not_supported`, `invalid_argument`, `surface_not_found`, `engine_busy`, `vault_locked`,
`cancelled`, `internal`.

`not_supported` is the expected answer when the engine lacks a capability — it is not a crash and must
not take the browser down.

## Invariants the protocol enforces (and the core relies on)

- **I1:** at most one surface with `is_primary: true` may exist. A second request is answered with
  `engine_busy`, never silently accepted. The only exception is a transient authentication popup,
  which is a *window*, not a card (`docs/HU/02` §8.5).
- **I1, measured exception (ADR-041/ADR-042):** the engine host holds at most **two** surfaces — the
  grid/card surface and one short-lived background preview worker. A third `create_surface` is answered
  with `engine_busy` (`at most two surfaces at a time`). Every surface-scoped op acts on the surface it
  *names*: an unknown or already destroyed id is answered with `surface_not_found` and is never served by
  another surface. `hidden: true` in the config keeps a surface's window invisible (the shell's process
  flag `--hidden` does the same for every surface of that host).
- **I2:** `snapshot` must be awaited and completed before `destroy_surface`; a destroyed surface
  cannot be snapshotted.
- **I5:** no message carries a secret towards a `main` world `eval`. Credential filling happens only
  in `isolated`.
