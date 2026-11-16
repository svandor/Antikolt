// SPDX-License-Identifier: MPL-2.0
//
// The CEF engine host — milestone 0.
//
// This build deliberately does *not* create a browser yet. Its job is to retire the integration
// risks before any browser code is written:
//
//   1. the CEF headers compile with our warning settings;
//   2. the import library (libcef.lib) links;
//   3. libcef.dll loads at run time from the output directory;
//   4. a CEF C function is callable.
//
// All four are proven by `--selftest`, which is what CI and scripts/build.ps1 run. The first real
// browser window (a native child window with one surface) is the F0 spike described in
// docs/HU/16-FEJLESZTESI-NAPLO.md.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "include/cef_version.h"
#include "include/internal/cef_string.h"
#include "include/internal/cef_types.h"

namespace {

void printVersion() {
    std::printf("cef=%s\n", CEF_VERSION);
    std::printf("chromium=%d.%d.%d.%d\n", CHROME_VERSION_MAJOR, CHROME_VERSION_MINOR,
                CHROME_VERSION_BUILD, CHROME_VERSION_PATCH);
    std::printf("cef_commit=%s\n", CEF_COMMIT_HASH);
    std::printf("copyright_year=%d\n", COPYRIGHT_YEAR);
}

/// Touch a real CEF entry point. If libcef.dll is missing or the ABI is wrong, this fails loudly
/// instead of the failure appearing later as an empty window.
bool cefRuntimeLoads() {
    cef_string_utf16_t string = {};
    const char16_t source[] = u"antikolt";
    const int ok = cef_string_utf16_set(source, 8, &string, 1);
    const bool allocated = ok != 0 && string.str != nullptr && string.length == 8;
    cef_string_utf16_clear(&string);
    const bool cleared = string.str == nullptr && string.length == 0;
    return allocated && cleared;
}

int runSelfTest() {
    std::printf("antikolt-engine-host %s\n", ANTIKOLT_VERSION);
    printVersion();

    if (!cefRuntimeLoads()) {
        std::printf("cef_runtime=FAILED (libcef.dll missing, or the CEF ABI does not match)\n");
        return 1;
    }
    std::printf("cef_runtime=ok\n");

    std::printf("browser=not-implemented\n");
    std::printf("ok\n");
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    enum class Mode { Browser, SelfTest, Version, Help, BadArgument };
    Mode mode = Mode::Browser;
    const char* badArgument = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--selftest") == 0) {
            mode = Mode::SelfTest;
        } else if (std::strcmp(argv[i], "--version") == 0) {
            mode = Mode::Version;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            mode = Mode::Help;
        } else {
            mode = Mode::BadArgument;
            badArgument = argv[i];
            break;
        }
    }

    switch (mode) {
        case Mode::SelfTest:
            return runSelfTest();
        case Mode::Version:
            printVersion();
            return 0;
        case Mode::Help:
            std::printf("usage: antikolt-engine-host [--selftest] [--version]\n"
                        "\n"
                        "The engine host owns the Chromium (CEF) engine in a separate process\n"
                        "(ADR-004). Browser mode is not implemented yet: the F0 spike adds one\n"
                        "surface and the memory measurement.\n");
            return 0;
        case Mode::BadArgument:
            std::fprintf(stderr, "unknown argument: %s\n", badArgument);
            return 2;
        case Mode::Browser:
            break;
    }

    std::fprintf(stderr,
                 "browser mode is not implemented yet.\n"
                 "This milestone only proves the CEF integration: run with --selftest.\n");
    return 3;
}
