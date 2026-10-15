#pragma once

#include "core/keepalive.h"

#include <memory>
#include <string>

namespace ws {
namespace gui { struct GuiModel; }

namespace app {

// Platform services the portable controller asks the host shell for. The host
// (the shell that owns the tray and the window) implements this; the controller
// itself never calls an OS API.
class Host {
public:
    virtual ~Host() = default;

    // The enabled state changed: update the tray icon/menu to match.
    virtual void onEnabledChanged(bool enabled) { (void)enabled; }

    // Report a problem to the user. May be a no-op (headless tests).
    virtual void showError(const std::string &title, const std::string &message) {
        (void)title; (void)message;
    }
};

// Portable application logic: owns the audio engine and the persisted
// configuration, and turns GUI/state changes into engine calls. It knows
// nothing about the tray, the window or the message loop, so the same
// controller can be driven by a Windows, macOS or Linux shell.
//
// Typical wiring in a host:
//   Controller c(&host);
//   c.load();          // read config (sets the initial enabled state)
//   c.init();          // start the engine if the last session was enabled
//   ... loop: c.syncModel(model); render; c.applyGuiChanges(model);
//   c.save();          // persisted, debounced by the host
//   c.stop();          // on quit
class Controller {
public:
    explicit Controller(Host *host);
    ~Controller();

    Controller(const Controller &) = delete;
    Controller &operator=(const Controller &) = delete;

    // Reads the config from disk (defaults if missing) and adopts its
    // enabled state. Does not touch the engine.
    void load();

    // Starts emitting if the persisted state is enabled. Missing/!startable
    // device is reported through Host::showError and leaves it disabled.
    void init();

    // Enables/disables emission, reporting failures through the host.
    void setEnabled(bool on);
    void toggleEnabled() { setEnabled(!m_enabled); }

    // Copies the current config/state into the GUI model.
    void syncModel(ws::gui::GuiModel &model) const;

    // Applies the GUI model back into the config and the engine. Returns true
    // if any tracked setting changed.
    bool applyGuiChanges(const ws::gui::GuiModel &model);

    // Persists the config, creating the directory if needed (best effort).
    void save() const;

    // Stops the engine (used when the host quits).
    void stop() { if (m_engine) m_engine->stop(); }

    // ---- state ----
    bool enabled() const { return m_enabled; }
    bool firstRun() const { return m_firstRun; }
    const KeepAlive::Config &config() const { return m_config; }
    KeepAlive *engine() { return m_engine.get(); }
    const KeepAlive *engine() const { return m_engine.get(); }

private:
    ws::Settings makeSettings() const;

    Host *m_host;
    std::unique_ptr<KeepAlive> m_engine;
    KeepAlive::Config m_config;
    bool m_enabled = true;
    bool m_firstRun = false;
};

} // namespace app
} // namespace ws
