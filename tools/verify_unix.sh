#!/bin/sh
# Build and unit-test White Sound on macOS/Linux.
#
#   sh tools/verify_unix.sh [build-dir]
#
# Configures with the unit tests enabled, builds everything and runs the tests.
# The audio engine and the tray need a real desktop session, so those are checked
# by running the built binary afterwards (see README, "Verifying on macOS/Linux").
set -eu

build_dir=${1:-build-verify}
os=$(uname -s)

echo "==> White Sound: verifying on $os (build dir: $build_dir)"

if ! command -v cmake >/dev/null 2>&1; then
    echo "cmake not found; install CMake >= 3.20" >&2
    exit 1
fi

# Prefer Ninja when it is installed; otherwise let CMake pick the default.
gen=""
if command -v ninja >/dev/null 2>&1; then
    gen="-G Ninja"
fi

echo "==> Configuring"
# shellcheck disable=SC2086  # $gen is an empty string or "-G Ninja"
if ! cmake $gen -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DWS_BUILD_TESTS=ON; then
    echo >&2
    echo "Configure failed. Missing development packages are the usual cause." >&2
    if [ "$os" = "Linux" ]; then
        echo >&2
        echo "On Debian/Ubuntu:" >&2
        echo "  sudo apt install build-essential cmake ninja-build pkgconf \\" >&2
        echo "      xorg-dev libwayland-dev libxkbcommon-dev libgl1-mesa-dev \\" >&2
        echo "      libdbus-1-dev libasound2-dev libpulse-dev" >&2
        echo >&2
        echo "On Fedora: gcc-c++ cmake ninja-build pkgconf-pkg-config" >&2
        echo "  libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel \\" >&2
        echo "  libXi-devel libXext-devel wayland-devel libxkbcommon-devel \\" >&2
        echo "  mesa-libGL-devel dbus-devel alsa-lib-devel" >&2
        echo >&2
        echo "No Wayland available? re-run with -DGLFW_BUILD_WAYLAND=OFF." >&2
    fi
    exit 1
fi

echo "==> Building"
cmake --build "$build_dir" --parallel

echo "==> Running unit tests"
ctest --test-dir "$build_dir" --output-on-failure

binary="$build_dir/white_sound_tray"
echo
echo "==> Build and tests OK: $binary"
echo "    Launch it to check the tray icon, the panel and the audio:"
echo
echo "        $binary"
echo
echo "    (a real run needs a desktop session with a tray host and an audio device)"
