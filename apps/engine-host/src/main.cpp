// SPDX-License-Identifier: MPL-2.0
//
// Engine host entry point.
//
//   antikolt-engine-host --selftest      prove the engine starts and loads a page, then exit
//   antikolt-engine-host --url <url>     open one surface in a real window
//   antikolt-engine-host --version       print the CEF/Chromium versions
//
// The selftest is deliberately network-free: it loads a `data:` URL, so a failure is always our
// failure and never the network's.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "host.h"
#include "include/cef_version.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

constexpr const char* kSelftestUrl =
    "data:text/html,%3Ctitle%3EAntikolt%20selftest%3C/title%3E%3Ch1%3Eok%3C/h1%3E";

std::string Utf8FromWide(const std::wstring& wide) {
#ifdef _WIN32
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
#else
    return std::string(wide.begin(), wide.end());
#endif
}

std::string ExecutablePath() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH * 4] = {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return Utf8FromWide(std::wstring(buffer, length));
#else
    return {};
#endif
}

std::string DefaultUserDataPath() {
    const char* local_app_data = std::getenv("LOCALAPPDATA");
    if (local_app_data != nullptr && *local_app_data != '\0') {
        return std::string(local_app_data) + "\\Antikolt\\engine";
    }
    return (std::filesystem::temp_directory_path() / "antikolt-engine").string();
}

void printVersion() {
    std::printf("antikolt-engine-host %s\n", ANTIKOLT_VERSION);
    std::printf("cef=%s\n", CEF_VERSION);
    std::printf("chromium=%d.%d.%d.%d\n", CHROME_VERSION_MAJOR, CHROME_VERSION_MINOR,
                CHROME_VERSION_BUILD, CHROME_VERSION_PATCH);
    std::printf("cef_commit=%s\n", CEF_COMMIT_HASH);
}

void printUsage() {
    std::printf(
        "usage: antikolt-engine-host [options]\n"
        "\n"
        "  --selftest              start CEF, load a data: URL, report and exit (CI uses this)\n"
        "  --url <url>             open this page in a window\n"
        "  --size <WxH>            window client size (default 1280x800)\n"
        "  --hidden                create the window without showing it\n"
        "  --user-data-dir <path>  CEF cache/profile directory\n"
        "  --version               print CEF and Chromium versions\n"
        "  --help                  this text\n");
}

/// Parse "1280x800". Returns false when the format is wrong.
bool parseSize(const char* text, int* width, int* height) {
    return std::sscanf(text, "%dx%d", width, height) == 2 && *width > 0 && *height > 0;
}

}  // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // No crash dialog, no "file not found" box: a failed run must exit with a code that CI can read,
    // not block on a modal window on somebody's desktop.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif

    // CEF helper processes first: they carry switches like `--type=gpu-process` that are not ours,
    // and they must reach CefExecuteProcess before any parsing happens. Getting this wrong kills the
    // GPU/utility processes and CEF then aborts with "GPU process isn't usable".
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--type=", 7) == 0) {
            return antikolt::RunCefSubprocess();
        }
    }

    antikolt::HostConfig config;
    config.exe_path = ExecutablePath();
    config.user_data_path = DefaultUserDataPath();

    bool selftest = false;
    bool user_data_explicit = false;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--selftest") == 0) {
            selftest = true;
        } else if (std::strcmp(arg, "--version") == 0) {
            printVersion();
            return 0;
        } else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            printUsage();
            return 0;
        } else if (std::strcmp(arg, "--hidden") == 0) {
            config.hidden = true;
        } else if (std::strcmp(arg, "--url") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--url needs a value\n");
                return 2;
            }
            config.start_url = argv[++i];
        } else if (std::strcmp(arg, "--size") == 0) {
            if (i + 1 >= argc || !parseSize(argv[++i], &config.width, &config.height)) {
                std::fprintf(stderr, "--size expects WxH, for example 1280x800\n");
                return 2;
            }
        } else if (std::strcmp(arg, "--user-data-dir") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--user-data-dir needs a value\n");
                return 2;
            }
            config.user_data_path = argv[++i];
            user_data_explicit = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg);
            printUsage();
            return 2;
        }
    }

    if (selftest) {
        config.hidden = true;
        config.quit_on_first_load = true;
        config.start_url = kSelftestUrl;
        if (!user_data_explicit) {
            // A selftest must be hermetic: its own cache directory, removed afterwards. Otherwise a
            // leftover instance or a previous run fights over Chromium's cache locks and the result
            // depends on history instead of on the code.
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            config.user_data_path =
                (std::filesystem::temp_directory_path() /
                 ("antikolt-selftest-" + std::to_string(static_cast<long long>(stamp))))
                    .string();
        }
        printVersion();
        std::printf("selftest=1\n");
    }

    std::error_code error;
    std::filesystem::create_directories(config.user_data_path, error);
    if (error) {
        std::fprintf(stderr, "cannot create the user data directory %s: %s\n",
                     config.user_data_path.c_str(), error.message().c_str());
        return 1;
    }
    if (selftest) {
        std::printf("user_data=%s\n", config.user_data_path.c_str());
        std::fflush(stdout);
    }

    const int exit_code = antikolt::RunEngineHost(config);

    if (selftest && !user_data_explicit) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(config.user_data_path, cleanup_error);
    }
    return exit_code;
}
