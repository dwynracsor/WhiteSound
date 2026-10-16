#pragma once

#include "core/keepalive.h"

#include <memory>
#include <string>

namespace ws {
namespace gui { struct GuiModel; }

namespace app {

class Host {
public:
    virtual ~Host() = default;

    virtual void onEnabledChanged(bool enabled) { (void)enabled; }

    virtual void showError(const std::string &title, const std::string &message) {
        (void)title; (void)message;
    }
};

class Controller {
public:
    explicit Controller(Host *host);
    ~Controller();

    Controller(const Controller &) = delete;
    Controller &operator=(const Controller &) = delete;

    void load();

    void init();

    void setEnabled(bool on);
    void toggleEnabled() { setEnabled(!m_enabled); }

    void syncModel(ws::gui::GuiModel &model) const;

    bool applyGuiChanges(const ws::gui::GuiModel &model);

    void save() const;

    void stop() { if (m_engine) m_engine->stop(); }

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
