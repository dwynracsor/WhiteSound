#pragma once

#include "core/keepalive.h"
#include <string>
#include <vector>

namespace ws {
namespace gui {

// A rectangle in ImGui screen coordinates (which match the window's client
// pixels for our borderless flyout).
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

// Backend-agnostic model for the flyout UI. The host (Win32/SDL) owns the
// KeepAlive engine and config persistence; it fills this model each frame and
// reads it back after drawFlyout() to apply changes.
struct GuiModel {
    ws::KeepAlive *keepAlive = nullptr;
    ws::Settings   settings;               // working copy (deviceIndex, level)
    bool           enabled = true;
    std::vector<ws::OutputDevice> devices;
    std::string    backend;

    // Filled by drawFlyout(): the title bar + header act as a drag handle so the
    // host can move the panel around the screen. dragExcludes covers the widgets
    // inside them that must keep receiving their own clicks (the power and the
    // window buttons).
    UiRect dragArea;
    std::vector<UiRect> dragExcludes;

    // One-shot requests raised by the window buttons; the host consumes them
    // right after drawing (minimize hides the panel, close exits the app).
    bool requestHide = false;
    bool requestQuit = false;

    // 0..1 "lit" value of the status bulb, animated towards `enabled` by
    // drawFlyout(). Lives in the model (not in static state) so the ramp
    // survives between frames; the host may reset it to 0 when the panel is
    // shown so the bulb visibly switches on as the panel opens.
    float bulbLit = 0.0f;
    // Seconds spent in the current bulb state, used to drive the breathe pulse.
    float bulbTime = 0.0f;
};

// True when (x, y) lies inside any of the given rectangles.
inline bool inAnyRect(const std::vector<UiRect> &rects, float x, float y) {
    for (const UiRect &r : rects) {
        if (r.contains(x, y)) return true;
    }
    return false;
}

// Apply the dark theme. Call once after ImGui::CreateContext().
void applyDarkStyle();

// Draw the flyout contents into the current ImGui frame.
// Returns true if anything changed (level dragging, device, enable toggle).
bool drawFlyout(GuiModel &m);

// Reload the output device list from the engine.
void refreshDevices(GuiModel &m);

// Amplitude (0..1) <-> slider position in dBFS.
float amplitudeToDb(float amplitude);
float dbToAmplitude(float db);

// Slider range in dBFS.
constexpr float kMinDb = -80.0f;
constexpr float kMaxDb = 0.0f;

// Volume change applied per mouse-wheel notch while hovering the volume slider.
constexpr float kWheelDbPerNotch = 2.0f;

// Below this level the keep-alive may not provide enough energy to keep a
// Bluetooth amplifier awake; the UI shows a warning when the user goes lower.
constexpr float kAdviceMinDb = -60.0f;

// Tooltip wrap position, in multiples of the font size. The flyout is narrow and
// its render target clips anything drawn outside 320 logical px, so hints must
// stay narrower than the panel to remain readable (21 * 13 px + padding still
// leaves room inside the window, even at the pointer's far right).
constexpr float kWrapChars = 21.0f;

// Status bulb: the app icon drawn in the header as a little lamp. "Off" is a
// dark, desaturated LED, "on" is the icon's cyan-to-blue badge with a soft halo.
constexpr float kBulbSize = 18.0f;          // logical px, square badge
constexpr float kBulbGap = 6.0f;            // badge -> status text
constexpr float kBulbRampSeconds = 0.20f;   // off <-> on transition
constexpr float kBulbBreathPeriod = 2.5f;   // glow pulse while lit

} // namespace gui
} // namespace ws
