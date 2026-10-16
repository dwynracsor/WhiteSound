#pragma once

#include <string>

namespace ws {
namespace platform {


bool acquireSingleInstance();

void initProcessDpi();

void showMessage(const std::string &title, const std::string &body, bool error);

} // namespace platform
} // namespace ws
