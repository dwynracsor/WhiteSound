// Native fallback flyout host: Win32 + DirectX 11 + ImGui.
//
// The default backend is the portable GLFW + OpenGL3 host (gui_glfw.cpp); this
// one is selected with -DWS_FLYOUT_BACKEND=win32 and reproduces the original
// Windows behaviour exactly (DWM rounding, HTCAPTION dragging, WM_ACTIVATE
// close). It implements the same ws::gui::Flyout interface.

#include "flyout.h"

#ifdef _WIN32

#include "gui_ui.h"
#include "app/icon/app_icon.h"

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx11.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace ws {
namespace gui {

namespace {
// Sized to the panel layout (gui_ui.h): the same layout fills the window, so a
// shorter window would clip the bottom (burst mode + the low-level warning is
// the worst case, ~456 logical px tall).
constexpr int kBaseW = kFlyoutW;
constexpr int kBaseH = kFlyoutH;
constexpr int kCornerRadius = 10;   // logical px, scaled by DPI

// DWMWINDOWATTRIBUTE / DWM_WINDOW_CORNER_PREFERENCE values, spelled out so the
// build does not depend on a recent SDK.
constexpr DWORD kDwmwaWindowCornerPreference = 33;
constexpr DWORD kDwmwaBorderColor = 34;
constexpr int   kDwmscpRound = 2;          // DWM_WINDOW_CORNER_PREFERENCE
constexpr DWORD kDwmColorNone = 0xFFFFFFFE; // DWMWA_COLOR_NONE

typedef HRESULT(WINAPI *DwmSetWindowAttributeFn)(HWND, DWORD, LPCVOID, DWORD);

// Bring a window to the foreground even when our process is in the background
// (a system tray app usually is). Windows' foreground lock otherwise silently
// ignores SetForegroundWindow for a non-foreground process.
void forceForeground(HWND hwnd) {
    if (GetForegroundWindow() == hwnd) return;
    DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD thisThread = GetCurrentThreadId();
    if (fgThread != 0 && fgThread != thisThread) {
        AttachThreadInput(fgThread, thisThread, TRUE);
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        AttachThreadInput(fgThread, thisThread, FALSE);
    } else {
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
    }
}

// Keep a window fully inside the work area of the given monitor.
void clampToMonitor(int &x, int &y, int w, int h, HMONITOR mon) {
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;
    if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
    if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;
}
} // namespace

class Win32Flyout : public Flyout {
public:
    Win32Flyout() : m_d3d(new D3D()) {}
    ~Win32Flyout() override {
        destroy();
        delete m_d3d;
        m_d3d = nullptr;
    }

    bool create(GuiModel *model) override {
        m_hInstance = GetModuleHandleW(nullptr);
        m_model = model;

        std::swprintf(m_className, 64, L"WhiteSoundFlyout_%p", static_cast<void *>(m_hInstance));

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = m_hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = m_className;
        if (!RegisterClassExW(&wc)) return false;

        m_hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            m_className, L"White Sound",
            WS_POPUP,
            0, 0, kBaseW, kBaseH,
            nullptr, nullptr, m_hInstance, this);
        if (!m_hwnd) return false;

        // Same artwork as the tray icon. The flyout is a tool window, so this only
        // shows up in shell dialogs, but keep it consistent.
        SendMessageW(m_hwnd, WM_SETICON, ICON_BIG,
                     reinterpret_cast<LPARAM>(ws::icon::loadLarge()));
        SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL,
                     reinterpret_cast<LPARAM>(ws::icon::loadSmall()));

        updateDpiScale(m_hwnd);
        applyRoundedCorners();
        resizeToContent();

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        applyDarkStyle();
        ImGui::GetStyle().ScaleAllSizes(m_scale);
        io.FontGlobalScale = m_scale;

        ImGui_ImplWin32_Init(m_hwnd);
        if (!createDeviceObjects()) {
            return false;
        }
        m_ready = true;
        return true;
    }

    void destroy() override {
        // The shell destroys the flyout explicitly and then drops it, so this
        // runs a second time from ~Win32Flyout(). ImGui's DX11 backend frees its
        // state on shutdown, so a second pass would dereference freed memory.
        if (m_destroyed) return;
        m_destroyed = true;

        if (m_ready) {
            ImGui_ImplWin32_Shutdown();
            m_ready = false;
        }
        cleanupDeviceObjects();
        if (ImGui::GetCurrentContext()) {
            ImGui::DestroyContext();
        }
        if (m_hwnd) {
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
        }
        if (m_className[0]) {
            UnregisterClassW(m_className, m_hInstance);
            m_className[0] = L'\0';
        }
    }

    void showNearCursor() override {
        if (!m_hwnd) return;

        POINT pt;
        GetCursorPos(&pt);

        RECT wr;
        GetWindowRect(m_hwnd, &wr);
        int w = wr.right - wr.left;
        int h = wr.bottom - wr.top;

        int x = 0;
        int y = 0;
        if (m_haveUserPos) {
            // The user dragged the panel somewhere: reopen where it was left.
            x = m_userX;
            y = m_userY;
            POINT anchor = {x, y};
            clampToMonitor(x, y, w, h, MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST));
        } else {
            HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = {};
            mi.cbSize = sizeof(mi);
            GetMonitorInfoW(mon, &mi);

            x = pt.x - w / 2;
            y = pt.y - h - 12;
            if (x < mi.rcWork.left) x = mi.rcWork.left;
            if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
            if (y < mi.rcWork.top) y = pt.y + 20;
            if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
        }

        m_gotActive = false;

        SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW | SWP_NOACTIVATE);
        ShowWindow(m_hwnd, SW_SHOW);
        forceForeground(m_hwnd);
        m_visible = true;
    }

    void hide() override {
        if (m_hwnd) {
            ShowWindow(m_hwnd, SW_HIDE);
        }
        m_visible = false;
    }

    bool visible() const override { return m_visible; }
    bool wasActive() const override { return m_gotActive; }
    bool hasFocus() const override { return m_hwnd && GetForegroundWindow() == m_hwnd; }

    // Forget a dragged position so the next show() goes back to the cursor.
    void resetPosition() override { m_haveUserPos = false; }

    void pumpEvents() override {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    void waitEvents(double timeoutSec) override {
        DWORD ms = static_cast<DWORD>(timeoutSec * 1000.0);
        MsgWaitForMultipleObjectsEx(0, nullptr, ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    bool renderFrame() override {
        if (!m_ready || !m_d3d || !m_d3d->ctx) return false;
        if (!m_d3d->rtv) createRTV();
        if (!m_d3d->rtv) return false;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        bool changed = m_model ? ws::gui::drawFlyout(*m_model) : false;

        ImGui::Render();
        const float clear[4] = {0.06f, 0.06f, 0.07f, 1.0f};
        m_d3d->ctx->OMSetRenderTargets(1, &m_d3d->rtv, nullptr);
        m_d3d->ctx->ClearRenderTargetView(m_d3d->rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        m_d3d->swap->Present(1, 0);
        return changed;
    }

private:
    struct D3D {
        ID3D11Device        *device = nullptr;
        ID3D11DeviceContext *ctx    = nullptr;
        IDXGISwapChain      *swap   = nullptr;
        ID3D11RenderTargetView *rtv = nullptr;
    };

    void updateDpiScale(HWND hwnd) {
        UINT dpi = 96;
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u) {
            typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
            // The void* hop keeps GCC's -Wcast-function-type quiet for the FARPROC.
            auto fn = reinterpret_cast<GetDpiForWindowFn>(
                reinterpret_cast<void *>(GetProcAddress(u, "GetDpiForWindow")));
            if (fn) dpi = fn(hwnd);
        }
        float s = dpi / 96.0f;
        if (s < 1.0f) s = 1.0f;
        m_scale = s;
        if (m_ready) {
            applyDarkStyle();
            ImGui::GetStyle().ScaleAllSizes(m_scale);
            ImGui::GetIO().FontGlobalScale = m_scale;
        }
    }

    void resizeToContent() {
        int w = static_cast<int>(kBaseW * m_scale);
        int h = static_cast<int>(kBaseH * m_scale);
        SetWindowPos(m_hwnd, nullptr, 0, 0, w, h,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        applyRoundedRegion();
    }

    // Ask DWM to round the window corners (Windows 11). When that is not available
    // (older Windows, attribute rejected) we clip the window with a rounded region
    // instead, so the corners are still visibly rounded.
    void applyRoundedCorners() {
        m_dwmRounded = false;
        if (!m_hwnd) return;

        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (dwm) {
            auto setAttr = reinterpret_cast<DwmSetWindowAttributeFn>(
                reinterpret_cast<void *>(GetProcAddress(dwm, "DwmSetWindowAttribute")));
            if (setAttr) {
                int pref = kDwmscpRound;
                if (SUCCEEDED(setAttr(m_hwnd, kDwmwaWindowCornerPreference, &pref, sizeof(pref)))) {
                    m_dwmRounded = true;
                    // No 1px outline around the rounded panel.
                    DWORD none = kDwmColorNone;
                    setAttr(m_hwnd, kDwmwaBorderColor, &none, sizeof(none));
                }
            }
            FreeLibrary(dwm);
        }
        applyRoundedRegion();
    }

    void applyRoundedRegion() {
        // A window region would defeat DWM's own (antialiased) rounding, so only
        // use it as a fallback.
        if (!m_hwnd || m_dwmRounded) return;
        RECT rc;
        if (!GetWindowRect(m_hwnd, &rc)) return;
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) return;
        const int r = static_cast<int>(kCornerRadius * m_scale);
        HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, r * 2, r * 2);
        if (rgn) SetWindowRgn(m_hwnd, rgn, TRUE);   // window takes ownership
    }

    void createRTV() {
        if (!m_d3d->swap || !m_d3d->device) return;
        ID3D11Texture2D *back = nullptr;
        if (SUCCEEDED(m_d3d->swap->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                             reinterpret_cast<void **>(&back))) && back) {
            m_d3d->device->CreateRenderTargetView(back, nullptr, &m_d3d->rtv);
            back->Release();
        }
    }

    void cleanupRTV() {
        if (m_d3d->rtv) {
            m_d3d->rtv->Release();
            m_d3d->rtv = nullptr;
        }
    }

    bool createDeviceObjects() {
        RECT rc;
        GetClientRect(m_hwnd, &rc);
        UINT w = static_cast<UINT>(rc.right - rc.left);
        UINT h = static_cast<UINT>(rc.bottom - rc.top);
        if (w == 0) w = kBaseW;
        if (h == 0) h = kBaseH;

        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 2;
        sd.BufferDesc.Width = w;
        sd.BufferDesc.Height = h;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.RefreshRate.Numerator = 60;
        sd.BufferDesc.RefreshRate.Denominator = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = m_hwnd;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_10_0;

        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            levels, 2, D3D11_SDK_VERSION,
            &sd, &m_d3d->swap, &m_d3d->device, &level, &m_d3d->ctx);
        if (FAILED(hr)) {
            hr = D3D11CreateDeviceAndSwapChain(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                levels, 2, D3D11_SDK_VERSION,
                &sd, &m_d3d->swap, &m_d3d->device, &level, &m_d3d->ctx);
        }
        if (FAILED(hr)) return false;

        createRTV();
        ImGui_ImplDX11_Init(m_d3d->device, m_d3d->ctx);
        return true;
    }

    void cleanupDeviceObjects() {
        // Only an initialised ImGui DX11 backend may be shut down; its state lives
        // in the ImGui context, which destroy() tears down afterwards.
        if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendRendererUserData) {
            ImGui_ImplDX11_Shutdown();
        }
        if (!m_d3d) return;
        cleanupRTV();
        if (m_d3d->swap) { m_d3d->swap->Release(); m_d3d->swap = nullptr; }
        if (m_d3d->ctx) { m_d3d->ctx->Release(); m_d3d->ctx = nullptr; }
        if (m_d3d->device) { m_d3d->device->Release(); m_d3d->device = nullptr; }
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        Win32Flyout *self = nullptr;
        if (msg == WM_NCCREATE) {
            auto cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
            self = static_cast<Win32Flyout *>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<Win32Flyout *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (self) return self->handleMessage(hwnd, msg, wParam, lParam);
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_ERASEBKGND:
                return 1;

            case WM_ACTIVATE:
                if (LOWORD(wParam) == WA_INACTIVE) {
                    // Losing focus is not handled here: the shell waits a short
                    // while (Shell::kFocusLossDelay) before closing the panel, so
                    // a click on the app's own taskbar button reaches the toggle
                    // logic while the panel is still visible. Hiding immediately
                    // would race that toggle.
                } else {
                    m_gotActive = true;
                }
                return 0;

            case WM_KEYDOWN:
                if (wParam == VK_ESCAPE) {
                    hide();
                    return 0;
                }
                break;

            case WM_MOVE:
                // Remember where a user-dragged panel ends up. Programmatic
                // placement happens while the window is still hidden, so it does
                // not overwrite the user's position.
                if (m_visible) {
                    RECT r;
                    if (GetWindowRect(hwnd, &r)) {
                        m_userX = r.left;
                        m_userY = r.top;
                        m_haveUserPos = true;
                    }
                }
                return 0;

            case WM_NCHITTEST: {
                // No frame: make the header behave like a title bar so the panel can
                // be dragged anywhere, while the widgets inside it keep working.
                if (m_model && m_visible) {
                    POINT p;
                    p.x = static_cast<LONG>(static_cast<short>(LOWORD(lParam)));
                    p.y = static_cast<LONG>(static_cast<short>(HIWORD(lParam)));
                    ScreenToClient(hwnd, &p);
                    const float px = static_cast<float>(p.x);
                    const float py = static_cast<float>(p.y);
                    if (m_model->dragArea.contains(px, py) &&
                        !ws::gui::inAnyRect(m_model->dragExcludes, px, py)) {
                        return HTCAPTION;
                    }
                }
                break;
            }

            case WM_NCLBUTTONDBLCLK:
                // Double-click on the header forgets a dragged position: the panel
                // goes back to opening next to the cursor.
                if (wParam == HTCAPTION) {
                    resetPosition();
                    return 0;
                }
                break;

            case WM_SIZE:
                if (m_d3d && m_d3d->swap && wParam != SIZE_MINIMIZED) {
                    cleanupRTV();
                    m_d3d->swap->ResizeBuffers(0, static_cast<UINT>(LOWORD(lParam)),
                                               static_cast<UINT>(HIWORD(lParam)),
                                               DXGI_FORMAT_UNKNOWN, 0);
                    createRTV();
                }
                applyRoundedRegion();
                return 0;

            case WM_DPICHANGED: {
                updateDpiScale(hwnd);
                RECT *r = reinterpret_cast<RECT *>(lParam);
                SetWindowPos(hwnd, nullptr, r->left, r->top,
                             r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }

            case WM_CLOSE:
                hide();
                return 0;

            case WM_DESTROY:
                return 0;

            default:
                break;
        }

        if (m_ready) {
            if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return 1;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    HINSTANCE   m_hInstance = nullptr;
    HWND        m_hwnd = nullptr;
    GuiModel   *m_model = nullptr;
    bool        m_visible = false;
    bool        m_ready = false;
    bool        m_destroyed = false;   // destroy() is called twice (shell + dtor)
    bool        m_gotActive = false;
    float       m_scale = 1.0f;
    bool        m_dwmRounded = false;
    bool        m_haveUserPos = false;   // panel was dragged by the user
    int         m_userX = 0;
    int         m_userY = 0;
    wchar_t     m_className[64] = {};

    D3D *m_d3d = nullptr;
};

std::unique_ptr<Flyout> createFlyout() {
    return std::make_unique<Win32Flyout>();
}

} // namespace gui
} // namespace ws

#endif // _WIN32
