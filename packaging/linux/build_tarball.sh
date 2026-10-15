#!/bin/sh
# Build White Sound and package it as a portable Linux tarball.
#
#   sh packaging/linux/build_tarball.sh [build-dir]
#
# Output: dist/white-sound-<version>-linux-<arch>.tar.gz
# The tarball ships bin/, share/ and install.sh; installing needs no root.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
build_dir=${1:-build}

gen=""
if command -v ninja >/dev/null 2>&1; then
    gen="-G Ninja"
fi

version=$(sed -n 's/^project(white_sound VERSION \([0-9][0-9.]*\).*/\1/p' \
    "$root/CMakeLists.txt" | head -n 1)
if [ -z "$version" ]; then
    echo "could not read the version from CMakeLists.txt" >&2
    exit 1
fi
arch=$(uname -m)

echo "==> Building White Sound $version"
# shellcheck disable=SC2086  # $gen is empty or "-G Ninja"
cmake $gen -B "$root/$build_dir" -S "$root" -DCMAKE_BUILD_TYPE=Release
cmake --build "$root/$build_dir" --parallel

bin="$root/$build_dir/white_sound_tray"
if [ ! -f "$bin" ]; then
    echo "the build did not produce $bin" >&2
    exit 1
fi

stage="$root/dist/white-sound-$version-linux-$arch"
echo "==> Staging $stage"
rm -rf "$stage"
mkdir -p "$stage/bin" \
         "$stage/share/applications" \
         "$stage/share/icons/hicolor/256x256/apps" \
         "$stage/share/doc/white-sound"

cp "$bin" "$stage/bin/white_sound_tray"
chmod 755 "$stage/bin/white_sound_tray"
cp "$here/white-sound.desktop" "$stage/share/applications/white-sound.desktop"
cp "$root/assets/white_sound.png"     "$stage/share/icons/hicolor/256x256/apps/white-sound.png"
cp "$root/assets/white_sound_off.png" "$stage/share/icons/hicolor/256x256/apps/white-sound-off.png"
for doc in README.md LICENSE THIRD_PARTY_NOTICES.md; do
    cp "$root/$doc" "$stage/share/doc/white-sound/$doc"
done
cp "$here/install.sh" "$stage/install.sh"
chmod 755 "$stage/install.sh"

tarball="$root/dist/white-sound-$version-linux-$arch.tar.gz"
echo "==> Creating $(basename "$tarball")"
tar -C "$root/dist" -czf "$tarball" "white-sound-$version-linux-$arch"

echo
echo "==> Done: $tarball"
echo "    Install with:"
echo "        tar xzf $(basename "$tarball")"
echo "        sh white-sound-$version-linux-$arch/install.sh"
