#include "app_controller.h"

#include "app/gui/gui_ui.h"
#include "core/paths.h"
#include "json/json.hpp"

#include <fstream>

using json = nlohmann::json;

namespace ws {
namespace app {

Controller::Controller(Host *host)
    : m_host(host), m_engine(std::make_unique<KeepAlive>()) {}

Controller::~Controller() = default;

ws::Settings Controller::makeSettings() const {
    ws::Settings s;
    s.deviceIndex     = m_config.deviceIndex;
    s.level           = m_config.level;
    s.autoTune        = m_config.autoTune;
    s.maxLevel        = m_config.maxLevel;
    s.noiseMode       = m_config.noiseMode;
    s.burstDurationMs = m_config.burstDurationMs;
    s.burstIntervalMs = m_config.burstIntervalMs;
    return s;
}

void Controller::load() {
    m_firstRun = false;

    std::ifstream file(ws::platform::configFile());
    if (!file.is_open()) {
        m_firstRun = true;
    } else {
        try {
            json j;
            file >> j;
            if (j.contains("deviceIndex")) m_config.deviceIndex = j["deviceIndex"].get<int>();
            if (j.contains("level")) m_config.level = j["level"].get<float>();
            if (j.contains("autoTune")) m_config.autoTune = j["autoTune"].get<bool>();
            if (j.contains("maxLevel")) m_config.maxLevel = j["maxLevel"].get<float>();
            if (j.contains("noiseMode")) m_config.noiseMode = j["noiseMode"].get<int>();
            if (j.contains("burstDurationMs")) m_config.burstDurationMs = j["burstDurationMs"].get<int>();
            if (j.contains("burstIntervalMs")) m_config.burstIntervalMs = j["burstIntervalMs"].get<int>();
            if (j.contains("enabled")) m_config.enabled = j["enabled"].get<bool>();

            if (m_config.level < 0.0f) m_config.level = 0.0f;
            if (m_config.level > 1.0f) m_config.level = 1.0f;
            if (m_config.maxLevel <= 0.0f) m_config.maxLevel = 0.02f;
            if (m_config.maxLevel > 1.0f) m_config.maxLevel = 1.0f;
            if (m_config.noiseMode != 0 && m_config.noiseMode != 1 && m_config.noiseMode != 2) m_config.noiseMode = 0;
            if (m_config.burstDurationMs < 10) m_config.burstDurationMs = 10;
            if (m_config.burstDurationMs > 10000) m_config.burstDurationMs = 10000;
            if (m_config.burstIntervalMs < 100) m_config.burstIntervalMs = 100;
            if (m_config.burstIntervalMs > 60000) m_config.burstIntervalMs = 60000;
        } catch (...) {}
    }

    // Start in the state the last session was left in.
    m_enabled = m_config.enabled;
}

void Controller::init() {
    m_enabled = m_config.enabled;
    if (m_enabled) {
        if (!m_engine->start(makeSettings())) {
            m_host->showError("Failed to start", m_engine->lastError());
            m_enabled = false;
            m_config.enabled = false;
            m_host->onEnabledChanged(false);
        }
    } else {
        m_host->onEnabledChanged(false);
    }
}

void Controller::setEnabled(bool on) {
    if (on == m_enabled) return;

    if (on) {
        if (!m_engine->start(makeSettings())) {
            m_host->showError("Failed to start", m_engine->lastError());
            return;   // stay disabled
        }
    } else {
        m_engine->stop();
    }

    m_enabled = on;
    m_config.enabled = on;
    m_host->onEnabledChanged(on);
}

void Controller::syncModel(ws::gui::GuiModel &model) const {
    model.keepAlive = m_engine.get();
    model.settings.deviceIndex = m_config.deviceIndex;
    model.settings.level = m_config.level;
    model.settings.autoTune = m_config.autoTune;
    model.settings.maxLevel = m_config.maxLevel;
    model.settings.noiseMode = m_config.noiseMode;
    model.settings.burstDurationMs = m_config.burstDurationMs;
    model.settings.burstIntervalMs = m_config.burstIntervalMs;
    model.enabled = m_enabled;
    model.backend = ws::getBackendName();
}

bool Controller::applyGuiChanges(const ws::gui::GuiModel &model) {
    const bool enableChanged = (model.enabled != m_enabled);
    const bool deviceChanged = (model.settings.deviceIndex != m_config.deviceIndex);
    const bool autoChanged   = (model.settings.autoTune != m_config.autoTune);
    const bool noiseChanged  = (model.settings.noiseMode != m_config.noiseMode ||
                                model.settings.burstDurationMs != m_config.burstDurationMs ||
                                model.settings.burstIntervalMs != m_config.burstIntervalMs);

    m_config.enabled         = model.enabled;
    m_config.deviceIndex     = model.settings.deviceIndex;
    m_config.level           = model.settings.level;
    m_config.autoTune        = model.settings.autoTune;
    m_config.maxLevel        = model.settings.maxLevel;
    m_config.noiseMode       = model.settings.noiseMode;
    m_config.burstDurationMs = model.settings.burstDurationMs;
    m_config.burstIntervalMs = model.settings.burstIntervalMs;

    if (enableChanged) {
        m_enabled = model.enabled;
        if (m_enabled) {
            if (!m_engine->start(makeSettings())) {
                m_host->showError("Failed to start", m_engine->lastError());
                m_enabled = false;
            }
        } else {
            m_engine->stop();
        }
        m_host->onEnabledChanged(m_enabled);
    } else if (deviceChanged && m_enabled) {
        if (!m_engine->start(makeSettings())) {
            m_host->showError("Failed to start", m_engine->lastError());
        }
    } else if (autoChanged) {
        m_engine->setAutoTune(m_config.autoTune);
    } else if (noiseChanged) {
        m_engine->setNoiseMode(m_config.noiseMode);
        m_engine->setBurstDuration(m_config.burstDurationMs);
        m_engine->setBurstInterval(m_config.burstIntervalMs);
    } else {
        m_engine->setLevel(m_config.level);
    }

    return enableChanged || deviceChanged || autoChanged || noiseChanged;
}

void Controller::save() const {
    const std::filesystem::path path = ws::platform::configFile();
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    json j;
    j["deviceIndex"] = m_config.deviceIndex;
    j["level"] = m_config.level;
    j["autoTune"] = m_config.autoTune;
    j["maxLevel"] = m_config.maxLevel;
    j["noiseMode"] = m_config.noiseMode;
    j["burstDurationMs"] = m_config.burstDurationMs;
    j["burstIntervalMs"] = m_config.burstIntervalMs;
    j["enabled"] = m_config.enabled;

    std::ofstream file(path);
    if (file.is_open()) {
        file << j.dump(2);
    }
}

} // namespace app
} // namespace ws
