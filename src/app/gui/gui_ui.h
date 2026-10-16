#pragma once

#include "core/keepalive.h"
#include <string>
#include <vector>

namespace ws {
namespace gui {

struct UiRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool contains(float px, float py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
    bool valid() const { return w > 0.0f && h > 0.0f; }
};

struct GuiModel {
    ws::KeepAlive *keepAlive = nullptr;
    ws::Settings   settings;
    bool           enabled = true;
    std::vector<ws::OutputDevice> devices;
    std::string    backend;

    UiRect dragArea;
    std::vector<UiRect> dragExcludes;

    bool requestHide = false;
    bool requestQuit = false;

    float bulbLit = 0.0f;
    float bulbTime = 0.0f;
};

inline bool inAnyRect(const std::vector<UiRect> &rects, float x, float y) {
    for (const UiRect &r : rects) {
        if (r.contains(x, y)) return true;
    }
    return false;
}

void applyDarkStyle();

bool drawFlyout(GuiModel &m);

void refreshDevices(GuiModel &m);

float amplitudeToDb(float amplitude);
float dbToAmplitude(float db);

constexpr float kMinDb = -80.0f;
constexpr float kMaxDb = 0.0f;

constexpr float kWheelDbPerNotch = 2.0f;

constexpr float kWheelBurstMsPerNotch = 5.0f;

constexpr float kAdviceMinDb = -60.0f;

constexpr float kWrapChars = 17.0f;

constexpr float kBulbSize = 18.0f;
constexpr float kBulbGap = 6.0f;
constexpr float kBulbRampSeconds = 0.20f;
constexpr float kBulbBreathPeriod = 2.5f;

constexpr int kFlyoutW = 320;
constexpr int kFlyoutH = 480;

} // namespace gui
} // namespace ws
