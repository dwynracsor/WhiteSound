#pragma once

#include <filesystem>

namespace ws {
namespace platform {

std::filesystem::path configDir();

std::filesystem::path configFile();

std::filesystem::path logFile();

} // namespace platform
} // namespace ws
