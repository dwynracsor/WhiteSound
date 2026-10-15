#pragma once

#include <functional>
#include <memory>
#include <string>

namespace ws {
namespace platform {

// Actions the tray context menu offers. The shell executes them; the tray only
// reports intent, so this list is shared by every OS backend.
enum class TrayCommand {
    OpenPanel,      // show/hide the flyout
    ToggleEnabled,  // start/stop emitting
    ShowConfig,     // configuration summary dialog
    ShowAbout,      // about dialog
    Quit,           // stop and exit
};

// Everything the tray asks the shell to do. Any callable may be left empty; a
// backend must then simply do nothing for that event.
struct TrayCallbacks {
    // A context-menu entry was chosen.
    std::function<void(TrayCommand)> onCommand;
    // The icon was activated with the primary button (left click / tap).
    std::function<void()> onActivate;
    // The global toggle hotkey (Ctrl+Alt+W) was pressed.
    std::function<void()> onHotkey;
};

// Native system-tray icon, one implementation per OS:
//
//   Windows  Shell_NotifyIcon on a hidden window; the same window owns the
//            global hotkey, so tray clicks and the hotkey arrive as messages
//   macOS    NSStatusItem with an NSMenu, plus a Carbon global hotkey
//   Linux    StatusNotifierItem (org.kde.StatusNotifierItem) with a
//            com.canonical.dbusmenu context menu, over the session D-Bus
//
// The tray owns its platform event source. Windows delivers events through the
// normal thread message pump (the flyout's pumpEvents already dispatches them),
// macOS through the Cocoa run loop GLFW drives, so pump() is a no-op there;
// Linux services its D-Bus connection from pump(), which the shell must call
// once per loop iteration.
class Tray {
public:
    virtual ~Tray() = default;

    // Creates the tray icon. Returns false when the platform tray is
    // unavailable (e.g. no D-Bus session bus); the shell then runs without a
    // tray icon (the hotkey still works where the OS registered one).
    virtual bool create(const TrayCallbacks &callbacks) = 0;
    // Removes the icon and releases the platform resources. Idempotent: the shell
    // calls it once at shutdown and the destructor calls it again.
    virtual void destroy() = 0;

    // Reflects the enabled state in the icon, the tooltip and the menu check.
    virtual void setEnabled(bool enabled) = 0;

    // Pops the context menu at the current pointer. Used by the shell's
    // tray-only fallback (no flyout). A no-op where the desktop shows the menu
    // on its own (macOS, Linux).
    virtual void showMenu() = 0;

    // First-run balloon / notification.
    virtual void notify(const std::string &title, const std::string &body) = 0;

    // Services platform events that are not part of the window event pump.
    virtual void pump() = 0;

    // Whether the global toggle hotkey is actually registered. Registration
    // fails when another application already owns the combination (very common
    // for Ctrl+Alt+W), so the shell reports it instead of silently doing
    // nothing. Backends without a global hotkey return false.
    virtual bool hotkeyAvailable() const { return false; }
};

// Creates the tray for the compiled platform.
std::unique_ptr<Tray> createTray();

} // namespace platform
} // namespace ws
