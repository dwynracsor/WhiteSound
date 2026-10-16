#!/bin/sh
# Build the Linux installer (portable tarball) from inside WSL (Ubuntu).
#
#   sh wsl_build.sh /mnt/c/path/to/white-sound
#
# Must run as root (WSL's default user before a distro account is created), so
# it can apt-get the build dependencies. Output: <repo>/installers/*.tar.gz,
# which is the Windows folder, so the artifact is immediately available.
set -eu

root=${1:?usage: wsl_build.sh <repo-dir>}
cd "$root"

export DEBIAN_FRONTEND=noninteractive
echo "==> Installing build dependencies"
apt-get update
apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config \
    xorg-dev libwayland-dev libxkbcommon-dev libgl1-mesa-dev \
    libdbus-1-dev libasound2-dev libpulse-dev

echo "==> Building"
sh installers/linux/build_tarball.sh build-linux
