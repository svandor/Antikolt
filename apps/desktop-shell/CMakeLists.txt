# SPDX-License-Identifier: MPL-2.0
#
# The Qt shell: window, chrome (toolbar), the live card and the engine link.
#
# The card is a native widget: its window handle goes to the engine host, which embeds Chromium into
# it (ADR-004). That keeps the UI process and the engine process separate while still showing the
# page inside our own window.

# A console-subsystem binary on purpose: every self test reads its output and its exit code from a shell, and
# that is what PowerShell gives a console program and does *not* give a GUI one (measured: with WIN32_EXECUTABLE
# `& $shell --test` returned at once, with no output and no exit code, which would have quietly disarmed the
# whole gate). The console *window* that Windows creates for a shortcut launch is dropped at startup instead —
# see `DropOwnConsoleWindow` in `main.cpp`: no extra window, and the harness keeps working.
qt_add_executable(antikolt-shell
  src/main.cpp
  src/main_window.cpp
  src/main_window.h
  src/instance_guard.cpp
  src/instance_guard.h
  src/live_card.cpp
  src/live_card.h
  src/engine_client.cpp
  src/engine_client.h
  src/core_client.cpp
  src/core_client.h
)

target_include_directories(antikolt-shell PRIVATE src)

target_link_libraries(antikolt-shell PRIVATE
  Qt6::Core
  Qt6::Gui
  Qt6::Widgets
  # Qt Network is used by the self tests only (a local HTTP origin for the session wipe check), but the
  # shell links it itself so a test never needs a second executable or an internet connection.
  Qt6::Network
)

target_compile_definitions(antikolt-shell PRIVATE
  ANTIKOLT_VERSION="${PROJECT_VERSION}"
)

if(Qt6Quick_FOUND)
  target_link_libraries(antikolt-shell PRIVATE Qt6::Quick Qt6::Qml)
  target_compile_definitions(antikolt-shell PRIVATE ANTIKOLT_HAVE_QTQUICK=1)
endif()

if(MSVC)
  target_compile_options(antikolt-shell PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
  # /utf-8 matters: the source contains Hungarian text in UTF-8.
endif()

# Keep the executable next to its Qt DLLs on Windows; windeployqt runs at install time, not here.
install(TARGETS antikolt-shell RUNTIME DESTINATION bin)
