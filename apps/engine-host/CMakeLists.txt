# SPDX-License-Identifier: MPL-2.0
#
# The engine host: a separate process that owns the Chromium (CEF) engine.
#
# Why a separate executable (ADR-004):
#   * the shell must stay usable when the engine is shut down (ultra-low mode, ~120-180 MB);
#   * a crash in the engine must not take the browser down;
#   * a second engine (WebKitGTK, WebView2) becomes another executable behind the same protocol.
#
# Today this target proves the CEF integration: headers compile, the import library links, libcef.dll
# loads and its C functions are callable. The browser window comes with the F0 spike.

if(NOT ANTIKOLT_CEF_ROOT)
  message(FATAL_ERROR "apps/engine-host needs ANTIKOLT_CEF_ROOT")
endif()

set(CEF_INCLUDE "${ANTIKOLT_CEF_ROOT}/include")
set(CEF_RELEASE "${ANTIKOLT_CEF_ROOT}/Release")
set(CEF_RESOURCES "${ANTIKOLT_CEF_ROOT}/Resources")

if(NOT EXISTS "${CEF_RELEASE}/libcef.lib")
  message(FATAL_ERROR "libcef.lib not found under ${CEF_RELEASE}")
endif()

add_executable(antikolt-engine-host src/main.cpp)

# CEF's own convention: the CEF root goes on the include path and sources say "include/cef_app.h".
target_include_directories(antikolt-engine-host PRIVATE "${ANTIKOLT_CEF_ROOT}")
target_compile_definitions(antikolt-engine-host PRIVATE
  ANTIKOLT_VERSION="${PROJECT_VERSION}"
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  UNICODE
  _UNICODE
)

target_link_directories(antikolt-engine-host PRIVATE "${CEF_RELEASE}")
target_link_libraries(antikolt-engine-host PRIVATE libcef.lib)

if(MSVC)
  target_compile_options(antikolt-engine-host PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
endif()

# Copy the CEF runtime next to the executable. CEF refuses to start without these files, and a
# developer should not have to discover that from a blank window.
set(CEF_RUNTIME_FILES)
foreach(file libcef.dll chrome_elf.dll d3dcompiler_47.dll dxcompiler.dll
             v8_context_snapshot.bin vk_swiftshader.dll vk_swiftshader_icd.json vulkan-1.dll)
  if(EXISTS "${CEF_RELEASE}/${file}")
    list(APPEND CEF_RUNTIME_FILES "${CEF_RELEASE}/${file}")
  endif()
endforeach()
foreach(file icudtl.dat resources.pak chrome_100_percent.pak chrome_200_percent.pak)
  if(EXISTS "${CEF_RESOURCES}/${file}")
    list(APPEND CEF_RUNTIME_FILES "${CEF_RESOURCES}/${file}")
  endif()
endforeach()

add_custom_command(TARGET antikolt-engine-host POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CEF_RUNTIME_FILES} "$<TARGET_FILE_DIR:antikolt-engine-host>"
  COMMENT "Copying the CEF runtime next to antikolt-engine-host"
  VERBATIM
)

if(EXISTS "${CEF_RESOURCES}/locales")
  add_custom_command(TARGET antikolt-engine-host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CEF_RESOURCES}/locales" "$<TARGET_FILE_DIR:antikolt-engine-host>/locales"
    COMMENT "Copying CEF locales"
    VERBATIM
  )
endif()

install(TARGETS antikolt-engine-host RUNTIME DESTINATION bin)
