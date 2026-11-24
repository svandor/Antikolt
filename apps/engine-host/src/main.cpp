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
#include <fstream>
#include <string>

#include "host.h"
#include "include/cef_version.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

constexpr const char* kSelftestUrl =
    "data:text/html,%3Ctitle%3EAntikolt%20selftest%3C/title%3E%3Ch1%3Eok%3C/h1%3E";

/// A login form for the autofill spike: a username input followed by a password input. The fill runs
/// in the isolated world; the verification reads the form from the main world.
constexpr const char* kAutofillTestUrl =
    "data:text/html,%3Ctitle%3Eautofill%3C/title%3E"
    "%3Cinput%20name=username%20autocomplete=username%3E"
    "%3Cinput%20type=password%20name=password%3E";

/// The passkey flow runs on a `antikolt://blob/…` page: the scheme is registered SECURE (host.cpp), so
/// `navigator.credentials` is available. A minimal HTML file is written to the preview directory first.
constexpr const char* kPasskeyTestName = "passkey-test.html";

void WritePasskeyTestPage(const std::string& previews_dir) {
    std::error_code error;
    std::filesystem::create_directories(previews_dir, error);
    if (error) {
        return;
    }
    std::ofstream stream(previews_dir + "\\" + kPasskeyTestName, std::ios::binary);
    stream << "<!doctype html><title>passkey</title><h1>passkey</h1>";
}

/// The Document-PiP probe runs on a `antikolt://blob/…` page: the scheme is registered SECURE
/// (host.cpp, ADR-093), so `window.isSecureContext` is true there — unlike a `data:` URL, which is an
/// opaque (non-secure) origin and can never expose a secure-context-gated API.
constexpr const char* kPipTestName = "pip-test.html";

void WritePipTestPage(const std::string& previews_dir) {
    std::error_code error;
    std::filesystem::create_directories(previews_dir, error);
    if (error) {
        return;
    }
    std::ofstream stream(previews_dir + "\\" + kPipTestName, std::ios::binary);
    stream << "<!doctype html><title>pip</title><h1>pip</h1>";
}

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

#ifdef _WIN32
/// Declare this process per-monitor DPI aware, before any window exists.
///
/// Windows *virtualises* the coordinates of a DPI-unaware process: at 175 % scaling, every coordinate
/// it sets or reads is quietly multiplied behind its back. The shell tells the engine the card's size
/// in physical pixels and the engine must honour exactly that, so awareness is not cosmetic — without
/// it, a 900×640 card asked the page for a 1575 px viewport and Chromium clamped the height to the
/// screen. Per-monitor v2 is what modern Windows offers; v1 is the Windows 8.1 fallback.
void EnablePerMonitorDpiAwareness() {
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        using SetContextFn = BOOL(WINAPI*)(HANDLE);
        auto set_context =
            reinterpret_cast<SetContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 is -4; spelled as an integer so the build does
        // not depend on how new the SDK's headers are.
        if (set_context != nullptr &&
            set_context(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4))) != FALSE) {
            return;
        }
    }
    const HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore != nullptr) {
        using SetAwarenessFn = HRESULT(WINAPI*)(int);
        auto set_awareness =
            reinterpret_cast<SetAwarenessFn>(GetProcAddress(shcore, "SetProcessDpiAwareness"));
        constexpr int kProcessPerMonitorDpiAware = 2;
        if (set_awareness != nullptr && SUCCEEDED(set_awareness(kProcessPerMonitorDpiAware))) {
            return;
        }
    }
    SetProcessDPIAware();
}
#endif

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
        "  --stdio                 run as the shell's protocol server on stdin/stdout\n"
        "  --selftest              start CEF, load a data: URL, report and exit (CI uses this)\n"
        "  --partitiontest         prove the per-partition contexts, proxy and permission policy (ADR-071)\n"
        "  --url <url>             open this page in a window (one-shot mode)\n"
        "  --size <WxH>            window client size (default 1280x800)\n"
        "  --hidden                create the window without showing it\n"
        "  --user-data-dir <path>  CEF cache/profile directory\n"
        "  --previews-dir <path>   where antikolt://blob/ serves from (default: <user-data-dir>/previews)\n"
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
    // Before anything can create a window, and before CEF's helper processes inherit the setting.
    EnablePerMonitorDpiAwareness();

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
    bool partition_test = false;
    bool webauthn_test = false;
    bool isolated_world_test = false;
    bool autofill_test = false;
    bool passkey_flow_test = false;
    bool pip_test = false;
    bool pip_window_test = false;
    bool views_test = false;
    bool stdio_mode = false;
    bool user_data_explicit = false;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--selftest") == 0) {
            selftest = true;
        } else if (std::strcmp(arg, "--partitiontest") == 0) {
            partition_test = true;
        } else if (std::strcmp(arg, "--webauthntest") == 0) {
            webauthn_test = true;
        } else if (std::strcmp(arg, "--isolatedworldtest") == 0) {
            isolated_world_test = true;
        } else if (std::strcmp(arg, "--autofilltest") == 0) {
            autofill_test = true;
        } else if (std::strcmp(arg, "--passkeyflowtest") == 0) {
            passkey_flow_test = true;
        } else if (std::strcmp(arg, "--pipchecktest") == 0) {
            pip_test = true;
        } else if (std::strcmp(arg, "--pipwindowtest") == 0) {
            pip_window_test = true;
        } else if (std::strcmp(arg, "--viewstest") == 0) {
            views_test = true;
        } else if (std::strcmp(arg, "--stdio") == 0) {
            stdio_mode = true;
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
        } else if (std::strcmp(arg, "--previews-dir") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--previews-dir needs a value\n");
                return 2;
            }
            config.previews_path = argv[++i];
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg);
            printUsage();
            return 2;
        }
    }

    if (selftest || partition_test || webauthn_test || isolated_world_test || autofill_test ||
        passkey_flow_test || pip_test || pip_window_test || views_test) {
        config.hidden = true;
        config.quit_on_first_load = true;
        config.start_url = passkey_flow_test ? "antikolt://blob/passkey-test.html"
                                             : ((pip_test || pip_window_test) ? "antikolt://blob/pip-test.html"
                                                          : (autofill_test ? kAutofillTestUrl
                                                                           : kSelftestUrl));
        config.webauthn_test = webauthn_test;
        config.isolated_world_test = isolated_world_test;
        config.autofill_test = autofill_test;
        config.passkey_flow_test = passkey_flow_test;
        config.pip_test = pip_test;
        config.pip_window_test = pip_window_test;
        if (!user_data_explicit) {
            // A selftest must be hermetic: its own cache directory, removed afterwards. Otherwise a
            // leftover instance or a previous run fights over Chromium's cache locks and the result
            // depends on history instead of on the code.
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            const char* prefix = partition_test
                                     ? "antikolt-partition-"
                                     : (webauthn_test
                                            ? "antikolt-webauthn-"
                                            : (isolated_world_test
                                                   ? "antikolt-isolatedworld-"
                                                   : (autofill_test
                                                          ? "antikolt-autofill-"
                                                          : (passkey_flow_test
                                                                 ? "antikolt-passkey-"
                                                                 : (pip_test ? "antikolt-pip-"
                                                                             : "antikolt-selftest-")))));
            config.user_data_path =
                (std::filesystem::temp_directory_path() /
                 (prefix + std::to_string(static_cast<long long>(stamp))))
                    .string();
        }
        printVersion();
        std::printf("selftest=1\n");
    }

    // The shell hands the profile path over with forward slashes (`QStandardPaths`), but CEF's Chrome runtime stores
    // its profile root as a canonical backslash path and compares each request context's cache path against it with an
    // exact string match (`cache_path_.DirName() == user_data_dir` in `chrome_browser_context.cc`). A forward-slash
    // child then fails that check — "Cannot create profile at path" — and silently falls back to in-memory storage, so
    // the profile is normalised to backslashes here, once, before anything derives a child path from it.
#ifdef _WIN32
    for (char& character : config.user_data_path) {
        if (character == '/') {
            character = '\\';
        }
    }
    for (char& character : config.previews_path) {
        if (character == '/') {
            character = '\\';
        }
    }
#endif

    // The passkey and PiP pages live in the preview directory, served over the (secure) `antikolt://` scheme.
    if (passkey_flow_test) {
        WritePasskeyTestPage(config.user_data_path + "\\previews");
    }
    if (pip_test || pip_window_test) {
        WritePipTestPage(config.user_data_path + "\\previews");
    }

    std::error_code error;
    std::filesystem::create_directories(config.user_data_path, error);
    if (error) {
        std::fprintf(stderr, "cannot create the user data directory %s: %s\n",
                     config.user_data_path.c_str(), error.message().c_str());
        return 1;
    }
    if (selftest || webauthn_test || isolated_world_test || autofill_test || passkey_flow_test ||
        pip_test || pip_window_test || views_test) {
        std::printf("user_data=%s\n", config.user_data_path.c_str());
        std::fflush(stdout);
    }
    if (partition_test) {
        return antikolt::RunPartitionSelfTest(config);
    }
    if (views_test) {
        return antikolt::RunViewsTest(config);
    }

    const int exit_code =
        stdio_mode ? antikolt::RunEngineHostServer(config) : antikolt::RunEngineHost(config);

    if ((selftest || webauthn_test || isolated_world_test || autofill_test || passkey_flow_test ||
         pip_test || pip_window_test || views_test) &&
        !user_data_explicit) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(config.user_data_path, cleanup_error);
    }
    return exit_code;
}