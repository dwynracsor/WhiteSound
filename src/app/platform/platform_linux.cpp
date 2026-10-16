#include "platform.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <vector>

#include "core/paths.h"

extern char **environ;

namespace ws {
namespace platform {
namespace {

int g_lockFd = -1;

bool trySpawn(const char *program, std::initializer_list<std::string> args) {
    std::vector<std::string> store(args);
    std::vector<char *> argv;
    argv.reserve(store.size() + 1);
    for (std::string &s : store) argv.push_back(const_cast<char *>(s.c_str()));
    argv.push_back(nullptr);

    pid_t pid = 0;
    if (posix_spawnp(&pid, program, nullptr, nullptr, argv.data(), environ) != 0) {
        return false;
    }

    int status = 0;
    pid_t r;
    do {
        r = waitpid(pid, &status, 0);
    } while (r < 0 && errno == EINTR);
    if (r < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

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
    if (trySpawn("zenity", {"zenity", error ? "--error" : "--info", "--title",
                            title, "--text", body})) {
        return;
    }
    if (trySpawn("kdialog", {"kdialog", "--title", title,
                             error ? "--error" : "--msgbox", body})) {
        return;
    }

    std::fprintf(stderr, "%s: %s\n", title.c_str(), body.c_str());
}

} // namespace platform
} // namespace ws
