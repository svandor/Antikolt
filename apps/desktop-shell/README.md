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

## What is not here yet

- The CEF engine host (`apps/engine-host`): created when `ANTIKOLT_CEF_ROOT` points at an extracted
  CEF tree (`pwsh -File scripts/install-cef.ps1`). Until then the window shows a placeholder.
- The home page host and the bridge (`docs/HU/02-ARCHITEKTURA.md` §11.2).
- The session panel, the vault UI, the theme ladder for pages.

The next milestone is the F0 spike: one CEF surface, the three reference pages, and a real memory
measurement (`perf/README.md`).
