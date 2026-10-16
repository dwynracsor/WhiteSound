#pragma once


#ifdef _WIN32

#include <windows.h>

namespace ws {
namespace icon {

constexpr int kId    = 101;
constexpr int kIdOff = 102;

inline HICON loadId(int id, int cx, int cy) {
    HMODULE mod = GetModuleHandleW(nullptr);
    HICON h = static_cast<HICON>(LoadImageW(mod, MAKEINTRESOURCEW(id),
                                           IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR));
    if (!h && id != kId) {
        h = static_cast<HICON>(LoadImageW(mod, MAKEINTRESOURCEW(kId),
                                          IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR));
    }
    if (!h) h = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
    return h;
}

inline HICON load(int cx, int cy) {
    return loadId(kId, cx, cy);
}

inline HICON loadSmall() {
    return load(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
}

inline HICON loadLarge() {
    return load(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
}

inline HICON loadOff(int cx, int cy) {
    return loadId(kIdOff, cx, cy);
}

inline HICON loadSmallOff() {
    return loadOff(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
}

inline HICON loadLargeOff() {
    return loadOff(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
}

} // namespace icon
} // namespace ws

#endif // _WIN32
