#include "paths.h"

#include <cstdlib>
#include <string>

#if defined(_WIN32)
#include <stdlib.h>   // _wdupenv_s
#endif

namespace ws {
namespace platform {
namespace {

#if defined(_WIN32)
// Windows: read a wide environment variable so non-ASCII paths survive.
std::filesystem::path env(const wchar_t *name) {
    wchar_t *value = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&value, &len, name) == 0 && value) {
        std::filesystem::path p(value);
        std::free(value);
        return p;
    }
    return {};
}
#else
std::filesystem::path env(const char *name) {
    const char *value = std::getenv(name);
    return (value && *value) ? std::filesystem::path(value)
                             : std::filesystem::path();
}
#endif

std::filesystem::path homeDir() {
#if defined(_WIN32)
    std::filesystem::path p = env(L"USERPROFILE");
    if (!p.empty()) return p;
    std::filesystem::path drive = env(L"HOMEDRIVE");
    std::filesystem::path rest  = env(L"HOMEPATH");
    if (!drive.empty() && !rest.empty()) {
        return std::filesystem::path(drive.wstring() + rest.wstring());
    }
    return {};
#else
    return env("HOME");
#endif
}

} // namespace

std::filesystem::path configDir() {
#if defined(_WIN32)
    std::filesystem::path base = env(L"APPDATA");
    if (base.empty()) base = homeDir();
#elif defined(__APPLE__)
    std::filesystem::path base = homeDir();
    if (!base.empty()) return base / "Library" / "Application Support" / "white-sound";
#else
    std::filesystem::path xdg = env("XDG_CONFIG_HOME");
    if (!xdg.empty()) return xdg / "white-sound";
    std::filesystem::path base = homeDir();
    if (!base.empty()) return base / ".config" / "white-sound";
#endif

    if (!base.empty()) return base / "white-sound";
    return ".";
}

std::filesystem::path configFile() { return configDir() / "config.json"; }
std::filesystem::path logFile()    { return configDir() / "ws.log"; }

} // namespace platform
} // namespace ws
