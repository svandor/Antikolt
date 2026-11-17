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

Requires `PATH` to contain the Qt `bin` directory (the shell links Qt dynamically).

## What is not here yet

- The CEF engine host (`apps/engine-host`): created when `ANTIKOLT_CEF_ROOT` points at an extracted
  CEF tree (`pwsh -File scripts/install-cef.ps1`). Until then the window shows a placeholder.
- The home page host and the bridge (`docs/HU/02-ARCHITEKTURA.md` §11.2).
- The session panel, the vault UI, the theme ladder for pages.

The next milestone is the F0 spike: one CEF surface, the three reference pages, and a real memory
measurement (`perf/README.md`).
