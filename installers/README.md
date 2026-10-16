# Installers

Per-platform packaging for White Sound. Each platform must be built on its own
operating system; the scripts here only *produce* that platform's package.

| Platform | Script | Output |
| --- | --- | --- |
| Windows | `windows/build_installer.ps1` | `WhiteSoundSetup-<version>.exe` (per-user, no UAC) |
| macOS | `macos/build_app.sh` | `White Sound.app` + `White Sound-<version>.dmg` |
| Linux | `linux/build_tarball.sh` | `white-sound-<version>-linux-<arch>.tar.gz` |

All packages are **unsigned**; Windows SmartScreen and macOS Gatekeeper warn on
first run.

## Windows (Inno Setup 6)

```powershell
powershell -ExecutionPolicy Bypass -File installers\windows\build_installer.ps1
```

Builds the app and compiles `windows/white_sound.iss` with Inno Setup, writing
`WhiteSoundSetup-<version>.exe` to this folder. The install is per-user (no admin
rights, no UAC prompt); it copies the executable and docs to
`%LOCALAPPDATA%\Programs\White Sound`, adds Start Menu / desktop shortcuts and an
optional "start with Windows" entry, and ships an uninstaller. Configuration in
`%APPDATA%\white-sound` is left untouched.

Requires Inno Setup 6 (`winget install JRSoftware.InnoSetup`). The version is
read from `build\generated\version.iss`, which CMake generates from the
`project(... VERSION ...)` line.

## macOS (`.app` + `.dmg`)

```sh
sh installers/macos/build_app.sh
```

Builds the app and assembles `White Sound.app` (executable in `Contents/MacOS`,
the tray PNGs and a generated `.icns` in `Contents/Resources`, and an
`Info.plist` with `LSUIElement` so it is a tray-only agent) plus a
`White Sound-<version>.dmg` with an `/Applications` shortcut for drag-to-install.
The bundle is only ad-hoc signed, so the first launch needs a **right-click →
Open**.

Building macOS packages requires macOS. If you do not have a Mac, the
[GitHub Actions workflow](../.github/workflows/build-installers.yml) builds the
`.dmg` (as a universal binary, arm64 + x86_64) and publishes it as a downloadable
artifact — see [Building in the cloud](#building-in-the-cloud).

## Linux (portable tarball)

```sh
sh installers/linux/build_tarball.sh
```

Produces `white-sound-<version>-linux-<arch>.tar.gz` containing `bin/`,
`share/` (a `.desktop` entry and the hicolor theme icons) and an `install.sh`.
To install:

```sh
tar xzf white-sound-<version>-linux-<arch>.tar.gz
sh white-sound-<version>-linux-<arch>/install.sh          # into ~/.local
sh white-sound-<version>-linux-<arch>/install.sh --system # into /usr/local
```

`install.sh` also takes `--autostart` (writes an `~/.config/autostart` entry) and
`--uninstall`. Installing the icon into the theme is what lets the desktop's tray
host resolve the `IconName` the StatusNotifierItem advertises.

## Building in the cloud

`.github/workflows/build-installers.yml` builds all three installers on GitHub's
runners (Windows with MinGW-w64, macOS universal, Linux):

**On demand** — Actions → Build installers → Run workflow, then download the
`white-sound-windows`, `white-sound-macos` and `white-sound-linux` artifacts.
These are temporary (they expire).

**As a release** — push a version tag and the installers are attached to a GitHub
Release, which is the permanent, public download page:

```sh
git tag v1.0.0
git push origin v1.0.0
```

The tag name is free-form; keep it in step with the `project(... VERSION ...)`
line in `CMakeLists.txt` (e.g. `v1.0.0` for version `1.0.0`). Re-running on the
same tag updates the existing release. Nothing is committed back to the
repository.
