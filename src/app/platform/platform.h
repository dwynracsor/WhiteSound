#pragma once

#include <string>

namespace ws {
namespace platform {

// Process-level platform services shared by the entry points. The tray and the
// window host are separate (see tray.h and app/gui/flyout.h); these are the few
// things that belong to neither.

// Enforces a single running instance: a second instance would open another
// stream to the same endpoint and the two would fight over the device.
// Returns true when this process is the only instance; on failure it errs on
// the side of allowing startup. The lock is held for the process lifetime.
bool acquireSingleInstance();

// Opts into per-monitor DPI awareness before any window or icon is created.
// Windows only; a no-op elsewhere. (The GLFW flyout host opts in as well;
// doing it here keeps the tray icon crisp even if the flyout fails.)
void initProcessDpi();

// Shows a native modal message dialog. `error` selects the error style when the
// platform distinguishes them.
void showMessage(const std::string &title, const std::string &body, bool error);

} // namespace platform
} // namespace ws
