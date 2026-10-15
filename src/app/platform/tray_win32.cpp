// Windows system tray backend: Shell_NotifyIcon on a hidden window that also
// owns the global toggle hotkey. See tray.h for the contract.

#include "tray.h"

#include <windows.h>
#include <shellapi.h>

#include <string>

#include "app/icon/app_icon.h"

namespace ws {
namespace platform {
namespace {

constexpr UINT WM_TRAYICON = WM_USER + 1;
constexpr int kHotkeyId = 1;
constexpr UINT ID_TRAY_OPEN = 1001;
constexpr UINT ID_TRAY_ENABLE = 1002;
constexpr UINT ID_TRAY_CONFIG = 1003;
constexpr UINT ID_TRAY_ABOUT = 1004;
constexpr UINT ID_TRAY_EXIT = 1005;

std::wstring utf8ToWide(const std::string &s) {
    if (s.empty()) return std::wstring();
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    return out;
}

class Win32Tray : public Tray {
public:
    ~Win32Tray() override { destroy(); }

    bool create(const TrayCallbacks &callbacks) override {
        m_cb = callbacks;

        WNDCLASS wc = {};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"WhiteSoundTrayClass";
        wc.hIcon = ws::icon::loadLarge();
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        // A regular (hidden) top-level window, not a message-only one, so the
        // process can be brought to the foreground when the flyout is shown.
        m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName,
                                 L"WhiteSoundTray", WS_POPUP, 0, 0, 0, 0, nullptr,
                                 nullptr, wc.hInstance, this);
        if (!m_hwnd) return false;

        // Icons are loaded once: LoadImageW would otherwise hand out a fresh
        // HICON on every enable/disable and leak it.
        m_iconOn = ws::icon::loadSmall();
        m_iconOff = ws::icon::loadSmallOff();

        m_nid.cbSize = sizeof(NOTIFYICONDATAW);
        m_nid.hWnd = m_hwnd;
        m_nid.uID = 1;
        m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        m_nid.uCallbackMessage = WM_TRAYICON;
        m_nid.hIcon = m_iconOn;
        wcscpy_s(m_nid.szTip, L"White Sound - Bluetooth Keep Alive");
        Shell_NotifyIconW(NIM_ADD, &m_nid);

        m_menu = CreatePopupMenu();
        AppendMenuW(m_menu, MF_STRING, ID_TRAY_OPEN, L"&Open panel");
        AppendMenuW(m_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m_menu, MF_STRING | MF_CHECKED, ID_TRAY_ENABLE, L"&Enabled");
        AppendMenuW(m_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m_menu, MF_STRING, ID_TRAY_CONFIG, L"&Configuration...");
        AppendMenuW(m_menu, MF_STRING, ID_TRAY_ABOUT, L"&About...");
        AppendMenuW(m_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m_menu, MF_STRING, ID_TRAY_EXIT, L"E&xit");

        // Registration fails when another application already owns the
        // combination (Ctrl+Alt+W is popular), so record the result and let the
        // shell tell the user instead of silently losing the hotkey.
        m_hotkeyOk = RegisterHotKey(m_hwnd, kHotkeyId,
                                    MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,
                                    'W') != FALSE;
        return true;
    }

    void destroy() override {
        if (m_hwnd) {
            Shell_NotifyIconW(NIM_DELETE, &m_nid);
            UnregisterHotKey(m_hwnd, kHotkeyId);
            m_hotkeyOk = false;
            if (m_menu) {
                DestroyMenu(m_menu);
                m_menu = nullptr;
            }
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
        }
    }

    void setEnabled(bool enabled) override {
        if (m_menu) {
            CheckMenuItem(m_menu, ID_TRAY_ENABLE,
                          enabled ? MF_CHECKED : MF_UNCHECKED);
        }
        if (m_hwnd) {
            // Swap the tray icon as well, so the state shows in the tray without
            // opening the panel (lit badge = active, dark badge = inactive).
            m_nid.hIcon = enabled ? m_iconOn : m_iconOff;
            m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            wcscpy_s(m_nid.szTip, enabled ? L"White Sound - Active"
                                          : L"White Sound - Inactive");
            Shell_NotifyIconW(NIM_MODIFY, &m_nid);
        }
    }

    void showMenu() override {
        if (!m_hwnd || !m_menu) return;
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(m_hwnd);
        TrackPopupMenu(m_menu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0,
                       m_hwnd, nullptr);
        PostMessage(m_hwnd, WM_NULL, 0, 0);
    }

    void notify(const std::string &title, const std::string &body) override {
        if (!m_hwnd) return;
        const std::wstring wtitle = utf8ToWide(title);
        const std::wstring wbody = utf8ToWide(body);
        m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO;
        wcsncpy_s(m_nid.szInfoTitle, wtitle.c_str(), _TRUNCATE);
        wcsncpy_s(m_nid.szInfo, wbody.c_str(), _TRUNCATE);
        m_nid.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &m_nid);
        m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    }

    // The thread message pump (GLFW's pumpEvents) already dispatches our window's
    // messages, so this only matters in the tray-only fallback: it drains the
    // messages addressed to the tray window (clicks, menu commands, hotkey)
    // without touching the flyout window's queue.
    void pump() override {
        if (!m_hwnd) return;
        MSG msg;
        while (PeekMessageW(&msg, m_hwnd, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    bool hotkeyAvailable() const override { return m_hotkeyOk; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                    LPARAM lParam) {
        Win32Tray *self = nullptr;
        if (msg == WM_NCCREATE) {
            auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
            self = static_cast<Win32Tray *>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self) self->m_hwnd = hwnd;
        } else {
            self = reinterpret_cast<Win32Tray *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (self) return self->handleMessage(hwnd, msg, wParam, lParam);
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    void command(TrayCommand c) { if (m_cb.onCommand) m_cb.onCommand(c); }
    void activate() { if (m_cb.onActivate) m_cb.onActivate(); else showMenu(); }

    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_HOTKEY:
                if (wParam == kHotkeyId && m_cb.onHotkey) m_cb.onHotkey();
                break;

            case WM_TRAYICON:
                if (lParam == WM_LBUTTONUP) {
                    // Debounce: a Windows double-click arrives as
                    // LBUTTONUP, LBUTTONDBLCLK, LBUTTONUP; without this the
                    // second UP would immediately re-toggle and hide the panel.
                    const DWORD now = GetTickCount();
                    if (now - m_lastClick > 350) {
                        m_lastClick = now;
                        activate();
                    }
                } else if (lParam == WM_RBUTTONUP) {
                    showMenu();
                }
                break;

            case WM_COMMAND:
                switch (LOWORD(wParam)) {
                    case ID_TRAY_OPEN: command(TrayCommand::OpenPanel); break;
                    case ID_TRAY_ENABLE: command(TrayCommand::ToggleEnabled); break;
                    case ID_TRAY_CONFIG: command(TrayCommand::ShowConfig); break;
                    case ID_TRAY_ABOUT: command(TrayCommand::ShowAbout); break;
                    case ID_TRAY_EXIT: command(TrayCommand::Quit); break;
                }
                break;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    TrayCallbacks m_cb;
    HWND m_hwnd = nullptr;
    HMENU m_menu = nullptr;
    NOTIFYICONDATAW m_nid{};
    HICON m_iconOn = nullptr;
    HICON m_iconOff = nullptr;
    DWORD m_lastClick = 0;
    bool m_hotkeyOk = false;
};

} // namespace

std::unique_ptr<Tray> createTray() { return std::make_unique<Win32Tray>(); }

} // namespace platform
} // namespace ws
