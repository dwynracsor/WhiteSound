#pragma once

#include <filesystem>

namespace ws {
namespace platform {

// Per-user directory that holds the configuration and the diagnostic log.
// Located per OS:
//   Windows  %APPDATA%\white-sound
//   macOS    ~/Library/Application Support/white-sound
//   Linux    $XDG_CONFIG_HOME/white-sound  (else ~/.config/white-sound)
// Falls back to the current directory when the environment is unavailable.
// The directory is NOT created here; callers that write do so themselves.
std::filesystem::path configDir();

// configDir()/config.json
std::filesystem::path configFile();

// configDir()/ws.log
std::filesystem::path logFile();

} // namespace platform
} // namespace ws
