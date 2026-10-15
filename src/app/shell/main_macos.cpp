// macOS entry point. GLFW (inside the flyout host) sets the NSApplication up,
// so a plain main() is enough; the tray attaches to the same application.

#include "app/platform/platform.h"
#include "app/shell/shell.h"

int main(int, char **) {
    if (!ws::platform::acquireSingleInstance()) {
        ws::platform::showMessage("White Sound", "White Sound is already running.",
                                  false);
        return 0;
    }

    return ws::shell::run();
}
