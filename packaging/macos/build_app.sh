#!/bin/sh
# Build White Sound and assemble "White Sound.app" plus a .dmg for macOS.
#
#   sh packaging/macos/build_app.sh [build-dir]
#
# Output: dist/White Sound.app and dist/White Sound-<version>.dmg
#
# The app is not code-signed (or only ad-hoc signed), so the first launch needs
# a right-click > Open, or System Settings > Privacy & Security > Open Anyway.
set -eu

if [ "$(uname -s)" != "Darwin" ]; then
    echo "this script is for macOS" >&2
    exit 1
fi

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

echo "==> Building White Sound $version"
# shellcheck disable=SC2086  # $gen is empty or "-G Ninja"
cmake $gen -B "$root/$build_dir" -S "$root" -DCMAKE_BUILD_TYPE=Release
cmake --build "$root/$build_dir" --parallel

bin="$root/$build_dir/white_sound_tray"
if [ ! -f "$bin" ]; then
    echo "the build did not produce $bin" >&2
    exit 1
fi

app="$root/dist/White Sound.app"
echo "==> Assembling $(basename "$app")"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"

cp "$bin" "$app/Contents/MacOS/white_sound_tray"
chmod 755 "$app/Contents/MacOS/white_sound_tray"

# The tray looks for the art next to the executable, then in ../Resources
# (see imageFor in src/app/platform/tray_macos.mm).
cp "$root/assets/white_sound.png"     "$app/Contents/Resources/white_sound.png"
cp "$root/assets/white_sound_off.png" "$app/Contents/Resources/white_sound_off.png"

sed "s|@WS_VERSION@|$version|g" "$here/Info.plist.in" > "$app/Contents/Info.plist"

# Finder/DMG icon, built from the PNG with the system tools. Best effort: if it
# fails, Finder just shows the generic application icon.
if command -v sips >/dev/null 2>&1 && command -v iconutil >/dev/null 2>&1; then
    tmp=$(mktemp -d)
    iconset="$tmp/white_sound.iconset"
    mkdir -p "$iconset"
    for s in 16 32 128 256 512; do
        sips -z "$s" "$s" "$root/assets/white_sound.png" \
            --out "$iconset/icon_${s}x${s}.png" >/dev/null
        sips -z "$((s * 2))" "$((s * 2))" "$root/assets/white_sound.png" \
            --out "$iconset/icon_${s}x${s}@2x.png" >/dev/null
    done
    if ! iconutil -c icns "$iconset" -o "$app/Contents/Resources/white_sound.icns"; then
        echo "note: could not build the .icns; Finder will use the generic icon"
    fi
    rm -rf "$tmp"
fi

# Ad-hoc signature: enough to run locally, still "unidentified" to Gatekeeper.
if command -v codesign >/dev/null 2>&1; then
    if ! codesign --force --sign - "$app" >/dev/null 2>&1; then
        echo "note: ad-hoc codesign skipped"
    fi
fi

dmg="$root/dist/White Sound-$version.dmg"
if command -v hdiutil >/dev/null 2>&1; then
    echo "==> Creating $(basename "$dmg")"
    stage=$(mktemp -d)
    cp -R "$app" "$stage/"
    ln -s /Applications "$stage/Applications"
    rm -f "$dmg"
    hdiutil create -volname "White Sound" -srcfolder "$stage" \
        -ov -format UDZO "$dmg" >/dev/null
    rm -rf "$stage"
fi

echo
echo "==> Done:"
echo "    $app"
if [ -f "$dmg" ]; then
    echo "    $dmg"
fi
echo
echo "    First launch: right-click the app > Open (it is not code-signed)."
