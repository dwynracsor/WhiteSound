// macOS system tray backend: an NSStatusItem with an NSMenu, plus a global
// toggle hotkey registered through Carbon's RegisterEventHotKey (the modern
// alternative, NSEvent global monitors, needs accessibility permission for key
// events; the Carbon call does not).
//
// Compiled as Objective-C++ (no ARC: the objects are retained explicitly and
// released in destroy()).

#include "tray.h"

#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>

#include <mach-o/dyld.h>

#include <cstring>
#include <string>

namespace ws {
namespace platform {
namespace {

// NSMenuItem tags -> TrayCommand.
enum {
    kTagOpen = 1,
    kTagEnable,
    kTagConfig,
    kTagAbout,
    kTagQuit,
};

// Directory that holds the running executable, used to find the tray art next
// to it (a plain build) or in the bundle / source tree (a .app or a dev build).
std::string executableDir() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size == 0) return std::string();
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return std::string();
    buf.resize(std::strlen(buf.c_str()));
    const size_t slash = buf.find_last_of('/');
    return slash == std::string::npos ? std::string() : buf.substr(0, slash);
}

} // namespace
} // namespace platform
} // namespace ws

// Objective-C target for the status button and the menu items; it forwards to
// the owning C++ ws::platform::MacTray through `owner`.
@interface WSTrayTarget : NSObject {
@public
    void *owner;
}
- (void)onMenuItem:(id)sender;
- (void)onStatusButton:(id)sender;
@end

namespace ws {
namespace platform {

class MacTray : public Tray {
public:
    ~MacTray() override { destroy(); }

    bool create(const TrayCallbacks &callbacks) override;
    void destroy() override;
    void setEnabled(bool enabled) override;
    void showMenu() override;
    void notify(const std::string &title, const std::string &body) override;
    void pump() override {}   // Cocoa events arrive through GLFW's event pump
    bool hotkeyAvailable() const override { return m_hotkeyOk; }

    // Called back from the Objective-C target / the hotkey handler.
    void dispatchCommand(int tag);
    void dispatchActivate();
    void dispatchHotkey();

private:
    void buildMenu();
    bool installHotkey();
    NSImage *imageFor(bool on) const;

    TrayCallbacks m_cb;
    NSStatusItem *m_status = nil;
    NSMenu *m_menu = nil;
    WSTrayTarget *m_target = nil;
    NSImage *m_iconOn = nil;
    NSImage *m_iconOff = nil;
    EventHotKeyRef m_hotkeyRef = nullptr;
    EventHandlerRef m_hotkeyHandler = nullptr;
    bool m_enabled = true;
    bool m_hotkeyOk = false;
};

namespace {

OSStatus wsHotkeyHandler(EventHandlerCallRef, EventRef, void *userData) {
    auto *tray = static_cast<MacTray *>(userData);
    if (tray) tray->dispatchHotkey();
    return noErr;
}

void addMenuItem(NSMenu *menu, NSString *title, id target, NSInteger tag) {
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title
                                                  action:@selector(onMenuItem:)
                                           keyEquivalent:@""];
    item.target = target;
    item.tag = tag;
    [menu addItem:item];
    [item release];
}

} // namespace

} // namespace platform
} // namespace ws

@implementation WSTrayTarget

- (void)onMenuItem:(id)sender {
    if (!owner) return;
    static_cast<ws::platform::MacTray *>(owner)->dispatchCommand((int)[sender tag]);
}

- (void)onStatusButton:(id)sender {
    if (!owner) return;
    auto *tray = static_cast<ws::platform::MacTray *>(owner);
    NSEvent *event = [NSApp currentEvent];
    if (event && (event.type == NSEventTypeRightMouseUp ||
                  (event.modifierFlags & NSEventModifierFlagControl))) {
        tray->showMenu();
    } else {
        tray->dispatchActivate();
    }
}

@end

namespace ws {
namespace platform {

bool MacTray::create(const TrayCallbacks &callbacks) {
    m_cb = callbacks;

    @autoreleasepool {
        [NSApplication sharedApplication];
        // Tray-only application: no Dock icon and no application menu.
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        m_target = [[WSTrayTarget alloc] init];
        m_target->owner = this;

        m_iconOn = imageFor(true);
        m_iconOff = imageFor(false);

        NSStatusBar *bar = [NSStatusBar systemStatusBar];
        m_status = [[bar statusItemWithLength:NSSquareStatusItemLength] retain];
        m_status.button.target = m_target;
        m_status.button.action = @selector(onStatusButton:);
        [m_status.button sendActionOn:(NSEventMaskLeftMouseUp |
                                       NSEventMaskRightMouseUp)];
        m_status.button.image = m_iconOn;
        m_status.button.toolTip = @"White Sound";

        buildMenu();
        m_hotkeyOk = installHotkey();
    }

    return m_status != nil;
}

void MacTray::destroy() {
    if (m_hotkeyRef) {
        UnregisterEventHotKey(m_hotkeyRef);
        m_hotkeyRef = nullptr;
    }
    if (m_hotkeyHandler) {
        RemoveEventHandler(m_hotkeyHandler);
        m_hotkeyHandler = nullptr;
    }
    m_hotkeyOk = false;

    @autoreleasepool {
        if (m_status) {
            [[NSStatusBar systemStatusBar] removeStatusItem:m_status];
            [m_status release];
            m_status = nil;
        }
        if (m_menu) { [m_menu release]; m_menu = nil; }
        if (m_target) { [m_target release]; m_target = nil; }
        if (m_iconOn) { [m_iconOn release]; m_iconOn = nil; }
        if (m_iconOff) { [m_iconOff release]; m_iconOff = nil; }
    }
}

void MacTray::setEnabled(bool enabled) {
    m_enabled = enabled;
    if (m_status) {
        m_status.button.image = enabled ? m_iconOn : m_iconOff;
        m_status.button.toolTip = enabled ? @"White Sound - Active"
                                          : @"White Sound - Inactive";
    }
    if (m_menu) {
        NSMenuItem *item = [m_menu itemWithTag:kTagEnable];
        if (item) {
            item.state = enabled ? NSControlStateValueOn : NSControlStateValueOff;
        }
    }
}

void MacTray::showMenu() {
    if (!m_menu) return;
    // Deprecated in name only: it is still the call that positions a status
    // item's menu correctly. (popUpMenuPositioningItem: is the modern spelling.)
    if (m_status) {
        [m_status popUpStatusItemMenu:m_menu];
    }
}

void MacTray::notify(const std::string &, const std::string &) {
    // macOS has no lightweight tray balloon: notifications would need the
    // UserNotifications framework and a permission prompt. The first-run hint is
    // therefore omitted here; the panel is reachable from the status item.
}

void MacTray::dispatchCommand(int tag) {
    if (!m_cb.onCommand) return;
    switch (tag) {
        case kTagOpen:   m_cb.onCommand(TrayCommand::OpenPanel); break;
        case kTagEnable: m_cb.onCommand(TrayCommand::ToggleEnabled); break;
        case kTagConfig: m_cb.onCommand(TrayCommand::ShowConfig); break;
        case kTagAbout:  m_cb.onCommand(TrayCommand::ShowAbout); break;
        case kTagQuit:   m_cb.onCommand(TrayCommand::Quit); break;
        default: break;
    }
}

void MacTray::dispatchActivate() {
    if (m_cb.onActivate) m_cb.onActivate();
}

void MacTray::dispatchHotkey() {
    if (m_cb.onHotkey) m_cb.onHotkey();
}

void MacTray::buildMenu() {
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@"White Sound"];
    [menu setAutoenablesItems:NO];

    addMenuItem(menu, @"Open panel", m_target, kTagOpen);
    [menu addItem:[NSMenuItem separatorItem]];
    addMenuItem(menu, @"Enabled", m_target, kTagEnable);
    [menu addItem:[NSMenuItem separatorItem]];
    addMenuItem(menu, @"Configuration...", m_target, kTagConfig);
    addMenuItem(menu, @"About...", m_target, kTagAbout);
    [menu addItem:[NSMenuItem separatorItem]];
    addMenuItem(menu, @"Quit", m_target, kTagQuit);

    m_menu = menu;
}

bool MacTray::installHotkey() {
    EventTypeSpec spec = { kEventClassKeyboard, kEventHotKeyPressed };
    if (InstallApplicationEventHandler(&wsHotkeyHandler, 1, &spec, this,
                                       &m_hotkeyHandler) != noErr) {
        m_hotkeyHandler = nullptr;
        return false;
    }

    EventHotKeyID id = { 'WHSD', 1 };
    if (RegisterEventHotKey(kVK_ANSI_W, controlKey | optionKey, id,
                            GetApplicationEventTarget(), 0,
                            &m_hotkeyRef) != noErr) {
        RemoveEventHandler(m_hotkeyHandler);
        m_hotkeyHandler = nullptr;
        m_hotkeyRef = nullptr;
        return false;
    }
    return true;
}

NSImage *MacTray::imageFor(bool on) const {
    const char *file = on ? "white_sound.png" : "white_sound_off.png";
    NSImage *image = nil;

    const std::string dir = executableDir();
    if (!dir.empty()) {
        NSString *base = [NSString stringWithUTF8String:dir.c_str()];
        NSString *name = [NSString stringWithUTF8String:file];
        NSMutableArray<NSString *> *dirs = [NSMutableArray array];
        [dirs addObject:base];
        [dirs addObject:[base stringByAppendingPathComponent:@"../Resources"]];
        [dirs addObject:[base stringByAppendingPathComponent:@"../assets"]];
        [dirs addObject:[base stringByAppendingPathComponent:@"assets"]];

        NSFileManager *fm = [NSFileManager defaultManager];
        for (NSString *d in dirs) {
            NSString *path =
                [[d stringByAppendingPathComponent:name] stringByStandardizingPath];
            if ([fm fileExistsAtPath:path]) {
                image = [[NSImage alloc] initWithContentsOfFile:path];
                if (image) break;
            }
        }
    }

    if (!image) {
        // No art found next to the executable: fall back to a system symbol so
        // the status item is never blank.
        if (@available(macOS 11.0, *)) {
            image = [[NSImage imageWithSystemSymbolName:@"waveform"
                               accessibilityDescription:@"White Sound"] retain];
        }
        if (image) image.template = YES;
    }

    if (image) image.size = NSMakeSize(18.0, 18.0);
    return image;
}

std::unique_ptr<Tray> createTray() { return std::make_unique<MacTray>(); }

} // namespace platform
} // namespace ws
