# The Antikolt shell (Qt 6)

This is the native shell: window, chrome, and the host for the home page. It is deliberately small —
the web content lives in the engine host (a separate process, ADR-004), and the domain logic lives in
the Rust core (`crates/`).

## Building

From the repository root, on Windows:

```powershell
pwsh -File scripts/build.ps1                 # configure + build + self test
pwsh -File scripts/build.ps1 -CheckOnly      # just report the toolchain
pwsh -File scripts/build.ps1 -Clean -Config Debug
```

The script discovers everything it needs (MSVC, Windows SDK, CMake, Ninja, Qt 6) and explains why it
does not simply call `vcvars64.bat`: on a Build Tools-only machine that stub does not reliably produce
a compiler environment, and the "Visual Studio 17 2022" CMake generator needs a registered VS instance.
See the header of `scripts/build.ps1`.

### Doing it by hand

```powershell
cmake -S . -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DCMAKE_PREFIX_PATH=D:/Tools/Qt/6.8.3/msvc2022_64 `
  -DCMAKE_CXX_COMPILER='C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/<ver>/bin/Hostx64/x64/cl.exe'
cmake --build build
build/apps/desktop-shell/antikolt-shell.exe --selftest -platform offscreen
```

`PATH` must contain the Qt `bin`, the MSVC `bin\Hostx64\x64` and the SDK `bin\<ver>\x64` directories,
and `INCLUDE`/`LIB` must point at MSVC and the SDK. The script does this for you.

## What `--selftest` proves

```
antikolt-shell 0.1.0
qt=6.8.3
platform=offscreen
screens=1
dpr=1
quick=1
theme=light ok
theme=grey ok
theme=dark ok
theme=amoled ok
ok
```

It initialises the UI stack (no window shown), prints the Qt version and platform plugin, and checks
that each of the four documented themes produces a stylesheet that carries its own background and
accent colours. A build that cannot do this is not a working build, so CI and the build script both
run it.

## The engine-backed checks

Each of these starts the engine host (and a real browser surface) and exits with 0 only if every line
below it says `ok`:

```powershell
antikolt-shell.exe --embedtest  build/apps/engine-host/antikolt-engine-host.exe
antikolt-shell.exe --opentest   build/apps/engine-host/antikolt-engine-host.exe
antikolt-shell.exe --capturetest build/apps/engine-host/antikolt-engine-host.exe
antikolt-shell.exe --gridtest   build/apps/engine-host/antikolt-engine-host.exe
antikolt-shell.exe --lifecycletest build/apps/engine-host/antikolt-engine-host.exe
antikolt-shell.exe --previewworkertest build/apps/engine-host/antikolt-engine-host.exe
antikolt-shell.exe --traytest
antikolt-shell.exe --themetest  build/apps/engine-host/antikolt-engine-host.exe
antikolt-shell.exe --coretest   target/debug/antikolt-core-service.exe
```

- `--embedtest` — the engine really drew into the Qt card's window: Chromium's child window exists
  inside the card's handle, the page's own viewport equals the card's size, the painting and the focus
  travel in both directions. Painting is measured with the engine's own `capture` (ADR-030); the screen
  grab is printed as a diagnostic only.
- `--opentest` — the product loop: the home page calls the injected bridge, the shell hibernates the
  grid and opens the requested card as a new surface on a new partition.
- `--capturetest` — `capture` returns a real picture of a three-band page through Chromium's compositor,
  at exactly the viewport × device scale, with distinct colours.
- `--gridtest` — the home grid shows the cards the **core** owns: the core is asked for its list first
  (the oracle), then the page reports what it displays through `window.antikoltGridState()`, and the two
  must agree. A push that never arrived, arrived empty or was overwritten by the page's sample cards
  fails here. Finally it clicks the grid's first tile and requires the shell to answer with a bridge
  call carrying that card's URL and a new surface for it.
- `--lifecycletest` — a live card with no user activity hibernates by itself: with
  `ANTIKOLT_HIBERNATE_MS` shortened, the card's surface is closed, a snapshot is written, and the grid
  comes back on its own (the ten-minute default would only measure patience).
- `--coretest` — the shell talks to the Rust core service: handshake, card list, card open (timestamp),
  and the session ledger end to end (write a row, list it, let the core judge it due, close it, see it
  gone) — the policy is evaluated in the core, see ADR-033.
- `--sessionpanel` — the session panel shows the core's ledger: a row is written into the service store,
  the window opens its Munkamenetek dock, and the panel's own text must contain that origin and the
  policy's verdict (`lejárt`). The window is also grabbed to a PNG, so a blank table shows up.
- `--wipetest` — the engine can clear one origin's storage: a local `http://` origin (served by a socket
  in the test process) writes a cookie and a localStorage entry, the test reads them, asks the engine to
  clear them through `Storage.clearDataForOrigin`, and requires the second read to come back empty. The
  wipe is proven by the difference, not by the reply.
- `--pintest` — the grid can write: the first card's pin badge is clicked inside its shadow root, and the
  click must travel through the bridge (`cards.upsert`) to the core. The assertion is the *flip*: the
  page's witness (`window.antikoltPinState`) and a fresh core client must both report the opposite of what
  the card had before, whichever direction that is — a click that changed nothing would pass a weaker
  test.
- `--previewtest` — a card preview is served, not fetched (ADR-035): a PNG is written into the profile's
  `previews` directory, the home page loads it as `antikolt://blob/probe.png`, and the page's own witness
  must report `naturalWidth × naturalHeight`. A second request for `..%2Fhost.cpp` must be *refused* — a
  card's data may not read another file through the browser.
- `--scrollrestore` — the reader's place end to end: scroll a card, go home, reopen it, and the page must be at
  the same offset (`first_scroll_y=700`, `restored_scroll_y=700`). Two passes through the same handler, with the
  click polling for the card because the list is pushed after the load.- `--scrollbook` — where the reader stopped is remembered per card (`scroll.json`): a positive offset survives a
  reload from disk, going back to the top forgets that card, and one card's offset does not overwrite another's.- `--previewbudget` — the daily budget persists: a scratch `budget.json` counts three previews, refuses at
  five, and a file written for another day reads as zero (a counter that resets on launch is not a budget).- `--previewrefresh` — the preview refresh policy: a missing file must be regenerated, a fresh one must not,
  a 30 day old one must, and a file whose timestamp is in the future must not turn every open into a
  screenshot.- `--previewclean` — the preview janitor: `previews/` is swept for files the core no longer references, while
  the referenced image and a non-preview file survive, and a missing directory is not an error.- `--rulestest` — the session rules can be changed over the wire (ADR-039): a 2 hour policy is stored and
  read back, an impossible 1 second TTL is *refused* by the core, and the stored policy stays the first one.- `--scrollzoomtest` — scrolling and zoom reach the page (ADR-038): the card is a tall local page, `scroll`
  moves it (`scrollY=600`), and `set_zoom` multiplies the screen's own DPR (`dpr_at_150=1.875` on a 125 %
  display, back to `1.25` at 100 %), and the shell's own `setZoomPercent` path (the toolbar calls it) must
  give `dpr_at_125=1.5625`. The assertion is the *ratio*, which does not depend on the display.- `--mediatest` — the site theme reaches the page (ADR-037): `setPageMedia("dark")` must make the page's own
  `matchMedia('(prefers-color-scheme: dark)')` true, and `"light"` must make it false again. Both directions,
  because a page that only ever reports dark is not following the browser, it is dark.
- `--metricstest` — the memory figure is measured (ADR-036): the engine counts the page (DOM nodes, JS
  heap) and reports its own working set, and the page's `system.memory` question comes back through the
  request channel with a non-zero `usedBytes`. Two witnesses: the engine's reply and the page's answer.
- `--previewgentest` — opening a card *produces* its preview: a card pointing at the local probe origin is
  written into the core without a picture, the grid shows it, the test clicks it, and once the page has
  loaded the shell captures it, moves the image into `previews/` and writes the name back to the record.
  Three witnesses: the file on disk, the record's `preview_ref`, and the page loading
  `antikolt://blob/<name>`.
- `--previewworkertest` — the background preview worker (ADR-041, ADR-042): a card with no picture is
  photographed by a short-lived hidden surface on the grid's own engine, without the user opening it.
  Three phases: a 5 ms ceiling (set through `ANTIKOLT_PREVIEW_DEADLINE_MS`) must end a job by itself and
  write nothing, not even the surface the engine answers with after the fact; a real job must produce
  `previews/<id>.png`, set the record's `preview_ref`, spend exactly one unit of the daily budget and leave
  `idle` with no live surface; and opening a card *while* the worker's surface request is in flight must
  abort it — the surface that arrives late is destroyed, no preview is written, and the card loads. The
  check reads the surface state from the engine (`surface_not_found` for the dead id), not from the shell.
  A machine whose compositor sometimes misses the first frame may need a second pass; the queue retries the
  same card, so the check does too (at most three, reported as `worker_attempts`).
- `--timeouttest` — every page request is answered exactly once: a deliberately unanswered method
  (`test.hang`, reachable only with `ANTIKOLT_BRIDGE_TEST=1`) must be *rejected* by the shell's watchdog in
  about five seconds. A promise nobody settles would leak into every later interaction.
- `--notetest` — the card's note editor writes too: the editor opens in place on the card (a re-render
  would throw the typed text away), "Mentés" goes through `cards.upsert` to the core, and four witnesses must
  agree — the editor's answer, the grid model, a fresh core client's record, and the *rendered* note (the card is
  rebuilt twice, so the probe polls the shadow DOM instead of reading the old element at click time) — plus the
  focus: after the save the rebuilt card owns it (`note_focus=<id>:card`), which the page restores synchronously
  as well as on an animation frame, because a background window may never deliver the frame.
- `--cleanuptest` — the policy's verdict is executed: a due row is written with `last_seen_ms: 0` and a
  `wipe` policy, the window cleans up while the grid is showing (nothing can be wiped immediately — one
  surface, I1), then the card for that origin is opened and the engine clears it. The page's own witness
  (`window.__wrote`) must be true, its storage empty, and the ledger row gone. The ledger is read by a
  fresh client, because a client that was already running reports its own stale copy.
- `--bridgetest` — the page can *ask* (protocol 2, ADR-034): the home page calls
  `window.antikoltNative.request('cards.search', {q:'wiki'})`, the engine holds the persistent `cefQuery`,
  the shell answers from the core, and the page's own variable receives the answer. Then it flips the
  found card's `pinned` through `cards.upsert` and asks again: the second answer must already carry the
  new value, which proves the answer comes from the core and not from a page-local copy. An unknown
  method must be rejected, not left hanging — a promise nobody settles is a hung page.
- `--logtest` — the shell log file is written, rotated and switched off by path (ADR-046). The lines go through
  the same message handler as stderr (`ForwardQtMessages`), so the check reads back real `[shell] …` lines with a
  timestamp, pushes the file past `ANTIKOLT_SHELL_LOG_LIMIT` and expects `shell.log.1` to appear, and then clears
  the path and expects no further writes. A self test writes the user's log by accident exactly once; this is
  the check that keeps it from happening twice.
- `--addcardtest` — the home page is editable (ADR-061). The check presses the `＋` button, types a URL and a title
  into the inline form, presses Enter, and compares the page's model with the core record read by a fresh client
  (title, URL and workspace all have to match what was typed). Then it removes the card through the page's own
  delete path, so the profile is left as it was found. Measured: `add_card=ok before=10 after=11 page=11`,
  `add_card_record=ok … workspace=w-home`, `delete_card=ok remaining=10`. The same check renames the card in place
  (ADR-062) and reports *which* path wrote it — `edit_card_title=ok gesture=typed wrote_by=gesture …`, so a silently
  dead editor cannot hide behind a working fallback. The URL is edited the same way through the domain span
  (ADR-063): `edit_card_url=ok gesture=typed stored="https://proba.example/masik-cikk"`, read back from the core.
  The check also asks the storage through the page (ADR-064 → ADR-070): `db_status=ok schema=4 cards=11 verified=yes`,
  which means all four stores are SQLite rows (`user_version = 4`): the four exports written from them parse back into
  exactly the same rows, and no store is left behind.
- `--profiletest` — the portable bundle (ADR-059): `profile_export` writes the whole profile (cards, containers,
  workspaces, sessions, rules) into one versioned JSON file, and `profile_import` reads it back into a *fresh*
  profile. The check needs no window: it runs two core processes with two throw-away data directories, exports
  from the first, imports into the second, and reads the *files* back through the second core. A bundle with an
  unknown schema number is refused instead of half-imported. Measured: `profile_export=ok`, `profile_import=ok`,
  `profile_refused=ok message="unsupported bundle schema: 99 (this core writes 1)"`. The last step opens a window
  and drives the *page* (`window.antikoltProfileWrite/Read` → bridge `profile.export/import`), because a bundle
  the shell cannot hand to a page is not a backup either: `profile_bridge=ok wrote_cards=10 read_cards=10`.
- `--tagtest` — tags are real: the card carries them, the core normalises them, and the search finds them
  (ADR-055). The check clicks the card's own `＋címke` button, types `kávézó`, presses Enter, and compares the
  page's model with the core record read by a fresh client; then it searches for `kavezo` (no accents) and expects
  the tagged card among the matches, and finally clicks the chip's `×` and expects the core to be clean again.
  Measured: `tag_add=ok`, `tag_core=ok`, `tag_search=ok … term=kavezo`, `tag_remove=ok`.
- `--workspacetest` — the tab row is the core's workspace table, and a card's workspace is a stored field
  (ADR-056). The check compares the page's tab row and the DOM tabs with the core's workspace table (SQLite rows since
  ADR-070), asserts
  that every card's workspace is one the table knows (the old `index > 12` fiction could invent "Olvasnivaló"
  for the thirteenth card), moves one card through the page's own write path (`antikoltMoveToWorkspace` →
  `cards.upsert` → core), and puts it back. Measured: `workspace_table=ok`, `workspace_cards=ok`,
  `workspace_move=ok`, `workspace_core=ok`, `workspace_restored=ok`. The same check then drives the tab row's
  own lifecycle (ADR-057): `＋` opens an inline name field, a double click on a tab renames it, and `×` deletes
  it — a workspace that still holds cards is refused, with the core's words shown on the row
  (`workspace_created=ok`, `workspace_renamed=ok`, `workspace_used_delete=ok`, `workspace_deleted=ok`).
  The card move itself is the card's own `.workspace-select` (ADR-058), so the check drives the same control the
  user does.
- `--containertest` — the container story, end to end (ADR-050…ADR-054).
  The card carries a native `<select>` in its meta row; the check picks the first card, changes the container
  through that select (event → `cards.upsert` → core), and compares three witnesses: the page's state after the
  switch, the core record read by a fresh client, and the model's container map. It then puts the card back,
  because a check must not rearrange the user's cards. The same check measures the legend's filter
  (`container_legend=ok`, `container_filter=ok`, `container_filter_cleared=ok`): the chip row above the grid is
  also the container filter (ADR-051), and its counts follow the search. Finally the table and its lifecycle, which
  live in the core (`containers.json`): the check clicks `＋ konténer`, types a name, submits the inline form, then
  double clicks the chip to rename it and clicks `×` to delete it — never calling the core directly — reloads the
  grid, and expects the legend to draw what the core holds (`container_table=ok`, `container_created=ok`,
  `container_reload=ok`, `container_rename=ok`). A container that still holds cards is refused
  (`container_used_delete=ok`, with the core's own words shown and a move row offered); picking another container
  there moves the cards first and deletes afterwards (`container_move_delete=ok`, with the card found in its new
  container). There is no JS dialog in this product (ADR-053).
