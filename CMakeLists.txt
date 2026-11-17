# SPDX-License-Identifier: MPL-2.0
#
# Antikolt build root.
#
# The shell (Qt 6) builds on its own; the engine host (CEF) is added when ANTIKOLT_CEF_ROOT points at
# an extracted CEF binary distribution. That split is deliberate (ADR-004): the shell must be able to
# build on a machine without the 1-2 GB CEF download, and the engine host is a separate process.
#
#   cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DANTIKOLT_CEF_ROOT=D:/Tools/cef/<build>
#   cmake --build build --config RelWithDebInfo
#   build/apps/desktop-shell/RelWithDebInfo/antikolt-shell.exe --selftest

cmake_minimum_required(VERSION 3.21)

project(
  Antikolt
  VERSION 0.1.0
  DESCRIPTION "Low-memory, card-based browser"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Qt is the shell (ADR-002). Qt Quick is required for the real UI; the first milestone only needs
# Core/Gui/Widgets, so Quick is optional until the QML layer lands.
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network)
find_package(Qt6 6.5 QUIET COMPONENTS Quick Qml)

message(STATUS "Qt ${Qt6_VERSION} found at ${Qt6_DIR}")

# Where an extracted CEF binary distribution lives. Empty means "no engine host this time".
set(ANTIKOLT_CEF_ROOT
    "$ENV{CEF_ROOT}"
    CACHE PATH "Extracted CEF binary distribution (contains cmake/cef_variables.cmake)"
)

add_subdirectory(apps/desktop-shell)

set(ANTIKOLT_ENGINE_HOST_AVAILABLE OFF)
if(ANTIKOLT_CEF_ROOT AND EXISTS "${ANTIKOLT_CEF_ROOT}/cmake/cef_variables.cmake")
  set(ANTIKOLT_ENGINE_HOST_AVAILABLE ON)
  message(STATUS "CEF found at ${ANTIKOLT_CEF_ROOT} — the engine host will be built")
  add_subdirectory(apps/engine-host)
else()
  message(
    STATUS
      "No CEF at ANTIKOLT_CEF_ROOT (got '${ANTIKOLT_CEF_ROOT}') — building the shell only.\n"
      "        Install it with: pwsh -File scripts/install-cef.ps1"
  )
endif()

# A single summary, so a build log answers "what did I actually build?".
message(STATUS "")
message(STATUS "Antikolt ${PROJECT_VERSION} configuration")
message(STATUS "  build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler     : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Qt           : ${Qt6_VERSION}")
message(STATUS "  Qt Quick     : ${Qt6Quick_FOUND}")
message(STATUS "  engine host  : ${ANTIKOLT_ENGINE_HOST_AVAILABLE}")
message(STATUS "")
