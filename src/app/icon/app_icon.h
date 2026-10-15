#pragma once

// Windows application icons: a white sine wave on a rounded badge, in two
// states. The lit version (cyan-to-blue gradient) means the app is active; the
// off version (dark LED with a light rim) means it is inactive, so the tray
// shows the state at a glance.
//
// The artwork lives in assets/white_sound.ico and assets/white_sound_off.ico,
// compiled into the executable by assets/white_sound.rc, so the icons are always
// loaded from the running module. Callers ask for the size they need (tray icons
// want the small icon metric, window icons the large one) and get the generic
// Windows icon back if the resource is unavailable, e.g. in a build without the
// .rc.

#ifdef _WIN32

#include <windows.h>

namespace ws {
namespace icon {

// Must match IDI_APP_ICON / IDI_APP_ICON_OFF in assets/white_sound.rc.
constexpr int kId    = 101;
constexpr int kIdOff = 102;

// Loads the icon with the given resource id at the requested pixel size, falling
// back to the lit icon and then to the generic Windows icon. Never returns null
// on a normal desktop session.
inline HICON loadId(int id, int cx, int cy) {
    HMODULE mod = GetModuleHandleW(nullptr);
    HICON h = static_cast<HICON>(LoadImageW(mod, MAKEINTRESOURCEW(id),
                                           IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR));
    if (!h && id != kId) {
        h = static_cast<HICON>(LoadImageW(mod, MAKEINTRESOURCEW(kId),
                                          IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR));
    }
    // IDI_APPLICATION is the ANSI variant unless UNICODE is defined for this
    // target; cast so the wide call stays valid either way.
    if (!h) h = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
    return h;
}

// Loads the lit icon at the requested pixel size.
inline HICON load(int cx, int cy) {
    return loadId(kId, cx, cy);
}

// Convenience overloads for the two sizes Windows asks for.
inline HICON loadSmall() {
    return load(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
}

inline HICON loadLarge() {
    return load(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
}

// The inactive ("off lamp") variants, used by the tray icon while disabled.
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
