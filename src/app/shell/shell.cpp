// Portable application shell.
//
// This is the only place that ties the three portable pieces together: the
// controller (config + audio engine orchestration), the flyout window host and
// the native tray. It contains no OS call at all, so the exact same loop drives
// Windows, macOS and Linux; everything platform-specific lives behind
// ws::platform::Tray and ws::platform::showMessage().
//
// Entry points (main_win32.cpp / main_macos.cpp / main_linux.cpp) only do the
// per-OS bootstrapping (DPI, single instance) and then call shell::run().

#include "shell.h"

#include "app/app_controller.h"
#include "app/gui/flyout.h"
#include "app/gui/gui_ui.h"
#include "app/platform/platform.h"
#include "app/platform/tray.h"
#include "core/keepalive.h"
#include "core/paths.h"
#include "ws_version.h"

#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace ws {
namespace shell {
namespace {

using Clock = std::chrono::steady_clock;

// Debounce window for persisting the config after a live change.
constexpr auto kSaveDelay = std::chrono::milliseconds(500);
// Wait for the window to settle before hiding it on focus loss. This is now
// the only place that closes the panel on focus loss (the window backends no
// longer hide it themselves), so it also has to be longer than the time it
// takes a click on the taskbar button to reach the toggle logic.
constexpr auto kFocusLossDelay = std::chrono::milliseconds(300);

class Shell : public ws::app::Host {
public:
    int run();

private:
    // ---- loop helpers ------------------------------------------------------
    void handleCommand(ws::platform::TrayCommand command);
    void activate();       // primary tray click
    void togglePanel();    // hotkey / menu "Open panel"
    void showPanel();      // bring the flyout up with a fresh snapshot
    void showConfig() const;
    void showAbout() const;
    void requestClose();   // the panel's X / the taskbar button's close
    void shutdown();
    void markDirty();

    // ---- ws::app::Host -----------------------------------------------------
    void onEnabledChanged(bool enabled) override;
    void showError(const std::string &title, const std::string &message) override;

    std::unique_ptr<ws::app::Controller> m_controller;
    std::unique_ptr<ws::gui::Flyout> m_flyout;
    std::unique_ptr<ws::platform::Tray> m_tray;
    ws::gui::GuiModel m_model;

    bool m_guiOk = false;
    bool m_trayOk = false;
    bool m_running = true;
    bool m_shutdown = false;
    bool m_dirty = false;
    Clock::time_point m_dirtySince;
    bool m_focusLost = false;
    Clock::time_point m_focusLostSince;
    // Number of close requests handled since the app started. The first one
    // only steps back to the tray; the next one exits for good (see
    // requestClose()).
    int m_closeCount = 0;
};

int Shell::run() {
    m_controller = std::make_unique<ws::app::Controller>(this);
    m_controller->load();

    // The window host first. On macOS it is what brings the NSApplication up
    // that the status item later attaches to.
    m_flyout = ws::gui::createFlyout();
    m_guiOk = m_flyout && m_flyout->create(&m_model);

    ws::platform::TrayCallbacks callbacks;
    callbacks.onCommand = [this](ws::platform::TrayCommand c) { handleCommand(c); };
    callbacks.onActivate = [this] { activate(); };
    callbacks.onHotkey = [this] { togglePanel(); };
    callbacks.onClose = [this] { requestClose(); };

    m_tray = ws::platform::createTray();
    m_trayOk = m_tray && m_tray->create(callbacks);
    if (m_trayOk) m_tray->setEnabled(m_controller->enabled());

    // Starts the engine if the last session was enabled; may report an error
    // through showError() and leave the app disabled.
    m_controller->init();

    if (m_trayOk && m_controller->firstRun()) {
        m_tray->notify("White Sound",
                       "White Sound is running. Use the tray icon to open the "
                       "panel.");
    }

    while (m_running) {
        if (m_guiOk && m_flyout->visible()) {
            // One frame: service events, redraw, then react to the requests the
            // panel raised (its window buttons and the focus-loss net).
            m_flyout->pumpEvents();
            if (!m_running) break;

            m_controller->syncModel(m_model);
            const bool guiChanged = m_flyout->renderFrame();
            if (m_model.requestQuit) {
                // The panel's X button. Same two-step close as the taskbar
                // button: the first press hides the panel and leaves the app in
                // the tray, the next one exits.
                m_model.requestQuit = false;
                requestClose();
                if (!m_running) break;
            }
            if (m_model.requestHide) {
                m_flyout->hide();   // the window's minimize button
                m_focusLost = false;
            }
            if (guiChanged) {
                m_controller->applyGuiChanges(m_model);
                markDirty();
            }

            // Safety net: close when the window lost focus (covers the case
            // where the OS never reported it). Only once the window actually
            // held focus, to avoid hiding it when activation was refused.
            if (m_flyout->wasActive() && !m_flyout->hasFocus()) {
                if (!m_focusLost) {
                    m_focusLost = true;
                    m_focusLostSince = Clock::now();
                } else if (Clock::now() - m_focusLostSince > kFocusLossDelay) {
                    m_flyout->hide();
                    m_focusLost = false;
                }
            } else {
                m_focusLost = false;
            }
        } else if (m_guiOk) {
            // Hidden: block for an event instead of spinning.
            m_flyout->waitEvents(0.05);
            m_flyout->pumpEvents();
            if (!m_running) break;
        } else {
            // No renderer: keep the tray (and its hotkey) alive on their own.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        // Linux services its D-Bus connection here; a no-op on Windows/macOS,
        // where the window pump already delivered the tray's events.
        if (m_trayOk) m_tray->pump();

        if (m_dirty && Clock::now() - m_dirtySince > kSaveDelay) {
            m_controller->save();
            m_dirty = false;
        }
    }

    if (m_dirty) m_controller->save();
    if (m_trayOk) m_tray->destroy();
    if (m_guiOk) m_flyout->destroy();
    return 0;
}

void Shell::handleCommand(ws::platform::TrayCommand command) {
    switch (command) {
        case ws::platform::TrayCommand::OpenPanel:
            togglePanel();
            break;
        case ws::platform::TrayCommand::ToggleEnabled:
            m_controller->toggleEnabled();
            markDirty();
            break;
        case ws::platform::TrayCommand::ShowConfig:
            showConfig();
            break;
        case ws::platform::TrayCommand::ShowAbout:
            showAbout();
            break;
        case ws::platform::TrayCommand::Quit:
            shutdown();
            break;
    }
}

void Shell::activate() {
    if (m_guiOk) {
        togglePanel();
    } else if (m_trayOk) {
        m_tray->showMenu();
    }
}

void Shell::togglePanel() {
    if (!m_guiOk) {
        if (m_trayOk) m_tray->showMenu();
        return;
    }
    if (!m_flyout->visible()) {
        showPanel();
    } else {
        m_flyout->hide();
    }
}

void Shell::showPanel() {
    m_controller->syncModel(m_model);
    ws::gui::refreshDevices(m_model);
    m_model.bulbLit = 0.0f;   // the status lamp visibly switches on
    // The panel is the app's only real window, so (on Windows) it also brings
    // back the app's taskbar button while it is open.
    if (m_trayOk) m_tray->setTaskbarVisible(true);
    m_flyout->showNearCursor();
}

void Shell::requestClose() {
    // A close request comes from the panel's X button or from the taskbar
    // button's own close (see the tray backend). The first one must not exit:
    // it hides the panel and leaves the app running so the tray icon and the
    // taskbar button stay. Only a following close really stops the app.
    if (m_closeCount++ == 0) {
        if (m_flyout) m_flyout->hide();
        // Closing steps back to the tray only: drop the app's taskbar button so
        // it is no longer listed on the taskbar while it keeps running.
        if (m_trayOk) m_tray->setTaskbarVisible(false);
        m_focusLost = false;
        return;
    }
    shutdown();
}

void Shell::shutdown() {
    if (m_shutdown) return;
    m_shutdown = true;
    if (m_controller) m_controller->stop();
    m_running = false;
}

void Shell::markDirty() {
    m_dirty = true;
    m_dirtySince = Clock::now();
}

// ---- ws::app::Host ---------------------------------------------------------

void Shell::onEnabledChanged(bool enabled) {
    if (m_trayOk) m_tray->setEnabled(enabled);
}

void Shell::showError(const std::string &title, const std::string &message) {
    ws::platform::showMessage(title, message, true);
}

// ---- dialogs ---------------------------------------------------------------

void Shell::showConfig() const {
    const ws::KeepAlive::Config &cfg = m_controller->config();

    std::string msg = "Configuration:\n\n";
    msg += "Device: ";
    msg += (cfg.deviceIndex >= 0 ? std::to_string(cfg.deviceIndex)
                                 : std::string("Default"));
    msg += "\nEnabled: ";
    msg += (m_controller->enabled() ? "on" : "off");
    msg += "\nLevel: " + std::to_string(cfg.level);
    msg += "\nNoise: ";
    if (cfg.noiseMode == 1) {
        msg += "pink";
    } else if (cfg.noiseMode == 2) {
        msg += "burst (" + std::to_string(cfg.burstDurationMs) + " ms every " +
               std::to_string(cfg.burstIntervalMs) + " ms)";
    } else {
        msg += "white";
    }
    msg += "\nAuto-tune: ";
    msg += (cfg.autoTune ? "on" : "off");

    // effectiveLevel() is what is actually emitted: the auto-tuner's current
    // value when it is on, otherwise the baseline. Shown whenever the engine
    // exists, not only when the tuner is active.
    const ws::KeepAlive *engine = m_controller->engine();
    if (engine) {
        msg += " (effective " + std::to_string(engine->effectiveLevel()) + ")";
    }
    if (engine) {
        const std::string opened = engine->lastOpenedDeviceName();
        if (!opened.empty()) {
            msg += "\nOpened on: " + opened + "  (" +
                   std::to_string(engine->lastSampleRate()) + " Hz)";
        }
    }
    msg += "\n\nEdit the configuration file:\n";
    msg += ws::platform::configFile().u8string();

    ws::platform::showMessage("Configuration", msg, false);
}

void Shell::showAbout() const {
    std::string msg =
        "White Sound - Bluetooth Keep Alive\n\n"
        "Emits very low level noise to keep\n"
        "Bluetooth speakers from sleeping their amplifier.\n\n"
        "Version " WS_VERSION_STRING "\n"
        "Backend: miniaudio (";
    msg += ws::getBackendName();
    msg += ")";

    // Global hotkey is registered by the tray (Windows/macOS; not on Linux). Say
    // so when it is not active, since registration fails silently when another
    // application already owns the combination.
    msg += "\nGlobal hotkey: ";
    msg += (m_trayOk && m_tray->hotkeyAvailable()) ? "active" : "not active";

    ws::platform::showMessage("About", msg, false);
}

} // namespace

int run() {
    Shell shell;
    return shell.run();
}

} // namespace shell
} // namespace ws
