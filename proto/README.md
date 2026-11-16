# A motorház–mag protokoll, v1

This is the written form of `crates/antikolt-ipc`. The Rust types are authoritative; this document
explains the contract to the C++ side (and to a future engine adapter) without requiring them to read
Rust. Changing anything here is a protocol change: bump `PROTOCOL_VERSION`, and record it in
`docs/HU/02-ARCHITEKTURA.md` §3.

## Transport

- Local socket only: a Windows named pipe or a Linux `AF_UNIX` socket in the user's runtime directory.
- Access restricted to the same user (`0600` on Unix, a per-user pipe name with a random suffix).
- The engine host **never** receives a filesystem path; it only ever learns a `partition` key.

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
| `create_surface` | core → engine | `config` (`partition`, `viewport`, `proxy`, `is_primary`) | `reply` (`{surface}`) |
| `destroy_surface` | core → engine | `surface` | `reply` |
| `navigate` | core → engine | `surface`, `url`, `opts` | `reply` |
| `eval` | core → engine | `surface`, `script`, `world` | `reply` (`{json}`) |
| `scroll` | core → engine | `surface`, `x`, `y`, `animate` | `reply` |
| `set_zoom` | core → engine | `surface`, `factor_percent` | `reply` |
| `set_media` | core → engine | `surface`, `scheme`, `forced_colors` | `reply` |
| `capture` | core → engine | `surface`, `format`, `quality` (default 80) | `reply` (`{bytes, width, height}`) |
| `snapshot` | core → engine | `surface` | `reply` (`{mhtml_bytes, integrity}`) |
| `metrics` | core → engine | `surface` | `reply` (`{dom_nodes, js_heap_bytes, engine_working_set_bytes}`) |
| `clear_origin_data` | core → engine | `partition`, `origin`, `storages[]` | `reply` |
| `reply` | engine → core | `to`, `outcome` | — |
| `event` | engine → core | `event` | — |

`World`: `isolated` (where the password watcher and theming live) or `main` (the page's own world —
never receives a secret).

`storages[]` for `clear_origin_data` are the identifiers produced by
`session::CleanupPlan::storage_types()`: `session_storage`, `cache_storage`, `service_workers`,
`code_cache`, `local_storage`, `indexed_db`, `cookies`.

## Events

`load_started`, `load_finished` (`http_status`), `load_failed` (`reason`), `title_changed`,
`favicon_changed`, `audible_changed` (`audible`, `playing`), `form_submitted`, `popup_requested`
(`disposition`: `authenticated_window` | `plain_window` | `blocked`), `permission_requested` (`kind`),
`download_started`, `renderer_gone` (`reason`), `shutting_down` (`reason`).

## Errors

`timeout`, `not_supported`, `invalid_argument`, `surface_not_found`, `engine_busy`, `vault_locked`,
`cancelled`, `internal`.

`not_supported` is the expected answer when the engine lacks a capability — it is not a crash and must
not take the browser down.

## Invariants the protocol enforces (and the core relies on)

- **I1:** at most one surface with `is_primary: true` may exist. A second request is answered with
  `engine_busy`, never silently accepted. The only exception is a transient authentication popup,
  which is a *window*, not a card (`docs/HU/02` §8.5).
- **I2:** `snapshot` must be awaited and completed before `destroy_surface`; a destroyed surface
  cannot be snapshotted.
- **I5:** no message carries a secret towards a `main` world `eval`. Credential filling happens only
  in `isolated`.
