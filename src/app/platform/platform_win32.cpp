#include "platform.h"

#include <windows.h>

#include <string>

namespace ws {
namespace platform {
namespace {

HANDLE g_instanceMutex = nullptr;

std::wstring utf8ToWide(const std::string &s) {
    if (s.empty()) return std::wstring();
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    return out;
}

} // namespace

bool acquireSingleInstance() {
    g_instanceMutex = CreateMutexW(nullptr, TRUE, L"WhiteSoundTray_SingleInstance");
    if (g_instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_instanceMutex);
        g_instanceMutex = nullptr;
        return false;
    }
    return true;
}

void initProcessDpi() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;
    typedef BOOL(WINAPI * SetProcessDpiAwarenessContextFn)(DPI_AWARENESS_CONTEXT);
    auto fn = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
        reinterpret_cast<void *>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
    if (fn) fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

void showMessage(const std::string &title, const std::string &body, bool error) {
    const std::wstring wtitle = utf8ToWide(title);
    const std::wstring wmsg = utf8ToWide(body);
    MessageBoxW(nullptr, wmsg.c_str(), wtitle.c_str(),
                MB_OK | MB_SETFOREGROUND | MB_TOPMOST |
                    (error ? MB_ICONERROR : MB_ICONINFORMATION));
}

} // namespace platform
} // namespace ws
