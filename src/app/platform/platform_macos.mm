#include "platform.h"

#import <Cocoa/Cocoa.h>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <filesystem>

#include "core/paths.h"

namespace ws {
namespace platform {
namespace {

int g_lockFd = -1;

} // namespace

bool acquireSingleInstance() {
    const std::filesystem::path dir = configDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    const std::filesystem::path lockPath = dir / "ws.lock";
    g_lockFd = ::open(lockPath.c_str(), O_CREAT | O_RDWR, 0600);
    if (g_lockFd < 0) return true;
    if (::flock(g_lockFd, LOCK_EX | LOCK_NB) != 0) {
        ::close(g_lockFd);
        g_lockFd = -1;
        return false;
    }
    return true;
}

void initProcessDpi() {}

void showMessage(const std::string &title, const std::string &body, bool error) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = [NSString stringWithUTF8String:title.c_str()];
        alert.informativeText = [NSString stringWithUTF8String:body.c_str()];
        [alert addButtonWithTitle:@"OK"];
        alert.alertStyle = error ? NSAlertStyleCritical : NSAlertStyleInformational;
        [alert runModal];
        [alert release];
    }
}

} // namespace platform
} // namespace ws
