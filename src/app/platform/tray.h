#pragma once

#include <functional>
#include <memory>
#include <string>

namespace ws {
namespace platform {

enum class TrayCommand {
    OpenPanel,
    ToggleEnabled,
    ShowConfig,
    ShowAbout,
    Quit,
};

struct TrayCallbacks {
    std::function<void(TrayCommand)> onCommand;
    std::function<void()> onActivate;
    std::function<void()> onHotkey;
    std::function<void()> onClose;
};

class Tray {
public:
    virtual ~Tray() = default;

    virtual bool create(const TrayCallbacks &callbacks) = 0;
    virtual void destroy() = 0;

    virtual void setEnabled(bool enabled) = 0;

    virtual void setTaskbarVisible(bool visible) { (void)visible; }

    virtual void showMenu() = 0;

    virtual void notify(const std::string &title, const std::string &body) = 0;

    virtual void pump() = 0;

    virtual bool hotkeyAvailable() const { return false; }
};

std::unique_ptr<Tray> createTray();

} // namespace platform
} // namespace ws
