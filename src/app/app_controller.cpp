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
    s.toneHz          = m_config.toneHz;
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
            if (j.contains("toneHz")) m_config.toneHz = j["toneHz"].get<int>();
            if (j.contains("enabled")) m_config.enabled = j["enabled"].get<bool>();

            if (m_config.level < 0.0f) m_config.level = 0.0f;
            if (m_config.level > 1.0f) m_config.level = 1.0f;
            if (m_config.maxLevel <= 0.0f) m_config.maxLevel = 0.02f;
            if (m_config.maxLevel > 1.0f) m_config.maxLevel = 1.0f;
            m_config.noiseMode = ws::clampNoiseMode(m_config.noiseMode);
            if (m_config.burstDurationMs < 10) m_config.burstDurationMs = 10;
            if (m_config.burstDurationMs > 10000) m_config.burstDurationMs = 10000;
            if (m_config.burstIntervalMs < 100) m_config.burstIntervalMs = 100;
            if (m_config.burstIntervalMs > 60000) m_config.burstIntervalMs = 60000;
            if (m_config.toneHz < 20) m_config.toneHz = 20;
            if (m_config.toneHz > 15000) m_config.toneHz = 15000;

            if (j.contains("levelsByMode") && j["levelsByMode"].is_array()) {
                const auto &levels = j["levelsByMode"];
                for (int i = 0; i < ws::kNoiseModeCount && i < static_cast<int>(levels.size()); ++i) {
                    if (levels[i].is_number()) {
                        float v = levels[i].get<float>();
                        if (v < 0.0f) v = 0.0f;
                        if (v > 1.0f) v = 1.0f;
                        m_config.levelByMode[i] = v;
                    }
                }
            }

            // Start at whatever level was tuned for the active noise mode.
            const int mode = ws::clampNoiseMode(m_config.noiseMode);
            if (m_config.levelByMode[mode] > 0.0f) {
                m_config.level = m_config.levelByMode[mode];
            }
        } catch (...) {}
    }

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
            return;
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
    model.settings.toneHz = m_config.toneHz;
    model.enabled = m_enabled;
    model.backend = ws::getBackendName();
}

bool Controller::applyGuiChanges(const ws::gui::GuiModel &model) {
    const int   prevMode  = m_config.noiseMode;
    const float prevLevel = m_config.level;

    const bool enableChanged = (model.enabled != m_enabled);
    const bool deviceChanged = (model.settings.deviceIndex != m_config.deviceIndex);
    const bool autoChanged   = (model.settings.autoTune != m_config.autoTune);
    const bool modeChanged   = (model.settings.noiseMode != prevMode);
    const bool noiseChanged  = (modeChanged ||
                                model.settings.burstDurationMs != m_config.burstDurationMs ||
                                model.settings.burstIntervalMs != m_config.burstIntervalMs ||
                                model.settings.toneHz != m_config.toneHz);

    m_config.enabled         = model.enabled;
    m_config.deviceIndex     = model.settings.deviceIndex;
    m_config.autoTune        = model.settings.autoTune;
    m_config.maxLevel        = model.settings.maxLevel;
    m_config.noiseMode       = model.settings.noiseMode;
    m_config.burstDurationMs = model.settings.burstDurationMs;
    m_config.burstIntervalMs = model.settings.burstIntervalMs;
    m_config.toneHz          = model.settings.toneHz;

    // The volume is remembered per noise mode: the level tuned for the mode we
    // are leaving is stored, and the level tuned for the one we are entering is
    // restored (or the current one, if that mode was never tuned). This is what
    // makes the level feel automatic when switching noise types.
    if (modeChanged) {
        m_config.levelByMode[ws::clampNoiseMode(prevMode)] = model.settings.level;
        const int entering = ws::clampNoiseMode(model.settings.noiseMode);
        m_config.level = (m_config.levelByMode[entering] > 0.0f)
                             ? m_config.levelByMode[entering]
                             : model.settings.level;
    } else {
        m_config.level = model.settings.level;
    }

    const bool levelChanged = (m_config.level != prevLevel);

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
    } else {
        // Every live setting is applied, so two changes in the same frame (for
        // example dragging the tone frequency and the volume together) do not
        // lose one of them.
        if (autoChanged) {
            m_engine->setAutoTune(m_config.autoTune);
        }
        if (noiseChanged) {
            m_engine->setNoiseMode(m_config.noiseMode);
            m_engine->setBurstDuration(m_config.burstDurationMs);
            m_engine->setBurstInterval(m_config.burstIntervalMs);
            m_engine->setToneHz(m_config.toneHz);
        }
        if (levelChanged) {
            m_engine->setLevel(m_config.level);
        }
    }

    return enableChanged || deviceChanged || autoChanged || levelChanged || noiseChanged;
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
    j["toneHz"] = m_config.toneHz;
    j["enabled"] = m_config.enabled;

    // One level per noise mode, with the live level folded into the active mode
    // so the file always reflects what is playing right now.
    const int active = ws::clampNoiseMode(m_config.noiseMode);
    json levels = json::array();
    for (int i = 0; i < ws::kNoiseModeCount; ++i) {
        levels.push_back((i == active) ? m_config.level : m_config.levelByMode[i]);
    }
    j["levelsByMode"] = levels;

    std::ofstream file(path);
    if (file.is_open()) {
        file << j.dump(2);
    }
}

} // namespace app
} // namespace ws
