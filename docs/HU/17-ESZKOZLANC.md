# 17 — Eszközlánc és build

Ez a dokumentum rögzíti, **mi van telepítve, hova**, hogyan kell építeni, és milyen buktatókat
kerülünk el a build szkripttel. A cél, hogy egy új gépen (vagy egy hónap múlva) ne kelljen újra
kitalálni.

## 1. Telepített eszközök a célgépen (VECTOR)

| Eszköz | Verzió | Hely |
|---|---|---|
| **MSVC** | 19.44.35223 (BuildTools 14.44.35207) | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207` |
| **Windows SDK** | 10.0.26100.0 | `C:\Program Files (x86)\Windows Kits\10` |
| **CMake** | 3.31.6 (a BuildTools-szal jön) | `…\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` |
| **Ninja** | a BuildTools-szal jön | `…\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe` |
| **Qt** | 6.8.3 (MSVC 2022 x64; qtbase + qtdeclarative + qtsvg) | `D:\Tools\Qt\6.8.3\msvc2022_64` |
| **CEF** | 158.0.3+g8c29be2 (Chromium 158.0.8127.18) — **beta csatorna** | `D:\Tools\cef\cef_binary_158.0.3+…_windows64_beta` |
| **Rust** | 1.98.0 (stable, msvc) | `%USERPROFILE%\.cargo\bin` |
| **aqtinstall** | 3.3.0 (pip) | `%LOCALAPPDATA%\…\pip` (a Qt telepítéséhez) |

> **CEF csatorna:** az első telepítés a legfrissebb elérhető buildet választotta, ami beta (158.0.3).
> Ez a fejlesztéshez megfelelő, de **kiadás előtt stabil csatornára kell váltani** — a
> `scripts/install-cef.ps1` már a legújabb *stabil* archívumot választja, ha van ilyen.

## 2. Telepítés (új gépen)

```powershell
# Qt 6 (nem kell GUI-s installer, a hivatalos archívumokból dolgozik)
python -m pip install --upgrade aqtinstall
python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O D:\Tools\Qt `
  --archives qtbase qtdeclarative qtsvg

# CEF (a legújabb stabil standard build; a repó gyökeréből)
pwsh -File scripts/install-cef.ps1

# Rust (ha még nincs)
winget install Rustlang.Rustup
```

## 3. Build

```powershell
pwsh -File scripts/build.ps1                 # configure + build + mindkét önteszt
pwsh -File scripts/build.ps1 -CheckOnly      # csak az eszközlánc ellenőrzése
pwsh -File scripts/build.ps1 -Clean -Config Debug
```

A szkript **automatikusan felderít** mindent (MSVC a `vswhere`-rel, a legújabb SDK és MSVC a lemezen,
Qt a `D:\Tools\Qt` alatt, CEF a `D:\Tools\cef` alatt), és beállítja a `PATH`/`INCLUDE`/`LIB` környezetet.

Kimenet:

```
=== Antikolt toolchain ===
  Visual Studio : C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
  MSVC          : 14.44.35207
  Windows SDK   : 10.0.26100.0
  Qt            : D:\Tools\Qt\6.8.3\msvc2022_64
  CEF           : D:\Tools\cef\cef_binary_158.0.3+…_windows64_beta
```

## 4. Négy buktató, amit a szkript kerül el (és miért)

1. **`vcvars64.bat` nem működik itt.** A fájl 39 bájt: csak továbbít a `vcvarsall.bat`-ra, és ezen a
   gépen nem hoz létre használható fordító-környezetet (`CMAKE_CXX_COMPILER` nem található). Ezért a
   szkript a `PATH`/`INCLUDE`/`LIB` változókat **közvetlenül** állítja be a megtalált MSVC- és SDK-
   könyvtárakból.
2. **A „Visual Studio 17 2022" CMake-generátor nem találja a Build Tools telepítést.** (Nincs
   regisztrált VS-instance.) Ezért a build **Ninja** generátort használ, abszolút útvonalakkal a
   `cmake.exe`-re és a `ninja.exe`-re.
3. **Backslash-es útvonal a CMake cache-ben szintaktikai hibát okoz** (pl. `rc.exe` → „Invalid
   character escape '\P'"). Ezért minden CMake-nek átadott útvonal **perjeles**.
4. **A CEF letöltési indexe nem időrendben van.** Az első szkriptverzió ezért a *legrégebbi* buildet
   (Chromium 51, 2016) választotta. A javított szkript **numerikusan rendezi** a verziókat, és a
   legújabbat választja.

## 5. CEF-integrációs szabályok (kódban)

- CEF include: a **CEF gyökér** kerül az include-path-ra, a források `include/cef_app.h` formában
  hivatkoznak (ez a CEF saját konvenciója).
- Linkelés: `${CEF_ROOT}/Release/libcef.lib`.
- **Futásidejű fájlok kötelezők** a futtatható mellett: `libcef.dll`, `chrome_elf.dll`,
  `d3dcompiler_47.dll`, `vulkan-1.dll`, `vk_swiftshader*`, `v8_context_snapshot.bin`, valamint a
  `Resources/` mappából `icudtl.dat`, `resources.pak`, `chrome_*_percent.pak` és a `locales/` mappa.
  Ezeket a `apps/engine-host/CMakeLists.txt` POST_BUILD lépése másolja a kimenet mellé.
- A sandbox (`cef_sandbox.lib`, `CefScopedSandboxInfo`) **még nincs** bekapcsolva; a böngészőablak
  elkészültekor kötelező lesz (biztonsági követelmény, `docs/HU/02` §14).

## 6. Amit az eszközlánc még nem tud

| Hiány | Hatás | Megoldás |
|---|---|---|
| CEF `libcef_dll_wrapper` nincs bekötve | csak C API-t használunk; a wrapper (C++ osztályok) később kell | `add_subdirectory(${CEF_ROOT}/libcef_dll …, )` a motorház következő lépésében |
| Nincs `cef_sandbox` | a motor nem sandboxolt | a böngészőablakkal együtt |
| CEF beta csatorna | nem kiadásra való | stabil archívum választása a kiadás előtt |
| Nincs `windeployqt` a telepítésben | futtatáshoz kézi Qt-`bin` a PATH-on | telepítő lépésként (`windeployqt`) később |
