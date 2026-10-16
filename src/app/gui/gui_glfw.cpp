#include "flyout.h"

#include "gui_ui.h"

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_glfw.h"
#include "imgui/backends/imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include <cmath>
#include <cstdio>

#if defined(_WIN32)
#  include <windows.h>
#  define GLFW_EXPOSE_NATIVE_WIN32
#  include <GLFW/glfw3native.h>
#  include <GL/gl.h>
#elif defined(__APPLE__)
#  include <OpenGL/gl3.h>
#else
#  include <GL/gl.h>
#endif

namespace ws {
namespace gui {
namespace {

constexpr int kBaseW = kFlyoutW;
constexpr int kBaseH = kFlyoutH;
constexpr int kCornerRadius = 10;

#if defined(_WIN32)
constexpr DWORD kDwmwaWindowCornerPreference = 33;
constexpr DWORD kDwmwaBorderColor = 34;
constexpr int   kDwmscpRound = 2;
constexpr DWORD kDwmColorNone = 0xFFFFFFFE;
typedef HRESULT(WINAPI *DwmSetWindowAttributeFn)(HWND, DWORD, LPCVOID, DWORD);

void forceForeground(HWND hwnd) {
    if (!hwnd || GetForegroundWindow() == hwnd) return;
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
#endif

void glfwErrorCallback(int code, const char *desc) {
    std::fprintf(stderr, "[glfw] error %d: %s\n", code, desc ? desc : "");
}

} // namespace

class GlfwFlyout : public Flyout {
public:
    ~GlfwFlyout() override { destroy(); }

    bool create(GuiModel *model) override {
        m_model = model;

        glfwSetErrorCallback(&glfwErrorCallback);
        if (!glfwInit()) {
            std::fprintf(stderr, "[glfw] glfwInit failed\n");
            return false;
        }
        m_glfwInited = true;

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
        glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

        m_window = glfwCreateWindow(kBaseW, kBaseH, "White Sound", nullptr, nullptr);
        if (!m_window) {
            std::fprintf(stderr, "[glfw] window creation failed (no OpenGL 3.2?)\n");
            destroy();
            return false;
        }

        glfwSetWindowUserPointer(m_window, this);
        glfwSetWindowFocusCallback(m_window, &GlfwFlyout::onFocus);
        glfwSetWindowCloseCallback(m_window, &GlfwFlyout::onClose);

        glfwMakeContextCurrent(m_window);
        glfwSwapInterval(1);

        float sx = 1.0f, sy = 1.0f;
        glfwGetWindowContentScale(m_window, &sx, &sy);
        m_scale = sx > 0.0f ? sx : 1.0f;
        if (m_scale < 1.0f) m_scale = 1.0f;
        if (m_scale != 1.0f) {
            glfwSetWindowSize(m_window,
                              static_cast<int>(kBaseW * m_scale),
                              static_cast<int>(kBaseH * m_scale));
        }

#if defined(_WIN32)
        applyWindowsChrome();
#endif

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

#if defined(_WIN32)
        if (io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.0f) == nullptr) {
            io.Fonts->AddFontDefault();
        }
#else
        io.Fonts->AddFontDefault();
#endif

        applyDarkStyle();
        ImGui::GetStyle().ScaleAllSizes(m_scale);
        io.FontGlobalScale = m_scale;

        ImGui_ImplGlfw_InitForOpenGL(m_window, true);
        ImGui_ImplOpenGL3_Init("#version 150");
        m_ready = true;
        return true;
    }

    void destroy() override {
        if (m_destroyed) return;
        m_destroyed = true;
        if (m_ready) {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            m_ready = false;
        }
        if (ImGui::GetCurrentContext()) {
            ImGui::DestroyContext();
        }
        if (m_window) {
            glfwDestroyWindow(m_window);
            m_window = nullptr;
        }
        if (m_glfwInited) {
            glfwTerminate();
            m_glfwInited = false;
        }
        m_visible = false;
        m_dragging = false;
    }

    void showNearCursor() override {
        if (!m_window) return;
        int w = 0, h = 0;
        glfwGetWindowSize(m_window, &w, &h);

        int x = 0, y = 0;
        if (m_haveUserPos) {
            x = m_userX;
            y = m_userY;
            clampToWorkArea(x, y, w, h);
        } else {
            int ax = 0, ay = 0;
            anchorPos(ax, ay);
            placeNearAnchor(ax, ay, w, h, x, y);
        }

        m_gotActive = false;
        glfwSetWindowPos(m_window, x, y);
        glfwShowWindow(m_window);
#if defined(_WIN32)
        forceForeground(glfwGetWin32Window(m_window));
#endif
        glfwFocusWindow(m_window);
        m_visible = true;
    }

    void hide() override {
        if (m_window) glfwHideWindow(m_window);
        m_visible = false;
        m_dragging = false;
    }

    bool visible() const override { return m_visible; }
    bool wasActive() const override { return m_gotActive; }
    bool hasFocus() const override {
        return m_window && glfwGetWindowAttrib(m_window, GLFW_FOCUSED);
    }
    void resetPosition() override { m_haveUserPos = false; }

    void pumpEvents() override { if (m_glfwInited) glfwPollEvents(); }
    void waitEvents(double timeoutSec) override {
        if (m_glfwInited) glfwWaitEventsTimeout(timeoutSec);
    }

    bool renderFrame() override {
        if (!m_ready || !m_window) return false;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const bool changed = m_model ? ws::gui::drawFlyout(*m_model) : false;
        updateDrag();

        const bool wantHide = ImGui::IsKeyPressed(ImGuiKey_Escape, false);

        ImGui::Render();
        int fbW = 0, fbH = 0;
        glfwGetFramebufferSize(m_window, &fbW, &fbH);
        glViewport(0, 0, fbW, fbH);
        glClearColor(0.06f, 0.06f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(m_window);

        if (wantHide) hide();
        return changed;
    }

private:
    static void onFocus(GLFWwindow *w, int focused) {
        auto *self = static_cast<GlfwFlyout *>(glfwGetWindowUserPointer(w));
        if (!self) return;
        if (focused) {
            self->m_gotActive = true;
        }
    }
    static void onClose(GLFWwindow *w) {
        auto *self = static_cast<GlfwFlyout *>(glfwGetWindowUserPointer(w));
        if (self) self->hide();
    }

    void anchorPos(int &x, int &y) const {
#if defined(_WIN32)
        POINT p;
        if (GetCursorPos(&p)) {
            x = p.x;
            y = p.y;
            return;
        }
#endif
        int mx = 0, my = 0, mw = 0, mh = 0;
        GLFWmonitor *mon = glfwGetPrimaryMonitor();
        if (mon) {
            glfwGetMonitorWorkarea(mon, &mx, &my, &mw, &mh);
        }
        if (mw > 0 && mh > 0) {
            x = mx + mw - 24;
#if defined(__APPLE__)
            y = my + 24;
#else
            y = my + mh - 24;
#endif
            return;
        }
        x = 0;
        y = 0;
    }

    void workAreaAround(int x, int y, int &mx, int &my, int &mw, int &mh) const {
        mx = my = 0;
        mw = mh = 0;
#if defined(_WIN32)
        POINT p;
        p.x = x;
        p.y = y;
        HMONITOR hm = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(hm, &mi)) {
            mx = mi.rcWork.left;
            my = mi.rcWork.top;
            mw = mi.rcWork.right - mi.rcWork.left;
            mh = mi.rcWork.bottom - mi.rcWork.top;
            return;
        }
#endif
        GLFWmonitor *mon = glfwGetPrimaryMonitor();
        if (mon) glfwGetMonitorWorkarea(mon, &mx, &my, &mw, &mh);
    }

    void placeNearAnchor(int ax, int ay, int w, int h, int &x, int &y) const {
        int mx = 0, my = 0, mw = 0, mh = 0;
        workAreaAround(ax, ay, mx, my, mw, mh);

        x = ax - w / 2;
        y = ay - h - 12;
        if (mw > 0 && mh > 0) {
            if (x < mx) x = mx;
            if (x + w > mx + mw) x = mx + mw - w;
            if (y < my) y = ay + 20;
            if (y + h > my + mh) y = my + mh - h;
        }
    }

    void clampToWorkArea(int &x, int &y, int w, int h) const {
        int mx = 0, my = 0, mw = 0, mh = 0;
        workAreaAround(x + w / 2, y + h / 2, mx, my, mw, mh);
        if (mw <= 0 || mh <= 0) return;
        if (x + w > mx + mw) x = mx + mw - w;
        if (y + h > my + mh) y = my + mh - h;
        if (x < mx) x = mx;
        if (y < my) y = my;
    }

    void updateDrag() {
        if (!m_model || !m_window) return;
        ImGuiIO &io = ImGui::GetIO();
        const bool onHeader =
            m_model->dragArea.contains(io.MousePos.x, io.MousePos.y) &&
            !inAnyRect(m_model->dragExcludes, io.MousePos.x, io.MousePos.y);

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && onHeader) {
            const double now = glfwGetTime();
            if (m_lastClickValid && (now - m_lastClickTime) <= 0.4 &&
                std::fabs(io.MousePos.x - m_lastClickX) <= 8.0f &&
                std::fabs(io.MousePos.y - m_lastClickY) <= 8.0f) {
                m_lastClickValid = false;
                resetPosition();
                return;
            }
            m_lastClickValid = true;
            m_lastClickTime = now;
            m_lastClickX = io.MousePos.x;
            m_lastClickY = io.MousePos.y;
        }

        if (!m_dragging && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && onHeader) {
            m_dragging = true;
            m_dragMoved = false;
            m_dragMouseX = io.MousePos.x;
            m_dragMouseY = io.MousePos.y;
        }

        if (m_dragging) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                int wx = 0, wy = 0;
                glfwGetWindowPos(m_window, &wx, &wy);
                const int nx = wx + static_cast<int>(io.MousePos.x - m_dragMouseX);
                const int ny = wy + static_cast<int>(io.MousePos.y - m_dragMouseY);
                if (nx != wx || ny != wy) m_dragMoved = true;
                glfwSetWindowPos(m_window, nx, ny);
            } else {
                m_dragging = false;
                if (m_dragMoved) {
                    int wx = 0, wy = 0;
                    glfwGetWindowPos(m_window, &wx, &wy);
                    m_userX = wx;
                    m_userY = wy;
                    m_haveUserPos = true;
                }
            }
        }
    }

#if defined(_WIN32)
    void applyWindowsChrome() {
        HWND hwnd = glfwGetWin32Window(m_window);
        if (!hwnd) return;
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        ex |= WS_EX_TOOLWINDOW;
        ex &= ~WS_EX_APPWINDOW;
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);

        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

        bool rounded = false;
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (dwm) {
            auto setAttr = reinterpret_cast<DwmSetWindowAttributeFn>(
                reinterpret_cast<void *>(GetProcAddress(dwm, "DwmSetWindowAttribute")));
            if (setAttr) {
                int pref = kDwmscpRound;
                if (SUCCEEDED(setAttr(hwnd, kDwmwaWindowCornerPreference, &pref, sizeof(pref)))) {
                    rounded = true;
                    DWORD none = kDwmColorNone;
                    setAttr(hwnd, kDwmwaBorderColor, &none, sizeof(none));
                }
            }
            FreeLibrary(dwm);
        }
        if (rounded) return;

        RECT rc;
        if (!GetWindowRect(hwnd, &rc)) return;
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) return;
        const int r = static_cast<int>(kCornerRadius * m_scale);
        HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, r * 2, r * 2);
        if (rgn) SetWindowRgn(hwnd, rgn, TRUE);
    }
#endif

    GLFWwindow *m_window = nullptr;
    GuiModel   *m_model = nullptr;
    bool        m_glfwInited = false;
    bool        m_ready = false;
    bool        m_destroyed = false;
    bool        m_visible = false;
    bool        m_gotActive = false;
    float       m_scale = 1.0f;
    bool        m_haveUserPos = false;
    int         m_userX = 0;
    int         m_userY = 0;

    bool   m_dragging = false;
    bool   m_dragMoved = false;
    double m_dragMouseX = 0.0;
    double m_dragMouseY = 0.0;

    bool   m_lastClickValid = false;
    double m_lastClickTime = 0.0;
    float  m_lastClickX = 0.0f;
    float  m_lastClickY = 0.0f;
};

std::unique_ptr<Flyout> createFlyout() {
    return std::make_unique<GlfwFlyout>();
}

} // namespace gui
} // namespace ws
