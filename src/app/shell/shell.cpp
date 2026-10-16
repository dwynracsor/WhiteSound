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

constexpr auto kSaveDelay = std::chrono::milliseconds(500);
constexpr auto kFocusLossDelay = std::chrono::milliseconds(300);

class Shell : public ws::app::Host {
public:
    int run();

private:
    void handleCommand(ws::platform::TrayCommand command);
    void activate();
    void togglePanel();
    void showPanel();
    void showConfig() const;
    void showAbout() const;
    void requestClose();
    void shutdown();
    void markDirty();

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
    int m_closeCount = 0;
};

int Shell::run() {
    m_controller = std::make_unique<ws::app::Controller>(this);
    m_controller->load();

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

    m_controller->init();

    if (m_trayOk && m_controller->firstRun()) {
        m_tray->notify("White Sound",
                       "White Sound is running. Use the tray icon to open the "
                       "panel.");
    }

    while (m_running) {
        if (m_guiOk && m_flyout->visible()) {
            m_flyout->pumpEvents();
            if (!m_running) break;

            m_controller->syncModel(m_model);
            const bool guiChanged = m_flyout->renderFrame();
            if (m_model.requestQuit) {
                m_model.requestQuit = false;
                requestClose();
                if (!m_running) break;
            }
            if (m_model.requestHide) {
                m_flyout->hide();
                m_focusLost = false;
            }
            if (guiChanged) {
                m_controller->applyGuiChanges(m_model);
                markDirty();
            }

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
            m_flyout->waitEvents(0.05);
            m_flyout->pumpEvents();
            if (!m_running) break;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

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
    m_model.bulbLit = 0.0f;
    if (m_trayOk) m_tray->setTaskbarVisible(true);
    m_flyout->showNearCursor();
}

void Shell::requestClose() {
    if (m_closeCount++ == 0) {
        if (m_flyout) m_flyout->hide();
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


void Shell::onEnabledChanged(bool enabled) {
    if (m_trayOk) m_tray->setEnabled(enabled);
}

void Shell::showError(const std::string &title, const std::string &message) {
    ws::platform::showMessage(title, message, true);
}


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
