#pragma once

namespace ws {
namespace shell {

// Runs the whole application: it wires the portable controller to the window
// host (ws::gui::Flyout) and the native tray (ws::platform::Tray), and blocks
// until the user quits. Must be called on the main thread (the window and tray
// hosts both require it). Returns the process exit code.
int run();

} // namespace shell
} // namespace ws
