// Linux process-level services: single instance (flock) and a best-effort
// native dialog through zenity/kdialog, falling back to stderr.

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

// Held for the process lifetime; the kernel drops it on exit.
int g_lockFd = -1;

// Runs a helper (zenity/kdialog) without a shell, so the text is never
// interpreted. Returns true only when the helper ran and exited successfully,
// so a present-but-failing dialog falls through to the next one.
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
    } while (r < 0 && errno == EINTR);   // retry only on EINTR; never spin
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
    if (g_lockFd < 0) return true;   // cannot lock: assume we are alone
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
