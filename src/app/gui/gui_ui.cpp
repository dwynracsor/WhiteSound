#include "gui_ui.h"

#include "imgui/imgui.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace ws {
namespace gui {

void applyDarkStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle &s = ImGui::GetStyle();

    s.WindowRounding    = 10.0f;
    s.ChildRounding     = 8.0f;
    s.FrameRounding     = 6.0f;
    s.PopupRounding     = 6.0f;
    s.GrabRounding      = 6.0f;
    s.ScrollbarRounding = 6.0f;
    s.WindowBorderSize  = 0.0f;
    s.FrameBorderSize   = 0.0f;
    s.WindowPadding     = ImVec2(12.0f, 10.0f);
    s.FramePadding      = ImVec2(9.0f, 5.0f);
    s.ItemSpacing       = ImVec2(8.0f, 8.0f);

    const ImVec4 accent   = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    const ImVec4 accentHi = ImVec4(0.40f, 0.70f, 1.00f, 1.00f);

    s.Colors[ImGuiCol_WindowBg]       = ImVec4(0.09f, 0.09f, 0.11f, 1.00f);
    s.Colors[ImGuiCol_PopupBg]        = ImVec4(0.12f, 0.12f, 0.14f, 0.98f);
    s.Colors[ImGuiCol_FrameBg]        = ImVec4(0.16f, 0.16f, 0.19f, 1.00f);
    s.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.22f, 0.26f, 1.00f);
    s.Colors[ImGuiCol_FrameBgActive]  = ImVec4(0.26f, 0.26f, 0.30f, 1.00f);
    s.Colors[ImGuiCol_SliderGrab]     = accent;
    s.Colors[ImGuiCol_SliderGrabActive] = accentHi;
    s.Colors[ImGuiCol_Button]         = ImVec4(0.20f, 0.20f, 0.24f, 1.00f);
    s.Colors[ImGuiCol_ButtonHovered]  = ImVec4(0.28f, 0.28f, 0.33f, 1.00f);
    s.Colors[ImGuiCol_ButtonActive]   = accent;
    s.Colors[ImGuiCol_Header]         = ImVec4(0.16f, 0.35f, 0.59f, 1.00f);
    s.Colors[ImGuiCol_HeaderHovered]  = accent;
    s.Colors[ImGuiCol_HeaderActive]   = accentHi;
    s.Colors[ImGuiCol_Text]           = ImVec4(0.92f, 0.92f, 0.95f, 1.00f);
    s.Colors[ImGuiCol_TextDisabled]   = ImVec4(0.50f, 0.50f, 0.55f, 1.00f);
    s.Colors[ImGuiCol_Separator]      = ImVec4(0.24f, 0.24f, 0.28f, 1.00f);
    s.Colors[ImGuiCol_CheckMark]      = accent;
    s.Colors[ImGuiCol_ScrollbarBg]    = ImVec4(0.09f, 0.09f, 0.11f, 1.00f);
    s.Colors[ImGuiCol_ScrollbarGrab]  = ImVec4(0.30f, 0.30f, 0.35f, 1.00f);
}

float amplitudeToDb(float amplitude) {
    if (amplitude <= 0.0f) return kMinDb;
    float db = 20.0f * std::log10(amplitude);
    if (db < kMinDb) db = kMinDb;
    if (db > kMaxDb) db = kMaxDb;
    return db;
}

float dbToAmplitude(float db) {
    if (db <= kMinDb) return 0.0f;
    return std::pow(10.0f, db / 20.0f);
}

void refreshDevices(GuiModel &m) {
    if (m.keepAlive) {
        m.devices = m.keepAlive->listOutputs();
    }
}

static std::string devicePreview(const GuiModel &m) {
    if (m.settings.deviceIndex < 0) return "Default";
    for (const auto &d : m.devices) {
        if (d.index == m.settings.deviceIndex) return d.name;
    }
    return "Device " + std::to_string(m.settings.deviceIndex);
}

// The device the stream is actually using: while running, the endpoint WASAPI
// resolved (which may differ from "Default"); otherwise the configured one.
static std::string connectedDeviceLabel(const GuiModel &m) {
    if (m.keepAlive && m.keepAlive->running()) {
        const std::string opened = m.keepAlive->lastOpenedDeviceName();
        if (!opened.empty()) return opened;
    }
    return devicePreview(m);
}

// ---------------------------------------------------------------------------
// Status bulb
//
// The header shows the app icon as a little lamp instead of a flat dot: dark and
// unlit while the app is inactive, and lit with the icon's cyan-to-blue badge,
// white wave and a soft halo while it keeps the speaker awake. The artwork
// mirrors assets/white_sound.ico (see tools/make_icon.ps1).
// ---------------------------------------------------------------------------

static ImVec4 lerpColor(const ImVec4 &a, const ImVec4 &b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

static ImU32 colorWithAlpha(const ImVec4 &c, float alphaMul) {
    return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alphaMul));
}

// The sine of the icon, drawn as a ribbon of constant vertical thickness: a
// stroked polyline would notch at the joins at this size. pMin is the top-left
// of the badge square the wave lives in.
static void addWaveRibbon(ImDrawList *dl, const ImVec2 &pMin, float size,
                          ImU32 col, float sc) {
    const float mid   = pMin.y + size * 0.5f;
    const float amp   = size * 0.175f;                          // one full cycle
    const float thick = (size * 0.105f > 2.0f * sc) ? size * 0.105f : 2.0f * sc;
    const float x0    = pMin.x + size * 0.205f;
    const float x1    = pMin.x + size * 0.795f;
    const int   steps = 14;
    ImVec2 prevTop, prevBot;
    for (int i = 0; i <= steps; i++) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float x = x0 + (x1 - x0) * t;
        const float y = mid - amp * sinf(6.2831853f * t);
        const ImVec2 top(x, y - thick * 0.5f);
        const ImVec2 bot(x, y + thick * 0.5f);
        if (i > 0) dl->AddQuadFilled(prevTop, top, bot, prevBot, col);
        prevTop = top;
        prevBot = bot;
    }
}

// Draws the lamp with its top-left corner at pMin. `lit` is the 0..1 ramp
// between the unlit LED and the fully lit icon, `pulse` the 0..1 breathe value.
static void drawStatusBulb(ImDrawList *dl, const ImVec2 &pMin, float size,
                           float lit, float pulse, float sc) {
    const ImVec2 pMax(pMin.x + size, pMin.y + size);
    const float rounding = size * 0.24f;

    // Unlit LED vs the icon's gradient colours.
    const ImVec4 offTop  = ImVec4(0.13f, 0.17f, 0.23f, 1.0f);
    const ImVec4 offBot  = ImVec4(0.09f, 0.12f, 0.17f, 1.0f);
    const ImVec4 offWave = ImVec4(0.42f, 0.47f, 0.57f, 1.0f);
    const ImVec4 onTop   = ImVec4(0.24f, 0.78f, 1.00f, 1.0f);   // #3EC8FF, as the .ico
    const ImVec4 onBot   = ImVec4(0.11f, 0.41f, 0.87f, 1.0f);   // #1C68DE
    const ImVec4 onWave  = ImVec4(1.00f, 1.00f, 1.00f, 1.0f);
    const ImVec4 halo    = ImVec4(0.26f, 0.59f, 0.98f, 1.0f);   // the panel accent

    // Halo: three rounded rects growing outwards, faded in by the ramp and
    // breathing gently while lit.
    if (lit > 0.01f) {
        const float spread[3] = {2.0f, 4.5f, 7.0f};
        const float alpha[3]  = {0.34f, 0.16f, 0.07f};
        const float breathe   = 0.75f + 0.25f * pulse;
        for (int i = 0; i < 3; i++) {
            const float s = spread[i] * sc;
            dl->AddRectFilled(ImVec2(pMin.x - s, pMin.y - s),
                              ImVec2(pMax.x + s, pMax.y + s),
                              colorWithAlpha(halo, alpha[i] * lit * breathe),
                              rounding + s * 0.7f);
        }
    }

    // Badge: mid colour plus a lighter top and a darker bottom band, which reads
    // as the icon's gradient (a true vertex gradient would need a custom path).
    const ImVec4 top = lerpColor(offTop, onTop, lit);
    const ImVec4 bot = lerpColor(offBot, onBot, lit);
    dl->AddRectFilled(pMin, pMax, colorWithAlpha(lerpColor(top, bot, 0.5f), 1.0f), rounding);
    dl->AddRectFilled(pMin, ImVec2(pMax.x, pMin.y + size * 0.52f),
                      colorWithAlpha(top, 0.45f), rounding, ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled(ImVec2(pMin.x, pMin.y + size * 0.48f), pMax,
                      colorWithAlpha(bot, 0.35f), rounding, ImDrawFlags_RoundCornersBottom);

    addWaveRibbon(dl, pMin, size, colorWithAlpha(lerpColor(offWave, onWave, lit), 1.0f), sc);
}

// A tiny title-bar button whose glyph is drawn by hand (the bundled bitmap font
// has no window glyphs). Returns true when clicked.
static bool titleBarButton(const char *id, const ImVec2 &pos, float w, float h,
                           float sc, bool closeGlyph, const char *tooltip) {
    ImGui::SetCursorScreenPos(pos);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    if (hovered) {
        const ImVec4 col = closeGlyph ? ImVec4(0.78f, 0.22f, 0.24f, 0.90f)
                                      : ImVec4(1.00f, 1.00f, 1.00f, 0.14f);
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h),
                          ImGui::GetColorU32(col), 5.0f * sc);
    }

    // Discreet by default so the glyphs do not draw the eye, but still readable;
    // they brighten up while hovered so the buttons stay easy to hit.
    const ImU32 fg = ImGui::GetColorU32(hovered ? ImVec4(0.86f, 0.86f, 0.91f, 1.0f)
                                                : ImVec4(0.45f, 0.45f, 0.50f, 1.0f));
    const ImVec2 c(pos.x + w * 0.5f, pos.y + h * 0.5f);
    const float s = 4.0f * sc;
    if (closeGlyph) {
        dl->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), fg, 1.5f * sc);
        dl->AddLine(ImVec2(c.x - s, c.y + s), ImVec2(c.x + s, c.y - s), fg, 1.5f * sc);
    } else {
        dl->AddLine(ImVec2(c.x - s, c.y), ImVec2(c.x + s, c.y), fg, 1.5f * sc);
    }

    if (hovered && tooltip) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

bool drawFlyout(GuiModel &m) {
    bool changed = false;
    const float sc = ImGui::GetIO().FontGlobalScale;

    // Requests from the previous frame are one-shot.
    m.requestHide = false;
    m.requestQuit = false;
    m.dragExcludes.clear();

    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    // Rounded background: on Windows 11 the OS also rounds the window frame
    // itself; elsewhere this still gives the panel soft corners over the darker
    // clear colour.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * sc);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("##ws_flyout", nullptr, flags);

    const float contentW = ImGui::GetContentRegionAvail().x;
    const float windowW = contentW + 2.0f * ImGui::GetStyle().WindowPadding.x;

    // ---- Title bar: minimize / close at the top right ----
    // The bar (plus the header below it) doubles as a title bar: the host turns
    // a click on it into a caption hit-test so the flyout can be dragged.
    const ImVec2 barPos = ImGui::GetCursorScreenPos();
    const float barBtnW = 26.0f * sc;
    const float barBtnH = 20.0f * sc;
    const float barBtnGap = 2.0f * sc;
    {
        const ImVec2 minPos(barPos.x + contentW - barBtnW * 2.0f - barBtnGap, barPos.y);
        const ImVec2 closePos(minPos.x + barBtnW + barBtnGap, barPos.y);

        if (titleBarButton("##ws_minimize", minPos, barBtnW, barBtnH, sc, false, "Minimize")) {
            m.requestHide = true;
        }
        m.dragExcludes.push_back(UiRect{minPos.x, minPos.y, barBtnW, barBtnH});

        if (titleBarButton("##ws_close", closePos, barBtnW, barBtnH, sc, true, "Close and exit")) {
            m.requestQuit = true;
        }
        m.dragExcludes.push_back(UiRect{closePos.x, closePos.y, barBtnW, barBtnH});
    }

    // ---- Header: status bulb + label + connected device + power toggle ----
    const ImVec2 headerPos = ImGui::GetCursorScreenPos();
    const float bulbSize = kBulbSize * sc;
    const float textIndent = bulbSize + kBulbGap * sc;
    float headerBottom = headerPos.y;
    {
        // Ramp the lamp towards the current state. The host resets bulbLit when
        // the panel is shown, so an active app visibly switches the bulb on.
        // DeltaTime is huge on the first frame after the panel was hidden (the
        // render loop stops while hidden), so it is clamped: the ramp then plays
        // out over real frames instead of jumping straight to the target.
        float dt = ImGui::GetIO().DeltaTime;
        if (dt > 1.0f / 30.0f) dt = 1.0f / 30.0f;
        if (dt < 0.0f) dt = 0.0f;
        const float target = m.enabled ? 1.0f : 0.0f;
        const float step = (kBulbRampSeconds > 0.0f) ? dt / kBulbRampSeconds : 1.0f;
        if (m.bulbLit < target) {
            m.bulbLit = (m.bulbLit + step < target) ? m.bulbLit + step : target;
        } else if (m.bulbLit > target) {
            m.bulbLit = (m.bulbLit - step > target) ? m.bulbLit - step : target;
        }
        m.bulbTime += dt;

        const float lineH = ImGui::GetTextLineHeight();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        // Centred on the status line; the header height is set by the power
        // button, so the lamp overhanging the text line does not move anything.
        drawStatusBulb(ImGui::GetWindowDrawList(),
                       ImVec2(p.x, p.y + lineH * 0.5f - bulbSize * 0.5f), bulbSize,
                       m.bulbLit,
                       0.5f + 0.5f * sinf(6.2831853f * m.bulbTime / kBulbBreathPeriod),
                       sc);

        ImGui::Dummy(ImVec2(bulbSize, lineH));
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + kBulbGap * sc);
        ImGui::TextUnformatted(m.enabled ? "Active" : "Inactive");

        const float btnW = 84.0f * sc;
        ImGui::SameLine(contentW - btnW);
        if (m.enabled) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.32f, 0.20f, 0.20f, 1.0f));
        if (ImGui::Button(m.enabled ? "Turn off" : "Turn on", ImVec2(btnW, 0.0f))) {
            m.enabled = !m.enabled;
            changed = true;
        }
        if (m.enabled) ImGui::PopStyleColor();

        const ImVec2 bMin = ImGui::GetItemRectMin();
        const ImVec2 bMax = ImGui::GetItemRectMax();
        m.dragExcludes.push_back(UiRect{bMin.x, bMin.y, bMax.x - bMin.x, bMax.y - bMin.y});

        // Device in use, right below the status text (placed explicitly so the
        // taller power button does not push it an extra item-spacing down).
        ImGui::SetCursorScreenPos(ImVec2(headerPos.x + textIndent, bMax.y + 2.0f * sc));
        ImGui::TextDisabled("%s", connectedDeviceLabel(m).c_str());

        headerBottom = ImGui::GetItemRectMax().y;
        if (bMax.y > headerBottom) headerBottom = bMax.y;
    }

    // The header (title bar + status block) is the drag handle, but only the
    // title-bar strip lights up on hover: the status text, the power button and
    // the device line keep their normal look.
    m.dragArea = UiRect{0.0f, 0.0f, windowW, headerBottom + 2.0f * sc};
    const UiRect barRect{0.0f, 0.0f, windowW, barPos.y + barBtnH + 2.0f * sc};
    if (barRect.valid() &&
        ImGui::IsMouseHoveringRect(ImVec2(barRect.x, barRect.y),
                                   ImVec2(barRect.right(), barRect.bottom())) &&
        !inAnyRect(m.dragExcludes, ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y)) {
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(barRect.x, barRect.y),
            ImVec2(barRect.right(), barRect.bottom()),
            ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.05f)), 8.0f * sc);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- Device selector ----
    ImGui::TextDisabled("Device");
    ImGui::SetNextItemWidth(contentW);
    const std::string preview = devicePreview(m);
    if (ImGui::BeginCombo("##device", preview.c_str())) {
        const bool isDefault = (m.settings.deviceIndex < 0);
        if (ImGui::Selectable("Default", isDefault)) {
            m.settings.deviceIndex = -1;
            changed = true;
        }
        if (isDefault) ImGui::SetItemDefaultFocus();
        for (const auto &d : m.devices) {
            const bool sel = (m.settings.deviceIndex == d.index);
            const std::string label = d.name + "  (" + d.hostapi + ")";
            if (ImGui::Selectable(label.c_str(), sel)) {
                m.settings.deviceIndex = d.index;
                changed = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (m.keepAlive && m.keepAlive->running()) {
        const int rate = m.keepAlive->lastSampleRate();
        if (rate > 0) {
            ImGui::TextDisabled("Opened at %d Hz", rate);
        }
    }

    ImGui::Spacing();

    // ---- Volume (perceptual dBFS) ----
    float db = amplitudeToDb(m.settings.level);
    ImGui::TextDisabled("Volume");
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f dBFS", db);
        const float tw = ImGui::CalcTextSize(buf).x;
        ImGui::SameLine(contentW - tw);
        ImGui::TextUnformatted(buf);
    }
    ImGui::SetNextItemWidth(contentW);
    bool volumeChanged = ImGui::SliderFloat("##volume", &db, kMinDb, kMaxDb, "");
    // The wheel nudges the volume too while the pointer is over the slider.
    if (ImGui::IsItemHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            db += wheel * kWheelDbPerNotch;
            if (db < kMinDb) db = kMinDb;
            if (db > kMaxDb) db = kMaxDb;
            volumeChanged = true;
        }
    }
    if (volumeChanged) {
        m.settings.level = dbToAmplitude(db);
        changed = true;
    }
    if (db < kAdviceMinDb) {
        ImGui::TextColored(ImVec4(1.00f, 0.62f, 0.25f, 1.0f),
                           "Level too low: may cut out");
    }

    // ---- Noise type ----
    ImGui::TextDisabled("Noise type");
    ImGui::SetNextItemWidth(contentW);
    {
        const char *noiseItems[] = {"White (recommended)", "Pink", "Burst (intermittent)"};
        int noiseMode = m.settings.noiseMode;
        if (noiseMode < 0 || noiseMode > 2) noiseMode = 0;
        if (ImGui::Combo("##noise", &noiseMode, noiseItems, 3)) {
            m.settings.noiseMode = noiseMode;
            changed = true;
        }
    }
    if (ImGui::IsItemHovered()) {
        // The panel is narrow: a hard-wrapped hint would be wider than the
        // window and get cut off on the right, so let ImGui soft-wrap it.
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * kWrapChars);
        ImGui::TextUnformatted("White: even energy across the whole spectrum (best for "
                               "small BT speakers).\n"
                               "Pink: more energy in the low end.\n"
                               "Burst: a short noise pulse every few seconds (useful when "
                               "continuous noise does not stop the speaker from sleeping).");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    // ---- Burst parameters ----
    if (m.settings.noiseMode == 2) {
        ImGui::SetNextItemWidth(contentW);
        if (ImGui::SliderInt("##burstdur", &m.settings.burstDurationMs, 20, 500, "Length: %d ms")) {
            changed = true;
        }
        ImGui::SetNextItemWidth(contentW);
        if (ImGui::SliderInt("##burstint", &m.settings.burstIntervalMs, 200, 10000, "Every: %d ms")) {
            changed = true;
        }
    }

    // ---- Auto-tune: raise the level automatically on drop-outs ----
    if (ImGui::Checkbox("Auto-tune", &m.settings.autoTune)) {
        changed = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * kWrapChars);
        ImGui::TextUnformatted("Raises the level automatically when drop-outs are detected, "
                               "and brings it down slowly while stable.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    if (m.settings.autoTune && m.keepAlive) {
        const float eff = m.keepAlive->effectiveLevel();
        if (std::fabs(eff - m.settings.level) > 1e-4f) {
            ImGui::SameLine();
            ImGui::TextDisabled("Auto: %.0f dBFS", amplitudeToDb(eff));
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
    return changed;
}

} // namespace gui
} // namespace ws
