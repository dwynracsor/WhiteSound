// Linux entry point: platform bootstrapping, then the portable shell.

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
