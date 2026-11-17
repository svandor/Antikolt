# SPDX-License-Identifier: MPL-2.0
#
# The Qt shell: window, chrome (toolbar), the live card and the engine link.
#
# The card is a native widget: its window handle goes to the engine host, which embeds Chromium into
# it (ADR-004). That keeps the UI process and the engine process separate while still showing the
# page inside our own window.

qt_add_executable(antikolt-shell
  src/main.cpp
  src/main_window.cpp
  src/main_window.h
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
