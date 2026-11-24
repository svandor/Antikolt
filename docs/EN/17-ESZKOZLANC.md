# 17 — Toolchain and build

This document records **what is installed and where**, how to build, and which pitfalls we avoid
with the build script. The goal is that on a new machine (or a month from now) we don't have to
figure it out again.

## 1. Installed tools on the target machine (VECTOR)

| Tool | Version | Location |
|---|---|---|
| **MSVC** | 19.44.35223 (BuildTools 14.44.35207) | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207` |
| **Windows SDK** | 10.0.26100.0 | `C:\Program Files (x86)\Windows Kits\10` |
| **CMake** | 3.31.6 (comes with BuildTools) | `…\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` |
| **Ninja** | comes with BuildTools | `…\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe` |
| **Qt** | 6.8.3 (MSVC 2022 x64; qtbase + qtdeclarative + qtsvg) | `D:\Tools\Qt\6.8.3\msvc2022_64` |
| **CEF** | 158.0.3+g8c29be2 (Chromium 158.0.8127.18) — **beta channel** | `D:\Tools\cef\cef_binary_158.0.3+…_windows64_beta` |
| **Rust** | 1.98.0 (stable, msvc) | `%USERPROFILE%\.cargo\bin` |
| **aqtinstall** | 3.3.0 (pip) | `%LOCALAPPDATA%\…\pip` (for installing Qt) |

> **CEF channel:** the first installation chose the latest available build, which is beta (158.0.3).
> This is fine for development, but **a switch to the stable channel is required before release** — the
> `scripts/install-cef.ps1` already picks the latest *stable* archive, if one exists.

## 2. Installation (on a new machine)

```powershell
# Qt 6 (no GUI installer needed; works from the official archives)
python -m pip install --upgrade aqtinstall
python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O D:\Tools\Qt `
  --archives qtbase qtdeclarative qtsvg

# CEF (the latest stable standard build; from the repo root)
pwsh -File scripts/install-cef.ps1

# Rust (if not already present)
winget install Rustlang.Rustup
```

## 3. Build

```powershell
pwsh -File scripts/build.ps1                 # configure + build + both self-tests
pwsh -File scripts/build.ps1 -CheckOnly      # toolchain check only
pwsh -File scripts/build.ps1 -Clean -Config Debug
```

The script **automatically detects** everything (MSVC via `vswhere`, the latest SDK and MSVC on disk,
Qt under `D:\Tools\Qt`, CEF under `D:\Tools\cef`), and sets up the `PATH`/`INCLUDE`/`LIB` environment.

Output:

```
=== Antikolt toolchain ===
  Visual Studio : C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
  MSVC          : 14.44.35207
  Windows SDK   : 10.0.26100.0
  Qt            : D:\Tools\Qt\6.8.3\msvc2022_64
  CEF           : D:\Tools\cef\cef_binary_158.0.3+…_windows64_beta
```

## 4. Four pitfalls the script avoids (and why)

1. **`vcvars64.bat` does not work here.** The file is 39 bytes: it just forwards to `vcvarsall.bat`, and on this
   machine it does not create a usable compiler environment (`CMAKE_CXX_COMPILER` is not found). Therefore the
   script sets the `PATH`/`INCLUDE`/`LIB` variables **directly** from the located MSVC and SDK
   directories.
2. **The "Visual Studio 17 2022" CMake generator does not find the Build Tools installation.** (There is no
   registered VS instance.) Therefore the build uses the **Ninja** generator, with absolute paths to
   `cmake.exe` and `ninja.exe`.
3. **A backslash path in the CMake cache causes a syntax error** (e.g. `rc.exe` → "Invalid
   character escape '\P'"). Therefore every path passed to CMake uses **forward slashes**.
4. **The CEF download index is not in chronological order.** The first script version therefore picked the *oldest* build
   (Chromium 51, 2016). The fixed script **sorts versions numerically** and picks the
   latest one.

## 5. CEF integration rules (in code)

- CEF include: the **CEF root** goes on the include path, and sources reference it as `include/cef_app.h`
  (this is CEF's own convention).
- Linking: `${CEF_ROOT}/Release/libcef.lib`.
- **Runtime files are required** alongside the executable: `libcef.dll`, `chrome_elf.dll`,
  `d3dcompiler_47.dll`, `vulkan-1.dll`, `vk_swiftshader*`, `v8_context_snapshot.bin`, as well as from the
  `Resources/` folder: `icudtl.dat`, `resources.pak`, `chrome_*_percent.pak`, and the `locales/` folder.
  These are copied next to the output by the POST_BUILD step in `apps/engine-host/CMakeLists.txt`.
- The sandbox (`cef_sandbox.lib`, `CefScopedSandboxInfo`) is **not yet** enabled; when the browser window
  is complete it will be mandatory (security requirement, `docs/HU/02` §14).

## 6. What the toolchain cannot do yet

| Gap | Impact | Solution |
|---|---|---|
| CEF `libcef_dll_wrapper` is not wired up | we only use the C API; the wrapper (C++ classes) is needed later | `add_subdirectory(${CEF_ROOT}/libcef_dll …, )` in the next engine-host step |
| No `cef_sandbox` | the engine is not sandboxed | together with the browser window |
| CEF beta channel | not suitable for release | choose the stable archive before release |
| No `windeployqt` in the deployment | manual Qt `bin` on the PATH to run | as an installer step (`windeployqt`) later |
