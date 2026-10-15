// Windows entry point: just the platform bootstrapping, then the portable shell.

#include <windows.h>

#include "app/platform/platform.h"
#include "app/shell/shell.h"

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // Before any window or icon exists, so sizes are reported at the real DPI.
    ws::platform::initProcessDpi();

    if (!ws::platform::acquireSingleInstance()) {
        ws::platform::showMessage("White Sound", "White Sound is already running.",
                                  false);
        return 0;
    }

    return ws::shell::run();
}
