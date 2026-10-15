#!/bin/sh
# Install White Sound from the extracted tarball.
#
#   sh install.sh [--prefix DIR] [--system] [--autostart] [--uninstall] [--help]
#
# By default everything goes under ~/.local (binary in ~/.local/bin, the desktop
# entry and icons in ~/.local/share) and no root is needed. --system installs
# under /usr/local and usually needs sudo. The tarball is self-contained: this
# script plus bin/ and share/.
set -eu

name="white-sound"
binary="white_sound_tray"

prefix="$HOME/.local"
autostart=no
uninstall=no

usage() {
    cat <<'EOF'
Usage: sh install.sh [options]

  --prefix DIR   install under DIR (default: ~/.local)
  --system       shorthand for --prefix /usr/local (may need sudo)
  --autostart    also start White Sound when you sign in
  --uninstall    remove a previous install (use the same --prefix/--system)
  --help         show this help
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)
            [ $# -ge 2 ] || { echo "--prefix needs a directory" >&2; exit 2; }
            prefix=$2
            shift 2
            ;;
        --system)    prefix=/usr/local; shift ;;
        --autostart) autostart=yes; shift ;;
        --uninstall) uninstall=yes; shift ;;
        --help|-h)   usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

bindir="$prefix/bin"
appdir="$prefix/share/applications"
icondir="$prefix/share/icons/hicolor/256x256/apps"
docdir="$prefix/share/doc/$name"
autostartdir="${XDG_CONFIG_HOME:-$HOME/.config}/autostart"

if [ "$uninstall" = yes ]; then
    rm -f "$bindir/$binary"
    rm -f "$appdir/$name.desktop"
    rm -f "$icondir/$name.png" "$icondir/$name-off.png"
    rm -f "$autostartdir/$name.desktop"
    rm -rf "$docdir"
    echo "Removed White Sound from $prefix"
    exit 0
fi

src=$(cd "$(dirname "$0")" && pwd)
if [ ! -f "$src/bin/$binary" ]; then
    echo "run install.sh from inside the extracted tarball" >&2
    exit 1
fi

echo "Installing White Sound under $prefix"
mkdir -p "$bindir" "$appdir" "$icondir" "$docdir"

cp "$src/bin/$binary" "$bindir/$binary"
chmod 755 "$bindir/$binary"

# The desktop entry points at the absolute installed path (the prefix is only
# known here), and the icon is picked up from the hicolor theme by name. Replace
# the Exec line line-by-line rather than with sed, so paths containing &, \ or |
# cannot corrupt the entry.
exe="$bindir/$binary"
while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
        "Exec=@EXEC@") printf 'Exec=%s\n' "$exe" ;;
        *) printf '%s\n' "$line" ;;
    esac
done < "$src/share/applications/$name.desktop" > "$appdir/$name.desktop"
chmod 644 "$appdir/$name.desktop"

cp "$src/share/icons/hicolor/256x256/apps/$name.png"     "$icondir/$name.png"
cp "$src/share/icons/hicolor/256x256/apps/$name-off.png" "$icondir/$name-off.png"

for doc in README.md LICENSE THIRD_PARTY_NOTICES.md; do
    if [ -f "$src/share/doc/$name/$doc" ]; then
        cp "$src/share/doc/$name/$doc" "$docdir/$doc"
    fi
done

if [ "$autostart" = yes ]; then
    mkdir -p "$autostartdir"
    cp "$appdir/$name.desktop" "$autostartdir/$name.desktop"
    echo "Autostart entry: $autostartdir/$name.desktop"
fi

# Refresh the caches so the entry and icon show up without a re-login.
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$appdir" >/dev/null 2>&1 || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t -f "$prefix/share/icons/hicolor" >/dev/null 2>&1 || true
fi

echo
echo "Installed:"
echo "  $exe"
echo "  $appdir/$name.desktop"
# shellcheck disable=SC2016  # single quotes are literal here on purpose
echo "Remove with: sh install.sh --uninstall --prefix '$prefix'"

case ":$PATH:" in
    *":$bindir:"*) ;;
    *) echo
       echo "Note: $bindir is not in PATH, so launch it with its full path." ;;
esac
