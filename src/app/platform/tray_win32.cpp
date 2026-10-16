#include "tray.h"

#include <windows.h>
#include <shellapi.h>

#include <string>

#include "app/icon/app_icon.h"

namespace ws {
namespace platform {
namespace {

constexpr UINT WM_TRAYICON = WM_USER + 1;
constexpr UINT WM_ANCHOR_ACTIVATE = WM_APP + 1;
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

        m_hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName,
                                 L"White Sound", WS_POPUP, 0, 0, 1, 1, nullptr,
                                 nullptr, wc.hInstance, this);
        if (!m_hwnd) return false;

        m_iconOn = ws::icon::loadSmall();
        m_iconOff = ws::icon::loadSmallOff();

        SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(wc.hIcon));
        SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(m_iconOn));

        m_anchor = true;
        ShowWindow(m_hwnd, SW_SHOWMINNOACTIVE);

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

        m_hotkeyOk = RegisterHotKey(m_hwnd, kHotkeyId,
                                    MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,
                                    'W') != FALSE;
        return true;
    }

    void destroy() override {
        if (m_hwnd) {
            m_anchor = false;
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
            m_nid.hIcon = enabled ? m_iconOn : m_iconOff;
            m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            wcscpy_s(m_nid.szTip, enabled ? L"White Sound - Active"
                                          : L"White Sound - Inactive");
            Shell_NotifyIconW(NIM_MODIFY, &m_nid);
        }
    }

    void setTaskbarVisible(bool visible) override {
        if (!m_hwnd || m_taskbarVisible == visible) return;
        m_taskbarVisible = visible;

        LONG_PTR ex = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
        if (visible) {
            ex |= WS_EX_APPWINDOW;
            ex &= ~WS_EX_TOOLWINDOW;
        } else {
            ex |= WS_EX_TOOLWINDOW;
            ex &= ~WS_EX_APPWINDOW;
        }
        SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, ex);

        m_suppressAnchor = true;
        ShowWindow(m_hwnd, SW_HIDE);
        SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                         SWP_FRAMECHANGED);
        ShowWindow(m_hwnd, SW_SHOWMINNOACTIVE);
        m_minimized = true;
        m_suppressAnchor = false;
    }

    void showMenu() override {
        if (!m_hwnd || !m_menu) return;
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(m_hwnd);
        TrackPopupMenu(m_menu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0,
                       m_hwnd, nullptr);
        PostMessage(m_hwnd, WM_NULL, 0, 0);
        ShowWindow(m_hwnd, SW_SHOWMINNOACTIVE);
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

    void anchorClick() {
        if (m_suppressAnchor) return;
        const DWORD now = GetTickCount();
        if (now - m_lastAnchorClick <= 350) return;
        m_lastAnchorClick = now;
        activate();
        if (m_hwnd) ShowWindow(m_hwnd, SW_SHOWMINNOACTIVE);
    }

    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_HOTKEY:
                if (wParam == kHotkeyId && m_cb.onHotkey) m_cb.onHotkey();
                break;

            case WM_TRAYICON:
                if (lParam == WM_LBUTTONUP) {
                    const DWORD now = GetTickCount();
                    if (now - m_lastClick > 350) {
                        m_lastClick = now;
                        activate();
                    }
                } else if (lParam == WM_RBUTTONUP) {
                    showMenu();
                }
                break;

            case WM_SYSCOMMAND:
                switch (wParam & 0xFFF0) {
                    case SC_RESTORE:
                    case SC_MAXIMIZE:
                        anchorClick();
                        return 0;
                }
                break;

            case WM_SIZE:
                if (m_anchor && !m_suppressAnchor) {
                    if (wParam == SIZE_MINIMIZED) {
                        m_minimized = true;
                    } else if (m_minimized) {
                        m_minimized = false;
                        PostMessageW(hwnd, WM_ANCHOR_ACTIVATE, 0, 0);
                    }
                }
                break;

            case WM_ANCHOR_ACTIVATE:
                anchorClick();
                return 0;

            case WM_CLOSE:
                if (m_cb.onClose) {
                    m_cb.onClose();
                } else {
                    command(TrayCommand::Quit);
                }
                return 0;

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
    DWORD m_lastAnchorClick = 0;
    bool m_hotkeyOk = false;
    bool m_anchor = false;
    bool m_minimized = false;
    bool m_taskbarVisible = true;
    bool m_suppressAnchor = false;
};

} // namespace

std::unique_ptr<Tray> createTray() { return std::make_unique<Win32Tray>(); }

} // namespace platform
} // namespace ws
